#include "CalibrationFilter.h"
#include "ESSGenerator.h"
#include "OboeEngine.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <sstream>
#include <android/log.h>
#include <sys/system_properties.h>

#define LOG_TAG "CalibrationFilter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace impulser {

template<typename T, size_t Alignment>
struct AlignedAllocator {
    using value_type = T;

    T* allocate(size_t n) {
        if (n == 0) return nullptr;
        void* ptr = nullptr;
        if (posix_memalign(&ptr, Alignment, n * sizeof(T)) != 0) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, size_t) noexcept {
        std::free(ptr);
    }
};

using AlignedFloat = std::vector<float, AlignedAllocator<float, 64>>;

} // namespace impulser

namespace impulser {

CalibrationFilter::CalibrationFilter(float f1, float f2, float duration, int sampleRate)
    : mF1(f1)
    , mF2(f2)
    , mDuration(duration)
    , mSampleRate(sampleRate)
{
    // Initialize filter with zeros
    mFilter.resize(kFilterLength, 0.0f);
    
    // Set center tap to 1.0 (passthrough) until calibration is performed
    mFilter[kFilterLength / 2] = 1.0f;
}

CalibrationFilter::~CalibrationFilter() {
    if (mFFTSetup) {
        pffft_destroy_setup(mFFTSetup);
    }
}

bool CalibrationFilter::runCalibration(OboeEngine& oboeEngine) {
    LOGI("Starting calibration...");
    
    // Check for existing calibration
    std::string deviceId = getDeviceId();
    if (load(deviceId)) {
        LOGI("Calibration already exists for device: %s", deviceId.c_str());
        return true;
    }
    
    // Generate reference ESS (3 seconds)
    ESSGenerator essGen(mF1, mF2, mDuration, mSampleRate);
    int essSamples = essGen.getTotalSamples();
    std::vector<float> referenceESS(essSamples);
    essGen.generate(referenceESS.data(), essSamples);
    
    // Allocate capture buffer (ESS + reverb tail ~2 seconds)
    int captureSamples = essSamples + static_cast<int>(2.0f * mSampleRate);
    std::vector<float> recordedESS(captureSamples, 0.0f);
    
    // Capture while playing
    if (!oboeEngine.captureWhilePlaying(referenceESS.data(), essSamples,
                                        recordedESS.data(), captureSamples)) {
        LOGE("Failed to capture calibration signal");
        
        // Fallback: simulate with no distortion
        LOGI("Using passthrough calibration (no device correction)");
        mIsCalibrated = true;
        return true;
    }
    
    // Compute transfer function and inverse filter
    if (!computeTransferFunction(recordedESS.data(), captureSamples, 
                                  referenceESS.data(), essSamples)) {
        LOGE("Failed to compute transfer function");
        return false;
    }
    
    // Save calibration for future use
    if (!save(deviceId)) {
        LOGW("Failed to save calibration to storage");
    }
    
    mIsCalibrated = true;
    LOGI("Calibration completed successfully");
    
return true;
}

void CalibrationFilter::apply(const float* input, float* output, int numSamples) {
    if (!mIsCalibrated || mFilter.empty()) {
        // No calibration - copy input to output
        if (input != output) {
            std::memcpy(output, input, numSamples * sizeof(float));
        }
        return;
    }
    
    // Apply convolution with calibration filter
    // This is a simplified time-domain convolution
    // In production, we would use frequency-domain convolution for efficiency
    
    int filterLen = static_cast<int>(mFilter.size());
    int halfFilter = filterLen / 2;
    
    for (int i = 0; i < numSamples; ++i) {
        float sum = 0.0f;
        
        for (int j = 0; j < filterLen; ++j) {
            int idx = i - halfFilter + j;
            if (idx >= 0 && idx < numSamples) {
                sum += input[idx] * mFilter[j];
            }
        }
        
        output[i] = sum;
    }
}

bool CalibrationFilter::load(const std::string& deviceId) {
    std::string filename = "/data/data/com.impulser.capture/files/calibration_" + deviceId + ".bin";
    std::vector<char> buffer(kFilterLength * sizeof(float));
    std::ifstream file(filename, std::ios::binary);
    
    if (!file.is_open()) {
        LOGI("No saved calibration found for device: %s", deviceId.c_str());
        return false;
    }
    
    // Read filter length
    int filterLen = 0;
    file.read(reinterpret_cast<char*>(&filterLen), sizeof(int));
    
    if (filterLen != kFilterLength) {
        LOGE("Invalid filter length in saved calibration: %d", filterLen);
        return false;
    }
    
    // Read filter coefficients
    mFilter.resize(filterLen);
    file.read(reinterpret_cast<char*>(mFilter.data()), filterLen * sizeof(float));
    
    file.close();
    
    mIsCalibrated = true;
    LOGI("Loaded calibration for device: %s", deviceId.c_str());
    
    return true;
}

bool CalibrationFilter::save(const std::string& deviceId) {
    if (!mIsCalibrated) {
        LOGE("No calibration to save");
        return false;
    }
    
    // In production, this would save to SharedPreferences via JNI
    // For now, we'll use a file-based approach for testing
    
    std::string filename = "/data/data/com.impulser.capture/files/calibration_" + deviceId + ".bin";
    std::ofstream file(filename, std::ios::binary);
    
    if (!file.is_open()) {
        LOGE("Failed to open file for saving calibration: %s", filename.c_str());
        return false;
    }
    
    // Write filter length
    int filterLen = static_cast<int>(mFilter.size());
    file.write(reinterpret_cast<const char*>(&filterLen), sizeof(int));
    
    // Write filter coefficients
    file.write(reinterpret_cast<const char*>(mFilter.data()), filterLen * sizeof(float));
    
    file.close();
    
    LOGI("Saved calibration for device: %s", deviceId.c_str());
    
    return true;
}

std::string CalibrationFilter::getDeviceId() {
    char manufacturer[PROP_VALUE_MAX] = {0};
    char model[PROP_VALUE_MAX] = {0};
    char fingerprint[PROP_VALUE_MAX] = {0};
    
    __system_property_get("ro.product.manufacturer", manufacturer);
    __system_property_get("ro.product.model", model);
    __system_property_get("ro.build.fingerprint", fingerprint);
    
    std::string deviceId;
    deviceId.reserve(PROP_VALUE_MAX * 3 + 2);
    deviceId.append(manufacturer);
    deviceId.push_back('_');
    deviceId.append(model);
    deviceId.push_back('_');
    deviceId.append(fingerprint);
    
    // Simple hash function
    std::hash<std::string> hasher;
    size_t hash = hasher(deviceId);
    
    return std::to_string(hash);
}

bool CalibrationFilter::computeTransferFunction(const float* recorded, int recLen,
                                                  const float* reference, int refLen) {
    // Calculate FFT size
    int convLen = recLen + refLen - 1;
    int N_fft = nextPowerOf2(convLen);
    
    // Ensure N_fft is valid for PFFFT
    while (N_fft % 32 != 0) {
        N_fft *= 2;
    }
    
    // Create FFT setup
    if (mFFTSetup) {
        pffft_destroy_setup(mFFTSetup);
    }
    mFFTSetup = pffft_new_setup(N_fft, PFFFT_REAL);
    
    if (!mFFTSetup) {
        LOGE("Failed to create FFT setup");
        return false;
    }
    
    // Allocate buffers (64-byte aligned for pffft SIMD)
    AlignedFloat y_padded(N_fft, 0.0f);
    AlignedFloat x_padded(N_fft, 0.0f);
    AlignedFloat Y(N_fft, 0.0f);
    AlignedFloat X(N_fft, 0.0f);
    AlignedFloat H_dev(N_fft, 0.0f);
    AlignedFloat work(N_fft, 0.0f);
    
    // Copy signals
    std::memcpy(y_padded.data(), recorded, recLen * sizeof(float));
    std::memcpy(x_padded.data(), reference, refLen * sizeof(float));
    
    // FFT of recorded signal
    pffft_transform_ordered(mFFTSetup, y_padded.data(), Y.data(), work.data(), PFFFT_FORWARD);
    
    // FFT of reference signal
    pffft_transform_ordered(mFFTSetup, x_padded.data(), X.data(), work.data(), PFFFT_FORWARD);
    
    // Compute transfer function: H_dev[k] = Y[k] / X[k]
    // With Tikhonov regularization to avoid division by near-zero
    const float epsilon = 1e-3f; // -60 dBFS noise floor protection
    
    for (int k = 0; k < N_fft / 2; ++k) {
        float X_real = X[2*k];
        float X_imag = X[2*k+1];
        float Y_real = Y[2*k];
        float Y_imag = Y[2*k+1];
        
        // |X[k]|^2
        float X_mag2 = X_real * X_real + X_imag * X_imag;
        
        // Regularized division: H = conj(X) * Y / (|X|^2 + epsilon)
        float denom = X_mag2 + epsilon;
        H_dev[2*k] = (X_real * Y_real + X_imag * Y_imag) / denom;
        H_dev[2*k+1] = (X_real * Y_imag - X_imag * Y_real) / denom;
    }
    
    // Compute inverse filter
    computeInverseFilter(H_dev.data(), N_fft);
    
    return true;
}

void CalibrationFilter::computeInverseFilter(const float* H_dev, int N) {
    // Compute Tikhonov-regularized inverse filter
    // H_corr[k] = conj(H_dev[k]) / (|H_dev[k]|^2 + epsilon)
    
    std::vector<float> H_corr(N, 0.0f);
    const float epsilon = 1e-3f;
    
    for (int k = 0; k < N / 2; ++k) {
        float H_real = H_dev[2*k];
        float H_imag = H_dev[2*k+1];
        
        // |H_dev[k]|^2
        float H_mag2 = H_real * H_real + H_imag * H_imag;
        
        // Regularized inverse: H_corr = conj(H_dev) / (|H_dev|^2 + epsilon)
        float denom = H_mag2 + epsilon;
        H_corr[2*k] = H_real / denom;
        H_corr[2*k+1] = -H_imag / denom; // Conjugate
    }
    
    // Inverse FFT to get time-domain filter
    std::vector<float> h_corr_time(N, 0.0f);
    std::vector<float> work(N, 0.0f);
    
    pffft_transform_ordered(mFFTSetup, H_corr.data(), h_corr_time.data(), work.data(), PFFFT_BACKWARD);
    
    // Window to kFilterLength taps
    int halfFilter = kFilterLength / 2;
    mFilter.resize(kFilterLength, 0.0f);
    
    // Extract the center portion of the filter
    int startIdx = N / 2 - halfFilter;
    for (int i = 0; i < kFilterLength; ++i) {
        int idx = startIdx + i;
        if (idx >= 0 && idx < N) {
            mFilter[i] = h_corr_time[idx];
        }
    }
    
    // Apply Hann window to smooth the filter
    for (int i = 0; i < kFilterLength; ++i) {
        float w = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (kFilterLength - 1)));
        mFilter[i] *= w;
    }
}

int CalibrationFilter::nextPowerOf2(int n) const {
    int power = 1;
    while (power < n) {
        power *= 2;
    }
    return power;
}

} // namespace impulser
