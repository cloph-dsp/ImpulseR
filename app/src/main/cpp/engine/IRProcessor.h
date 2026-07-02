#pragma once

#include <vector>
#include <functional>

namespace impulser {

/**
 * Impulse response post-processor.
 * 
 * Handles onset detection, precise trimming, half-Hann tail windowing,
 * and normalization to produce a clean, production-ready impulse response.
 */
class IRProcessor {
public:
    /**
     * Construct an IR processor with specified sample rate.
     * 
     * @param sampleRate Sample rate in Hz (default: 48000)
     */
    explicit IRProcessor(int sampleRate = 48000);

    /**
     * Process a raw impulse response.
     * 
     * @param input Raw IR from deconvolution
     * @param inputLen Length of input IR
     * @param output Processed IR (allocated by this function)
     * @param outputLen Length of output IR
     * @return true if processing succeeded
     */
    bool process(const float* input, int inputLen, float** output, int* outputLen);

    /**
     * Set custom trim start position.
     * 
     * @param samples Number of samples to trim from start
     */
    void setTrimStart(int samples) { mTrimStart = samples; }

    /**
     * Set custom trim end position.
     * 
     * @param samples Number of samples to trim from end
     */
    void setTrimEnd(int samples) { mTrimEnd = samples; }

    /**
     * Get the detected peak position.
     * 
     * @return Peak sample index
     */
    int getPeakPosition() const { return mPeakPosition; }

    /**
     * Get the peak amplitude.
     * 
     * @return Peak amplitude
     */
    float getPeakAmplitude() const { return mPeakAmplitude; }

    /**
     * Get the noise floor position.
     * 
     * @return Noise floor sample index
     */
    int getNoiseFloorPosition() const { return mNoiseFloorPosition; }

    /**
     * Get the sample rate.
     * 
     * @return Sample rate in Hz
     */
    int getSampleRate() const { return mSampleRate; }

private:
    /**
     * Find the peak in the impulse response.
     * 
     * @param input Input IR
     * @param len Length of IR
     * @return Peak sample index
     */
    int findPeak(const float* input, int len) const;

    /**
     * Find the noise floor position.
     * 
     * @param input Input IR
     * @param len Length of IR
     * @param peakAmplitude Peak amplitude
     * @return Noise floor sample index
     */
    int findNoiseFloor(const float* input, int len, float peakAmplitude) const;

    /**
     * Apply half-Hann window to the tail.
     * 
     * @param buffer IR buffer
     * @param len Total length
     * @param noiseFloorPos Noise floor position
     */
    void applyHalfHannWindow(float* buffer, int len, int noiseFloorPos) const;

    /**
     * Normalize the IR to -3 dBFS.
     * 
     * @param buffer IR buffer
     * @param len Length of IR
     */
    void normalize(float* buffer, int len) const;

/**
      * Calculate RMS over a window.
      * 
      * @param buffer IR buffer
      * @param start Start index
      * @param windowLen Window length
      * @param totalLen Total buffer length
      * @return RMS value
      */
    float calculateRMS(const float* buffer, int start, int windowLen, int totalLen) const;

    // Parameters
    int mSampleRate;
    
    // Processing parameters
    static constexpr float kPreDelay = 0.050f; // 50ms pre-delay
    static constexpr float kNoiseFloorThreshold = -60.0f; // -60 dBFS
    static constexpr float kRMSThreshold = -60.0f; // -60 dBFS
    static constexpr int kRMSWindowMs = 10; // 10ms RMS window
    static constexpr float kTargetPeak = 0.7079f; // -3 dBFS
    
    // Trim positions (can be set by UI)
    int mTrimStart = -1; // -1 means auto-detect
    int mTrimEnd = -1;   // -1 means auto-detect
    
    // Detected values
    int mPeakPosition = 0;
    float mPeakAmplitude = 0.0f;
    int mNoiseFloorPosition = 0;
};

} // namespace impulser
