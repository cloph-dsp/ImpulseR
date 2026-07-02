#pragma once

#include <string>
#include <vector>

namespace impulser {

/**
 * WAV file exporter for impulse responses.
 * 
 * Exports IR as standard 48kHz/24-bit mono RIFF/WAV file.
 * No dynamic memory allocation during write - streams directly to FILE*.
 */
class WavExporter {
public:
    WavExporter();
    ~WavExporter();

    /**
     * Export an impulse response to a WAV file.
     * 
     * @param path File path to write
     * @param samples Pointer to sample data
     * @param numSamples Number of samples
     * @param sampleRate Sample rate in Hz
     * @return true if export succeeded
     */
    bool exportWav(const std::string& path, const float* samples, 
                   int numSamples, int sampleRate);

private:
    /**
     * Write WAV file header.
     * 
     * @param file FILE pointer
     * @param numSamples Number of samples
     * @param sampleRate Sample rate in Hz
     */
    void writeHeader(FILE* file, int numSamples, int sampleRate);

    /**
     * Convert float sample to 24-bit PCM.
     * 
     * @param sample Float sample in range [-1.0, 1.0]
     * @return 24-bit signed integer
     */
    int32_t floatTo24Bit(float sample) const;

    /**
     * Write 24-bit value as 3 bytes (little-endian).
     * 
     * @param file FILE pointer
     * @param value 24-bit value
     */
    void write24Bit(FILE* file, int32_t value) const;

    // Constants
    static constexpr int kBitsPerSample = 24;
    static constexpr int kNumChannels = 1; // Mono
    static constexpr int kBlockAlign = kNumChannels * (kBitsPerSample / 8);
};

} // namespace impulser
