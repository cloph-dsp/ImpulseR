#pragma once

#include <vector>
#include <cmath>

namespace impulser {

/**
 * Exponential Sine Sweep (ESS) generator.
 * 
 * Generates a logarithmic sine sweep signal suitable for impulse response measurement.
 * The sweep is designed to have constant energy per octave and produces a clean
 * impulse response after deconvolution.
 */
class ESSGenerator {
public:
    /**
     * Construct an ESS generator with specified parameters.
     * 
     * @param f1 Start frequency in Hz (default: 20.0)
     * @param f2 End frequency in Hz (default: 20000.0)
     * @param duration Duration in seconds (default: 7.0)
     * @param sampleRate Sample rate in Hz (default: 48000)
     */
    ESSGenerator(float f1 = 20.0f, float f2 = 20000.0f, float duration = 7.0f, int sampleRate = 48000);

    /**
     * Generate the ESS signal.
     * 
     * @param out Output buffer to fill with samples
     * @param numSamples Number of samples to generate
     */
    void generate(float* out, int numSamples);

    /**
     * Get the total number of samples in the sweep.
     * 
     * @return Number of samples
     */
    int getTotalSamples() const { return mTotalSamples; }

    /**
     * Get the duration in seconds.
     * 
     * @return Duration in seconds
     */
    float getDuration() const { return mDuration; }

    /**
     * Get the sample rate.
     * 
     * @return Sample rate in Hz
     */
    int getSampleRate() const { return mSampleRate; }

    /**
     * Get the start frequency.
     * 
     * @return Start frequency in Hz
     */
    float getF1() const { return mF1; }

    /**
     * Get the end frequency.
     * 
     * @return End frequency in Hz
     */
    float getF2() const { return mF2; }

private:
    /**
     * Apply raised-cosine fade-in to the beginning of the signal.
     * 
     * @param buffer Signal buffer
     * @param numSamples Total number of samples
     * @param fadeSamples Number of samples for fade-in
     */
    void applyFadeIn(float* buffer, int numSamples, int fadeSamples);

    /**
     * Apply raised-cosine fade-out to the end of the signal.
     * 
     * @param buffer Signal buffer
     * @param numSamples Total number of samples
     * @param fadeSamples Number of samples for fade-out
     */
    void applyFadeOut(float* buffer, int numSamples, int fadeSamples);

    // Parameters
    float mF1;           // Start frequency (Hz)
    float mF2;           // End frequency (Hz)
    float mDuration;     // Duration (seconds)
    int mSampleRate;     // Sample rate (Hz)
    
    // Derived parameters
    float mL;            // Sweep rate parameter
    int mTotalSamples;   // Total number of samples
    
    // Fade parameters (20ms each)
    static constexpr float kFadeDuration = 0.020f; // 20ms
};

} // namespace impulser
