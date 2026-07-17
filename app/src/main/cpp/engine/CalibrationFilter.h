#pragma once

#include <vector>
#include <string>
#include <memory>
#include <complex>

namespace impulser {

class OboeEngine;

/**
 * Hardware calibration filter for flat frequency response.
 * 
 * Computes and applies a Tikhonov-regularized inverse filter to compensate
 * for the device's speaker-microphone transfer function. The filter is
 * computed once per device and persisted to SharedPreferences.
 */
class CalibrationFilter {
public:
    /**
     * Construct a calibration filter with specified parameters.
     * 
     * @param f1 Start frequency in Hz
     * @param f2 End frequency in Hz
     * @param duration Calibration sweep duration in seconds
     * @param sampleRate Sample rate in Hz
     */
    CalibrationFilter(float f1 = 20.0f, float f2 = 20000.0f, float duration = 3.0f, int sampleRate = 48000);

    ~CalibrationFilter();

    /**
     * Run calibration by playing a short ESS and recording simultaneously.
     * 
     * @param oboeEngine Reference to OboeEngine for audio I/O
     * @return true if calibration succeeded
     */
    bool runCalibration(OboeEngine& oboeEngine);

    /**
     * Apply calibration filter to a signal.
     * 
     * @param input Input signal
     * @param output Output signal (can be same as input)
     * @param numSamples Number of samples
     */
    void apply(const float* input, float* output, int numSamples);

    /**
     * Get the calibration filter coefficients.
     * 
     * @return Pointer to filter coefficients
     */
    const float* getFilter() const { return mFilter.data(); }

    /**
     * Get the filter length.
     * 
     * @return Number of taps
     */
    int getFilterLength() const { return static_cast<int>(mFilter.size()); }

    /**
     * Check if calibration has been performed.
     * 
     * @return true if calibration is available
     */
    bool isCalibrated() const { return mIsCalibrated; }

    /**
     * Load calibration from storage.
     * 
     * @param deviceId Device identifier string
     * @return true if calibration was loaded successfully
     */
    bool load(const std::string& deviceId);

    /**
     * Save calibration to storage.
     * 
     * @param deviceId Device identifier string
     * @return true if calibration was saved successfully
     */
    bool save(const std::string& deviceId);

    /**
     * Get the device ID for the current device.
     * 
     * @return Device ID string
     */
    static std::string getDeviceId();

    /**
     * Compute magnitude spectrum from audio window.
     * Reads window of samples, runs FFT, returns log-magnitude of first nBins.
     * 
     * @param window Input audio samples
     * @param windowSize Number of samples in window (should be power of 2)
     * @param outBins Output magnitude bins (size nBins)
     * @param nBins Number of output bins
     */
    static void computeMagnitudeSpectrum(const float* window, int windowSize,
                                        float* outBins, int nBins);

private:
    /**
     * Compute the device transfer function from recorded calibration signal.
     * 
     * @param recorded Recorded calibration signal
     * @param recLen Length of recorded signal
     * @param reference Reference ESS signal
     * @param refLen Length of reference signal
     * @return true if computation succeeded
     */
    bool computeTransferFunction(const float* recorded, int recLen, 
                                  const float* reference, int refLen);

    /**
     * Compute Tikhonov-regularized inverse filter.
     * 
     * @param H_dev Device transfer function (complex spectrum)
     * @param N FFT size
     */
    void computeInverseFilter(const std::complex<float>* H_dev, int N);

    /**
     * Find the next power of 2 >= n.
     * 
     * @param n Minimum size
     * @return Next power of 2
     */
    int nextPowerOf2(int n) const;

    // Parameters
    float mF1;           // Start frequency (Hz)
    float mF2;           // End frequency (Hz)
    float mDuration;     // Duration (seconds)
    int mSampleRate;     // Sample rate (Hz)
    
    // Calibration filter
    std::vector<float> mFilter;
    static constexpr int kFilterLength = 2048; // 42.7ms at 48kHz
    
    // State
    bool mIsCalibrated = false;
};

} // namespace impulser
