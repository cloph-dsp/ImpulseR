#include "CalibrationFilter.h"
#include "ESSGenerator.h"
#include "OboeEngine.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <complex>
#include <fstream>
#include <sstream>
#include <android/log.h>
#include <sys/system_properties.h>

#define LOG_TAG "CalibrationFilter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace impulser {

} // namespace impulser

namespace {

void fft_inplace(std::vector<std::complex<float>>& a) {
    const int N = static_cast<int>(a.size());
    if (N <= 1) return;

    int j = 0;
    for (int i = 0; i < N - 1; ++i) {
        if (i < j) {
            std::swap(a[i], a[j]);
        }
        int k = N;
        do {
            k >>= 1;
        } while (k <= j);
        j = j - k + (k > j ? k : 0);
    }

    for (int len = 2; len <= N; len <<= 1) {
        float angle = -2.0f * static_cast<float>(M_PI) / len;
        std::complex<float> wlen(std::cos(angle), std::sin(angle));
        for (int i = 0; i < N; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int jj = 0; jj < len / 2; ++jj) {
                std::complex<float> u = a[i + jj];
                std::complex<float> t = w * a[i + jj + len / 2];
                a[i + jj] = u + t;
                a[i + jj + len / 2] = u - t;
                w *= wlen;
            }
        }
    }
}

void ifft_inplace(std::vector<std::complex<float>>& a) {
    const int N = static_cast<int>(a.size());
    if (N <= 1) return;

    for (auto& x : a) {
        x = std::conj(x);
    }

    fft_inplace(a);

    for (auto& x : a) {
        x = std::conj(x) / static_cast<float>(N);
    }
}

} // anonymous namespace

namespace impulser {

void CalibrationFilter::computeMagnitudeSpectrum(const float* window, int windowSize,
                                                 float* outBins, int nBins) {
    if (window == nullptr || windowSize <= 0 || outBins == nullptr || nBins <= 0) {
        for (int i = 0; i < nBins; i++) outBins[i] = 0.0f;
        return;
    }

    // Pad to next power of 2
    int N = 1;
    while (N < windowSize) N <<= 1;
    int safeBins = std::min(nBins, N / 2);
    std::vector<std::complex<float>> fftBuffer(N);

    // Copy window to complex buffer
    for (int i = 0; i < windowSize; i++) {
        fftBuffer[i] = std::complex<float>(window[i], 0.0f);
    }
    for (int i = windowSize; i < N; i++) {
        fftBuffer[i] = std::complex<float>(0.0f, 0.0f);
    }

    // Run FFT (use the local fft_inplace from anonymous namespace)
    fft_inplace(fftBuffer);

    // Find max magnitude for normalization
    float maxMag = 1e-6f;
    for (int i = 0; i < safeBins; i++) {
        float mag = std::abs(fftBuffer[i]);
        if (mag > maxMag) maxMag = mag;
    }

    // Linear magnitude normalized to [0, 1] where peak = 1
    for (int i = 0; i < safeBins; i++) {
        outBins[i] = std::abs(fftBuffer[i]) / maxMag;
    }
    // Fill remaining bins with zeros
    for (int i = safeBins; i < nBins; i++) {
        outBins[i] = 0.0f;
    }
}

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
    // No external resources to clean up
}

bool CalibrationFilter::runCalibration(OboeEngine& oboeEngine) {
    LOGI("Starting calibration...");
    
    // Check for existing calibration
    std::string deviceId = getDeviceId();
    if (load(deviceId)) {
        LOGI("Calibration already exists for device: %s", deviceId.c_str());
        return true;
    }
    
    // Generate reference ESS (7 seconds, same duration as measurement sweep)
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
    __system_property_get("ro.product.manufacturer", manufacturer);
    __system_property_get("ro.product.model", model);
    return std::string(manufacturer) + ":" + std::string(model);
}

bool CalibrationFilter::computeTransferFunction(const float* recorded, int recLen,
                                                  const float* reference, int refLen) {
    int convLen = recLen + refLen - 1;
    int N_fft = nextPowerOf2(convLen);

    std::vector<std::complex<float>> Y(N_fft);
    std::vector<std::complex<float>> X(N_fft);

    for (int i = 0; i < recLen; ++i) {
        Y[i] = recorded[i];
    }
    for (int i = recLen; i < N_fft; ++i) {
        Y[i] = 0.0f;
    }

    for (int i = 0; i < refLen; ++i) {
        X[i] = reference[i];
    }
    for (int i = refLen; i < N_fft; ++i) {
        X[i] = 0.0f;
    }

    fft_inplace(Y);
    fft_inplace(X);

    // SNR-adaptive epsilon regularization
    std::vector<float> mag2(N_fft / 2);
    for (int k = 1; k < N_fft / 2; ++k) {
        mag2[k - 1] = std::norm(X[k]);
    }
    int safeLen = N_fft / 2 - 1;
    std::nth_element(mag2.begin(), mag2.begin() + safeLen / 2, mag2.end());
    float bandMedian = mag2[safeLen / 2];
    float eps = std::max(bandMedian * 1e-3f, 1e-12f);
    float killFloor = bandMedian * 1e-6f;

    std::vector<std::complex<float>> H_dev(N_fft);
    for (int k = 0; k < N_fft; ++k) {
        float X_mag2 = std::norm(X[k]);
        if (X_mag2 < killFloor) {
            H_dev[k] = std::complex<float>(0.0f, 0.0f);
        } else {
            float denom = X_mag2 + eps;
            H_dev[k] = std::conj(X[k]) * Y[k] / denom;
        }
    }

    computeInverseFilter(H_dev.data(), N_fft);

    return true;
}

void CalibrationFilter::computeInverseFilter(const std::complex<float>* H_dev, int N) {
    std::vector<std::complex<float>> H_corr(N);

    std::vector<float> mag2(N / 2);
    for (int k = 1; k < N / 2; ++k) {
        mag2[k - 1] = std::norm(H_dev[k]);
    }
    int safeLen = N / 2 - 1;
    std::nth_element(mag2.begin(), mag2.begin() + safeLen / 2, mag2.end());
    float bandMedian = mag2[safeLen / 2];
    float eps = std::max(bandMedian * 1e-3f, 1e-12f);
    float killFloor = bandMedian * 1e-6f;

    for (int k = 0; k < N; ++k) {
        float H_mag2 = std::norm(H_dev[k]);
        if (H_mag2 < killFloor) {
            H_corr[k] = std::complex<float>(0.0f, 0.0f);
        } else {
            float denom = H_mag2 + eps;
            H_corr[k] = std::conj(H_dev[k]) / denom;
        }
    }

    ifft_inplace(H_corr);

    int halfFilter = kFilterLength / 2;
    mFilter.resize(kFilterLength, 0.0f);

    int startIdx = 0;
    for (int i = 0; i < kFilterLength; ++i) {
        int idx = startIdx + i;
        if (idx >= 0 && idx < N) {
            mFilter[i] = H_corr[idx].real();
        }
    }

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
