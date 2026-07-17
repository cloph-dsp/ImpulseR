#pragma once

#include <oboe/Oboe.h>
#include <memory>
#include <atomic>
#include <functional>
#include <vector>
#include "RingBuffer.h"

namespace impulser {

/**
 * Callback interface for receiving captured audio buffers.
 */
class IRCaptureCallback {
public:
    virtual ~IRCaptureCallback() = default;
    
    /**
     * Called when a complete captured buffer is available.
     * 
     * @param buffer Pointer to captured audio samples
     * @param numSamples Number of samples in the buffer
     */
    virtual void onCaptureComplete(const float* buffer, int numSamples) = 0;
};

/**
 * Full-duplex audio engine using Oboe.
 * 
 * Creates separate input and output streams for simultaneous playback and capture.
 * Handles latency compensation and provides a ring buffer for captured data.
 */
class OboeEngine : public oboe::AudioStreamDataCallback,
                   public oboe::AudioStreamErrorCallback {
public:
    OboeEngine();
    ~OboeEngine();

    /**
     * Initialize the audio engine with input and output streams.
     * 
     * @return true if initialization succeeded, false otherwise
     */
    bool initialize();

    /**
     * Start both input and output streams.
     * 
     * @return true if streams started successfully
     */
    bool start();

    /**
     * Stop both input and output streams.
     */
    void stop();

    /**
     * Set the audio data to play (ESS signal).
     * The engine will loop through this data until stopped.
     * 
     * @param data Pointer to audio samples
     * @param numSamples Number of samples
     */
    void setPlaybackData(const float* data, int numSamples);

    /**
     * Clear the playback data (stop playing).
     */
    void clearPlaybackData();

    /**
     * Get the round-trip delay in samples between input and output.
     * This is used for latency compensation during deconvolution.
     * 
     * @return Delay in samples
     */
    int getRoundTripDelaySamples() const { return mRoundTripDelaySamples; }

    /**
     * Get the sample rate of the audio streams.
     * 
     * @return Sample rate in Hz
     */
    int getSampleRate() const { return mSampleRate; }

    /**
     * Get the playback progress (0.0 to 1.0).
     * Uses double for precision, returns as float for JNI compatibility.
     * 
     * @return Progress 0.0-1.0, or 0.0 if no playback data
     */
    float getPlaybackProgress() {
        std::lock_guard<std::mutex> lock(mPlaybackMutex);
        if (mPlaybackData.empty()) return 0.0f;
        return static_cast<float>(static_cast<double>(mPlaybackIndex) / mPlaybackData.size());
    }

    /**
     * Get FFT magnitude spectrum of the last 1024 playback samples.
     * Reads last 1024 samples of mPlaybackData (zero-pads if not enough),
     * computes log-magnitude spectrum, returns first nBins bins.
     * 
     * @param outBins Output array for magnitude bins (size nBins)
     * @param nBins Number of bins to output
     */
    void getCurrentSpectrum(float* outBins, int nBins);

    /**
      * Get the ring buffer containing captured audio.
      * 
      * @return Pointer to the ring buffer
      */
    RingBuffer<float>* getCaptureBuffer() { return mCaptureBuffer.get(); }

    /**
     * Capture audio while playing ESS.
     * This method sets the ESS data, starts playback/recording,
     * waits for completion, and returns the captured audio.
     * 
     * @param essData ESS signal to play
     * @param essSamples Number of ESS samples
     * @param captureBuffer Output buffer for captured audio (must be pre-allocated)
     * @param captureSamples Number of samples to capture
     * @return true if capture succeeded
     */
    bool captureWhilePlaying(const float* essData, int essSamples,
                            float* captureBuffer, int captureSamples);

/**
      * Set the callback for receiving captured audio.
      * 
      * @param callback Pointer to the callback object
      */
    void setCallback(IRCaptureCallback* callback) { mCaptureCallback = callback; }

    /**
      * Check if the engine is currently running.
      * 
      * @return true if both streams are running
      */
    bool isRunning() const { return mIsRunning; }

    /**
      * Check if playback has completed (ESS finished).
      * 
      * @return true if playback is done
      */
    bool isPlaybackComplete() const { return !mIsPlaying; }

    // oboe::AudioStreamDataCallback interface
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* audioStream,
        void* audioData,
        int32_t numFrames) override;

    // oboe::AudioStreamErrorCallback interface
    void onErrorBeforeClose(oboe::AudioStream* audioStream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream* audioStream, oboe::Result error) override;

private:
    /**
     * Disable audio effects (AGC, NS, AEC) on the input stream.
     * This is a belt-and-suspenders measure in addition to InputPreset::Unprocessed.
     */
    void disableAudioEffects();

    /**
     * Measure the round-trip latency between input and output streams.
     */
    void measureLatency();

    // Audio streams
    std::shared_ptr<oboe::AudioStream> mOutputStream;
    std::shared_ptr<oboe::AudioStream> mInputStream;

    // Deferred-close guards: ensure close() is called exactly once per stream
    std::atomic<bool> mInputStopCompleted{false};
    std::atomic<bool> mOutputStopCompleted{false};

    // Configuration
    static constexpr int kChannelCount = 1; // Mono
    static constexpr int kFramesPerBurst = 192; // Typical for 48kHz
    static constexpr int kRingBufferSize = 16384; // Power of 2, >= 4x burst size (128 * 128)

    // State
    std::atomic<bool> mIsRunning{false};
    int mRoundTripDelaySamples = 0;
    int mSampleRate = 48000;

    // Callback
    IRCaptureCallback* mCaptureCallback = nullptr;

    // Playback data (mutex protects mPlaybackData + mPlaybackIndex from audio thread vs control thread)
    std::vector<float> mPlaybackData;
    int mPlaybackIndex = 0;
    std::atomic<bool> mIsPlaying{false};
    std::mutex mPlaybackMutex;

    // Ring buffer for captured audio
    std::unique_ptr<RingBuffer<float>> mCaptureBuffer;

    // Capture diagnostics
    std::atomic<bool> mCaptureClipped{false};
    float mPeakAmplitude = 0.0f;
    std::atomic<bool> mCaptureOverflowed{false};

    // Pre-roll/post-roll padding (in samples at current sample rate)
    // ponytail: post-roll not implemented — audio callback has no sweep-end signal
    int mPreRollSamples = 4800;   // 100 ms at 48 kHz
    int mPostRollSamples = 72000;  // 1.5 s at 48 kHz (allocated but not used)

    /**
     * Check if capture clipped (any sample ≥ 0.99f absolute).
     * Resets at start of each new capture.
     */
    bool wasCaptureClipped() const { return mCaptureClipped; }

    /**
     * Check if the capture ring buffer overflowed at any point.
     * Resets at start of each new capture.
     */
    bool wasCaptureOverflowed() const { return mCaptureOverflowed; }

    // Temporary buffer for output audio (sine wave for testing)
    std::vector<float> mOutputBuffer;
    int mOutputPhase = 0;
};

} // namespace impulser
