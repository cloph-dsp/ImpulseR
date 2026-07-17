#pragma once

#include <vector>
#include <memory>
#include <complex>

namespace impulser {

/**
 * FFT-based deconvolver for ESS impulse response extraction.
 * 
 * Performs frequency-domain deconvolution using the analytical inverse filter
 * of the exponential sine sweep. Includes harmonic isolation to extract only
 * the linear impulse response.
 */
class Deconvolver {
public:
    /**
     * Construct a deconvolver with specified parameters.
     * 
     * @param f1 Start frequency in Hz
     * @param f2 End frequency in Hz
     * @param duration Sweep duration in seconds
     * @param sampleRate Sample rate in Hz
     */
    Deconvolver(float f1, float f2, float duration, int sampleRate);

    ~Deconvolver();

    /**
     * Deconvolve a recorded sweep to extract the impulse response.
     * 
     * @param recorded Pointer to recorded sweep samples
     * @param recLen Number of samples in recorded sweep
     * @param irOut Pointer to output IR buffer (allocated by this function)
     * @param irLen Pointer to output IR length
     * @return true if deconvolution succeeded
     */
    bool deconvolve(const float* recorded, int recLen, float** irOut, int* irLen);

    /**
     * Set calibration filter to apply before deconvolution.
     * 
     * @param filter Pointer to calibration filter samples
     * @param filterLen Number of samples in filter
     */
    void setCalibrationFilter(const float* filter, int filterLen);

    /**
     * Get the round-trip delay in samples for latency compensation.
     * 
     * @return Delay in samples
     */
    int getRoundTripDelaySamples() const { return mRoundTripDelaySamples; }

    /**
     * Set the round-trip delay in samples.
     * 
     * @param delay Delay in samples
     */
    void setRoundTripDelaySamples(int delay) { mRoundTripDelaySamples = delay; }

private:
    /**
     * Generate the analytical inverse filter for the ESS.
     * 
     * @param out Output buffer for inverse filter
     * @param numSamples Number of samples to generate
     */
    void generateInverseFilter(float* out, int numSamples);

    /**
     * Find the next power of 2 >= n.
     * 
     * @param n Minimum size
     * @return Next power of 2
     */
    int nextPowerOf2(int n) const;

    /**
     * Apply calibration filter in frequency domain.
     * 
     * @param spectrum Complex spectrum to modify
     * @param N FFT size
     */
    void applyCalibrationFilter(std::complex<float>* spectrum, int N);

    /**
      * Isolate the linear IR from the full deconvolved signal.
      * 
      * @param irFull Full deconvolved signal
      * @param N_fft FFT size
      * @param irOut Output buffer for isolated IR
      * @param irLen Output IR length
      * @return true if isolation succeeded
      */
    bool isolateLinearIR(const float* irFull, int N_fft, float** irOut, int* irLen);

    // Parameters
    float mF1;           // Start frequency (Hz)
    float mF2;           // End frequency (Hz)
    float mDuration;     // Duration (seconds)
    int mSampleRate;     // Sample rate (Hz)
    
    // Derived parameters
    float mL;            // Sweep rate parameter
    
    // Calibration filter
    std::vector<float> mCalibrationFilter;
    bool mHasCalibrationFilter = false;

    // Cached FFT of calibration filter
    std::vector<std::complex<float>> mCalibrationFilterFreq;
    int mCalibrationFilterN = 0;
    
    // Latency compensation
    int mRoundTripDelaySamples = 0;
};

} // namespace impulser
