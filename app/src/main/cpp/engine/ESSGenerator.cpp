#include "ESSGenerator.h"
#include <algorithm>
#include <stdexcept>
#include <android/log.h>

#ifndef LOGW
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ESSGenerator", __VA_ARGS__)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace impulser {

ESSGenerator::ESSGenerator(float f1, float f2, float duration, int sampleRate)
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

    // Anti-aliasing guard: cap f2 to avoid DAC non-linearities aliasing above fs/2
    double f2Max = static_cast<double>(sampleRate) * 0.45;
    if (static_cast<double>(mF2) > f2Max) {
        LOGW("ESSGenerator: f2=%.1f capped to %.1f (anti-aliasing guard)", mF2, f2Max);
        mF2 = static_cast<float>(f2Max);
    }

    // Calculate sweep rate parameter L = T / log(f2/f1)
    mL = mDuration / std::log(mF2 / mF1);
    
    // Calculate total number of samples
    mTotalSamples = static_cast<int>(mDuration * mSampleRate);
}

void ESSGenerator::generate(float* out, int numSamples) {
    if (!out || numSamples <= 0) {
        return;
    }

    // Generate ESS signal
    // x[n] = sin(2π · f1 · L · (exp(n / (fs·L)) - 1))
    const float twoPiF1L = 2.0f * M_PI * mF1 * mL;
    const float invFsL = 1.0f / (mSampleRate * mL);

    for (int n = 0; n < numSamples; ++n) {
        float t = static_cast<float>(n) / mSampleRate;
        float exponent = n * invFsL;
        float phase = twoPiF1L * (std::exp(exponent) - 1.0f);
        out[n] = std::sin(phase);
    }

    // Apply fade-in (20ms raised-cosine)
    int fadeSamples = static_cast<int>(kFadeDuration * mSampleRate);
    applyFadeIn(out, numSamples, fadeSamples);

    // Apply fade-out (20ms raised-cosine)
    applyFadeOut(out, numSamples, fadeSamples);
}

void ESSGenerator::applyFadeIn(float* buffer, int numSamples, int fadeSamples) {
    if (fadeSamples <= 0 || fadeSamples > numSamples) {
        return;
    }

    // Raised-cosine fade-in: w[n] = 0.5 * (1 - cos(π * n / fadeSamples))
    for (int n = 0; n < fadeSamples; ++n) {
        float w = 0.5f * (1.0f - std::cos(M_PI * n / fadeSamples));
        buffer[n] *= w;
    }
}

void ESSGenerator::applyFadeOut(float* buffer, int numSamples, int fadeSamples) {
    if (fadeSamples <= 0 || fadeSamples > numSamples) {
        return;
    }

    // Raised-cosine fade-out: w[n] = 0.5 * (1 + cos(π * (n - (numSamples - fadeSamples)) / fadeSamples))
    int startSample = numSamples - fadeSamples;
    for (int n = startSample; n < numSamples; ++n) {
        float w = 0.5f * (1.0f + std::cos(M_PI * (n - startSample) / fadeSamples));
        buffer[n] *= w;
    }
}

} // namespace impulser
