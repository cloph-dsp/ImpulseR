#include "IRProcessor.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "IRProcessor", __VA_ARGS__)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace impulser {

IRProcessor::IRProcessor(int sampleRate)
    : mSampleRate(sampleRate)
{
    if (sampleRate <= 0) {
        throw std::invalid_argument("Sample rate must be positive");
    }
}

bool IRProcessor::process(const float* input, int inputLen, float** output, int* outputLen) {
    if (!input || inputLen <= 0 || !output || !outputLen) {
        return false;
    }

    // Find peak
    mPeakPosition = findPeak(input, inputLen);
    mPeakAmplitude = std::abs(input[mPeakPosition]);

    // Find noise floor
    mNoiseFloorPosition = findNoiseFloor(input, inputLen, mPeakAmplitude);

    // Determine trim positions
    int trimStart = mTrimStart >= 0 ? mTrimStart : 
                    std::max(0, mPeakPosition - static_cast<int>(kPreDelay * mSampleRate));
    
    int trimEnd = mTrimEnd >= 0 ? mNoiseFloorPosition + mTrimEnd :
                  mNoiseFloorPosition + static_cast<int>(0.1f * mSampleRate); // 100ms tail

    // Clamp trim positions
    trimStart = std::max(0, std::min(trimStart, inputLen - 1));
    trimEnd = std::max(trimStart + 1, std::min(trimEnd, inputLen));

    // Allocate output buffer
    int outputLength = trimEnd - trimStart;
    *output = new float[outputLength];
    if (!*output) {
        LOGE("Failed to allocate output buffer");
        *outputLen = 0;
        return false;
    }
    *outputLen = outputLength;

    // Copy trimmed IR
    std::memcpy(*output, input + trimStart, outputLength * sizeof(float));

    // Apply half-Hann window to tail
    int noiseFloorInOutput = mNoiseFloorPosition - trimStart;
    if (noiseFloorInOutput > 0 && noiseFloorInOutput < outputLength) {
        applyHalfHannWindow(*output, outputLength, noiseFloorInOutput);
    }

    // Normalize to -3 dBFS
    normalize(*output, outputLength);

    return true;
}

int IRProcessor::findPeak(const float* input, int len) const {
    int peakIdx = 0;
    float peakVal = 0.0f;

    for (int i = 0; i < len; ++i) {
        float absVal = std::abs(input[i]);
        if (absVal > peakVal) {
            peakVal = absVal;
            peakIdx = i;
        }
    }

    return peakIdx;
}

int IRProcessor::findNoiseFloor(const float* input, int len, float peakAmplitude) const {
    // Calculate threshold in linear scale
    float threshold = peakAmplitude * std::pow(10.0f, kNoiseFloorThreshold / 20.0f);
    
    // RMS window length in samples
    int rmsWindowSamples = static_cast<int>(kRMSWindowMs * mSampleRate / 1000.0f);
    
    // Scan backward from the end
    for (int i = len - rmsWindowSamples; i >= 0; --i) {
        float rms = calculateRMS(input, i, rmsWindowSamples, len);
        
        if (rms < threshold) {
            // Found noise floor
            return i;
        }
    }

    // If no noise floor found, return 90% of the length
    return static_cast<int>(len * 0.9f);
}

void IRProcessor::applyHalfHannWindow(float* buffer, int len, int noiseFloorPos) const {
    // Half-Hann window: w[n] = 0.5 * (1 + cos(π * (n - noiseFloorPos) / (len - noiseFloorPos)))
    // This analytically reaches exactly 0.0f at the end
    
    int windowLen = len - noiseFloorPos;
    if (windowLen <= 0) {
        return;
    }

    for (int i = noiseFloorPos; i < len; ++i) {
        float t = static_cast<float>(i - noiseFloorPos) / windowLen;
        float w = 0.5f * (1.0f + std::cos(M_PI * t));
        buffer[i] *= w;
    }

    // Ensure the last sample is exactly 0.0f
    buffer[len - 1] = 0.0f;
}

void IRProcessor::normalize(float* buffer, int len) const {
    // Find peak amplitude
    float peak = 0.0f;
    for (int i = 0; i < len; ++i) {
        float absVal = std::abs(buffer[i]);
        if (absVal > peak) {
            peak = absVal;
        }
    }

    if (peak < 1e-10f) {
        // Avoid division by zero
        return;
    }

    // Scale to target peak (-3 dBFS = 0.7079)
    float scale = kTargetPeak / peak;
    for (int i = 0; i < len; ++i) {
        buffer[i] *= scale;
    }
}

float IRProcessor::calculateRMS(const float* buffer, int start, int windowLen, int totalLen) const {
    if (start < 0 || start + windowLen > totalLen) {
        return 0.0f;
    }

    float sumSquares = 0.0f;
    for (int i = start; i < start + windowLen; ++i) {
        sumSquares += buffer[i] * buffer[i];
    }

    return std::sqrt(sumSquares / windowLen);
}

} // namespace impulser
