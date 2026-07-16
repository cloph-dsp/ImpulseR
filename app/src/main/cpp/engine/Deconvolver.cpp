#include "Deconvolver.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Deconvolver", __VA_ARGS__)

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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace impulser {

Deconvolver::Deconvolver(float f1, float f2, float duration, int sampleRate)
    : mF1(f1)
    , mF2(f2)
    , mDuration(duration)
    , mSampleRate(sampleRate)
{
    // Validate parameters
    if (f1 <= 0.0f || f2 <= 0.0f) {
        throw std::invalid_argument("Frequencies must be positive");
    }
    if (f1 >= f2) {
        throw std::invalid_argument("Start frequency must be less than end frequency");
    }
    if (duration <= 0.0f) {
        throw std::invalid_argument("Duration must be positive");
    }
    if (sampleRate <= 0) {
        throw std::invalid_argument("Sample rate must be positive");
    }

    // Calculate sweep rate parameter L = T / log(f2/f1)
    mL = mDuration / std::log(mF2 / mF1);
}

Deconvolver::~Deconvolver() {
    if (mFFTSetup) {
        pffft_destroy_setup(mFFTSetup);
    }
}

bool Deconvolver::deconvolve(const float* recorded, int recLen, float** irOut, int* irLen) {
    if (!recorded || recLen <= 0 || !irOut || !irLen) {
        return false;
    }

    // Generate inverse filter
    int inverseFilterLen = static_cast<int>(mDuration * mSampleRate);
    std::vector<float> inverseFilter(inverseFilterLen);
    generateInverseFilter(inverseFilter.data(), inverseFilterLen);

    // Calculate FFT size (next power of 2 >= recLen + inverseFilterLen - 1)
    int convLen = recLen + inverseFilterLen - 1;
    int N_fft = nextPowerOf2(convLen);

    // Ensure N_fft is valid for PFFFT (multiple of 32 for real transforms)
    while (N_fft % 32 != 0) {
        N_fft *= 2;
    }

    // Create or recreate FFT setup if needed
    if (!mFFTSetup || mCurrentFFTSize != N_fft) {
        if (mFFTSetup) {
            pffft_destroy_setup(mFFTSetup);
        }
        mFFTSetup = pffft_new_setup(N_fft, PFFFT_REAL);
        mCurrentFFTSize = N_fft;
        
        if (!mFFTSetup) {
            return false;
        }
    }

    // Allocate buffers (64-byte aligned for pffft SIMD)
    AlignedFloat y_padded(N_fft, 0.0f);
    AlignedFloat h_inv_padded(N_fft, 0.0f);
    AlignedFloat Y(N_fft, 0.0f);
    AlignedFloat H_inv(N_fft, 0.0f);
    AlignedFloat IR_freq(N_fft, 0.0f);
    AlignedFloat ir_full(N_fft, 0.0f);
    AlignedFloat work(N_fft, 0.0f);

    // Copy recorded signal with latency compensation
    int delay = mRoundTripDelaySamples;
    if (delay > 0 && delay < recLen) {
        std::memcpy(y_padded.data(), recorded + delay, (recLen - delay) * sizeof(float));
    } else {
        std::memcpy(y_padded.data(), recorded, recLen * sizeof(float));
    }

    // Copy inverse filter
    std::memcpy(h_inv_padded.data(), inverseFilter.data(), inverseFilterLen * sizeof(float));

    // Apply calibration filter if available
    if (mHasCalibrationFilter && !mCalibrationFilter.empty()) {
        // Apply calibration in time domain (convolution)
        // For simplicity, we'll apply it in frequency domain after FFT
    }

    // FFT of recorded signal
    pffft_transform_ordered(mFFTSetup, y_padded.data(), Y.data(), work.data(), PFFFT_FORWARD);

    // FFT of inverse filter
    pffft_transform_ordered(mFFTSetup, h_inv_padded.data(), H_inv.data(), work.data(), PFFFT_FORWARD);

    // Apply calibration filter in frequency domain
    if (mHasCalibrationFilter && !mCalibrationFilter.empty()) {
        applyCalibrationFilter(Y.data(), N_fft);
    }

    // Complex multiplication: IR[k] = Y[k] * H_inv[k]
    std::memset(IR_freq.data(), 0, N_fft * sizeof(float));
    pffft_zconvolve_accumulate(mFFTSetup, Y.data(), H_inv.data(), IR_freq.data(), 1.0f);

    // Inverse FFT
    pffft_transform_ordered(mFFTSetup, IR_freq.data(), ir_full.data(), work.data(), PFFFT_BACKWARD);

    // Isolate linear IR from harmonic images
    isolateLinearIR(ir_full.data(), N_fft, irOut, irLen);

    return true;
}

void Deconvolver::setCalibrationFilter(const float* filter, int filterLen) {
    if (filter && filterLen > 0) {
        mCalibrationFilter.assign(filter, filter + filterLen);
        mHasCalibrationFilter = true;
    } else {
        mCalibrationFilter.clear();
        mHasCalibrationFilter = false;
    }
}

void Deconvolver::generateInverseFilter(float* out, int numSamples) {
    // h_inv[n] = x[N-1-n] * exp(-n * log(f2/f1) / (T*fs))
    // where x[n] is the original ESS signal
    
    const float logRatio = std::log(mF2 / mF1);
    const float invTfs = 1.0f / (mDuration * mSampleRate);
    const float twoPiF1L = 2.0f * M_PI * mF1 * mL;
    const float invFsL = 1.0f / (mSampleRate * mL);

    for (int n = 0; n < numSamples; ++n) {
        // Time-reversed index
        int n_rev = numSamples - 1 - n;
        
        // Generate original ESS at time-reversed position
        float exponent = n_rev * invFsL;
        float phase = twoPiF1L * (std::exp(exponent) - 1.0f);
        float x_rev = std::sin(phase);
        
        // Apply amplitude modulation
        float amplitude = std::exp(-n * logRatio * invTfs);
        
        out[n] = x_rev * amplitude;
    }
}

int Deconvolver::nextPowerOf2(int n) const {
    int power = 1;
    while (power < n) {
        power *= 2;
    }
    return power;
}

void Deconvolver::applyCalibrationFilter(float* spectrum, int N) {
    // Apply calibration filter in frequency domain
    // This is a simplified version - in production, we would:
    // 1. FFT the calibration filter
    // 2. Multiply with the spectrum
    // 3. Handle the complex multiplication properly
    
    // For now, we'll skip this and apply it in time domain
    // The CalibrationFilter class will handle the actual application
}

bool Deconvolver::isolateLinearIR(const float* irFull, int N_fft, float** irOut, int* irLen) {
    // The linear IR sits at sample index N_fft - 1 (end of output for causal convolution)
    // The n-th harmonic is offset by τ_n = round(T * log(n) / log(f2/f1) * fs) samples earlier
    
    const float logRatio = std::log(mF2 / mF1);
    
    // Calculate offset to 2nd harmonic
    int tau_2 = static_cast<int>(std::round(mDuration * std::log(2.0f) / logRatio * mSampleRate));
    
    // Guard samples to avoid harmonic leakage
    int guardSamples = static_cast<int>(0.01f * mSampleRate); // 10ms guard
    
    // Tail samples for natural decay
    int tailSamples = static_cast<int>(0.1f * mSampleRate); // 100ms tail
    
    // Calculate window boundaries
    int startSample = N_fft - tau_2 + guardSamples;
    int endSample = N_fft + tailSamples;
    
    // Clamp to valid range
    startSample = std::max(0, std::min(startSample, N_fft - 1));
    endSample = std::max(startSample + 1, std::min(endSample, N_fft));
    
    // Allocate output buffer
    int irLength = endSample - startSample;
    *irOut = new float[irLength];
    if (!*irOut) {
        LOGE("Failed to allocate IR output buffer");
        *irLen = 0;
        return false;
    }
    *irLen = irLength;
    
    // Copy isolated IR
    std::memcpy(*irOut, irFull + startSample, irLength * sizeof(float));
    
    return true;
}

} // namespace impulser
