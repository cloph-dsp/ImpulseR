#include "Deconvolver.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <complex>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Deconvolver", __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

/**
 * Cooley-Tukey radix-2 in-place FFT.
 * N must be a power of 2.
 */
void fft_inplace(std::complex<float>* a, int N) {
    if (N <= 1) return;

    // Bit-reversal permutation
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

    // Cooley-Tukey iterative FFT
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

inline void fft_inplace(std::vector<std::complex<float>>& a) {
    fft_inplace(a.data(), static_cast<int>(a.size()));
}

/**
 * In-place inverse FFT: conjugate, fft, conjugate, divide by N.
 */
void ifft_inplace(std::vector<std::complex<float>>& a) {
    const int N = static_cast<int>(a.size());
    if (N <= 1) return;

    // Conjugate
    for (auto& x : a) {
        x = std::conj(x);
    }

    // Forward FFT
    fft_inplace(a);

    // Conjugate and divide by N
    for (auto& x : a) {
        x = std::conj(x) / static_cast<float>(N);
    }
}

} // anonymous namespace

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
    // No external resources to clean up
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

    // Allocate complex buffers for frequency-domain processing
    std::vector<std::complex<float>> Y(N_fft);
    std::vector<std::complex<float>> H_inv(N_fft);
    std::vector<std::complex<float>> IR_freq(N_fft);
    std::vector<float> ir_full(N_fft);

    // Copy recorded signal with latency compensation into complex buffer
    int delay = mRoundTripDelaySamples;
    if (delay > 0 && delay < recLen) {
        for (int i = 0; i < recLen - delay; ++i) {
            Y[i] = recorded[delay + i];
        }
        for (int i = recLen - delay; i < N_fft; ++i) {
            Y[i] = 0.0f;
        }
    } else {
        for (int i = 0; i < recLen; ++i) {
            Y[i] = recorded[i];
        }
        for (int i = recLen; i < N_fft; ++i) {
            Y[i] = 0.0f;
        }
    }

    // Copy inverse filter into complex buffer
    for (int i = 0; i < inverseFilterLen; ++i) {
        H_inv[i] = inverseFilter[i];
    }
    for (int i = inverseFilterLen; i < N_fft; ++i) {
        H_inv[i] = 0.0f;
    }

    // Forward FFT
    fft_inplace(Y);
    fft_inplace(H_inv);

    // Apply calibration filter in frequency domain
    if (mHasCalibrationFilter && !mCalibrationFilter.empty()) {
        applyCalibrationFilter(Y.data(), N_fft);
    }

    // Point-wise multiply: IR_freq = Y * H_inv
    for (int k = 0; k < N_fft; ++k) {
        IR_freq[k] = Y[k] * H_inv[k];
    }

    // Inverse FFT
    ifft_inplace(IR_freq);

    // Extract real part to ir_full buffer
    for (int i = 0; i < N_fft; ++i) {
        ir_full[i] = IR_freq[i].real();
    }

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

void Deconvolver::applyCalibrationFilter(std::complex<float>* spectrum, int N) {
    if (mCalibrationFilter.empty() || N <= 0) return;
    if (mCalibrationFilterN != N || mCalibrationFilterFreq.size() != static_cast<size_t>(N)) {
        mCalibrationFilterFreq.assign(N, std::complex<float>(0.0f, 0.0f));
        int taps = static_cast<int>(mCalibrationFilter.size());
        int copy = std::min(taps, N);
        for (int i = 0; i < copy; ++i) {
            mCalibrationFilterFreq[i] = std::complex<float>(mCalibrationFilter[i], 0.0f);
        }
        fft_inplace(mCalibrationFilterFreq.data(), N);
        mCalibrationFilterN = N;
    }
    for (int k = 0; k < N; ++k) {
        spectrum[k] *= mCalibrationFilterFreq[k];
    }
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
    
    // --- Multi-notch harmonic isolation: remove harmonics 2–5 of the speaker's distortion ---
    // Work on a mutable copy of the full IR
    std::vector<float> irClean(irFull, irFull + N_fft);

    for (int harmonic = 2; harmonic <= 5; ++harmonic) {
        int tau_n = static_cast<int>(tau_2 * harmonic);
        if (tau_n >= N_fft - 5280) continue; // skip if window would exceed IR length
        int windowStart = std::max(0, tau_n - 480);
        int windowEnd   = std::min(N_fft, tau_n + 4800);

        // Hann taper at window leading edge (240 samples = 5 ms at 48 kHz)
        for (int i = 0; i < 240 && (windowStart + i) < windowEnd; ++i) {
            float w = 0.5f * (1.0f - std::cos(M_PI * i / 240.0f));
            irClean[windowStart + i] *= w;
        }
        // Hann taper at window trailing edge
        for (int i = 0; i < 240 && (windowEnd - 1 - i) >= windowStart; ++i) {
            float w = 0.5f * (1.0f - std::cos(M_PI * i / 240.0f));
            irClean[windowEnd - 1 - i] *= w;
        }
        // Zero the body (after taper ramps) to remove harmonic contribution
        for (int i = windowStart + 240; i < windowEnd - 240; ++i) {
            irClean[i] = 0.0f;
        }
    }

    // Calculate window boundaries for final IR extraction
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

    // Copy isolated IR (with Hann fade-in/fade-out baked into the window boundaries)
    std::memcpy(*irOut, irClean.data() + startSample, irLength * sizeof(float));
    
    return true;
}

} // namespace impulser
