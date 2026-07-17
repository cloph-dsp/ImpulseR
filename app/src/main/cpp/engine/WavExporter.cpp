#include "WavExporter.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <android/log.h>

#define LOG_TAG "WavExporter"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace impulser {

WavExporter::WavExporter() {
}

WavExporter::~WavExporter() {
}

bool WavExporter::exportWav(const std::string& path, const float* samples,
                             int numSamples, int sampleRate) {
    if (!samples || numSamples <= 0) {
        LOGE("Invalid input parameters");
        return false;
    }

    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        LOGE("Failed to open file: %s", path.c_str());
        return false;
    }

    LOGI("Exporting %d samples to %s at %d Hz", numSamples, path.c_str(), sampleRate);

    // Write header
    writeHeader(file, numSamples, sampleRate);

    // Write sample data
    for (int i = 0; i < numSamples; ++i) {
        int32_t pcm24 = floatTo24Bit(samples[i]);
        write24Bit(file, pcm24);
    }

    fclose(file);

    LOGI("Export successful: %s", path.c_str());
    return true;
}

void WavExporter::writeHeader(FILE* file, int numSamples, int sampleRate) {
    int byteRate = sampleRate * kBlockAlign;
    int dataSize = numSamples * kBlockAlign;
    int fileSize = 36 + dataSize; // 36 = header size - 8 bytes for RIFF header

    // RIFF header
    fwrite("RIFF", 1, 4, file);
    fwrite(&fileSize, 4, 1, file);
    fwrite("WAVE", 1, 4, file);

    // fmt chunk
    fwrite("fmt ", 1, 4, file);
    int fmtChunkSize = 16;
    fwrite(&fmtChunkSize, 4, 1, file);
    
    short audioFormat = 1; // PCM
    fwrite(&audioFormat, 2, 1, file);
    
    short numChannels = kNumChannels;
    fwrite(&numChannels, 2, 1, file);
    
    fwrite(&sampleRate, 4, 1, file);
    fwrite(&byteRate, 4, 1, file);
    
    short blockAlign = kBlockAlign;
    fwrite(&blockAlign, 2, 1, file);
    
    short bitsPerSample = kBitsPerSample;
    fwrite(&bitsPerSample, 2, 1, file);

    // data chunk
    fwrite("data", 1, 4, file);
    fwrite(&dataSize, 4, 1, file);
}

int32_t WavExporter::floatTo24Bit(float sample) const {
    // Soft-limit extremes with tanh to avoid hard-clip distortion
    // For |sample| <= 1.0, tanh(x) ≈ x, so no audible change
    // For |sample| > 1.0, smoothly saturates toward ±1
    if (sample > 1.0f || sample < -1.0f) {
        sample = std::tanh(sample);
    }

    // Convert to 24-bit signed integer
    int32_t pcm = static_cast<int32_t>(sample * 8388607.0f);
    pcm = std::max(-8388608, std::min(8388607, pcm));
    return pcm;
}

void WavExporter::write24Bit(FILE* file, int32_t value) const {
    // Write as 3 bytes (little-endian)
    uint8_t bytes[3];
    bytes[0] = value & 0xFF;
    bytes[1] = (value >> 8) & 0xFF;
    bytes[2] = (value >> 16) & 0xFF;
    
    fwrite(bytes, 1, 3, file);
}

} // namespace impulser
