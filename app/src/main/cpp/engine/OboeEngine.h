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

    // Configuration
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannelCount = 1; // Mono
    static constexpr int kFramesPerBurst = 192; // Typical for 48kHz
    static constexpr int kRingBufferSize = 16384; // Power of 2, >= 4x burst size (128 * 128)

    // State
    std::atomic<bool> mIsRunning{false};
    int mRoundTripDelaySamples = 0;
    int mSampleRate = kSampleRate;

    // Callback
    IRCaptureCallback* mCaptureCallback = nullptr;

    // Playback data
    std::vector<float> mPlaybackData;
    int mPlaybackIndex = 0;
    std::atomic<bool> mIsPlaying{false};

    // Ring buffer for captured audio
    std::unique_ptr<RingBuffer<float>> mCaptureBuffer;

    // Temporary buffer for output audio (sine wave for testing)
    std::vector<float> mOutputBuffer;
    int mOutputPhase = 0;
};

} // namespace impulser
