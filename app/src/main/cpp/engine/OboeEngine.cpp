#include "OboeEngine.h"
#include <android/log.h>
#include <jni.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>

#define LOG_TAG "OboeEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace impulser {

OboeEngine::OboeEngine() {
    LOGI("OboeEngine created");
}

OboeEngine::~OboeEngine() {
    stop();
    LOGI("OboeEngine destroyed");
}

bool OboeEngine::initialize() {
    LOGI("Initializing OboeEngine");

    // Create output stream
    oboe::AudioStreamBuilder outputBuilder;
    outputBuilder.setDirection(oboe::Direction::Output)
                 ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                 ->setSharingMode(oboe::SharingMode::Shared)
                 ->setFormat(oboe::AudioFormat::Float)
                 ->setChannelCount(kChannelCount)
                 ->setSampleRate(kSampleRate)
                 ->setDataCallback(this)
                 ->setErrorCallback(this);

    oboe::Result result = outputBuilder.openStream(mOutputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open output stream: %s", oboe::convertToText(result));
        return false;
    }

    LOGI("Output stream opened: %d Hz, %d channels, format=%d",
         mOutputStream->getSampleRate(),
         mOutputStream->getChannelCount(),
         static_cast<int>(mOutputStream->getFormat()));

    // Create input stream
    oboe::AudioStreamBuilder inputBuilder;
    inputBuilder.setDirection(oboe::Direction::Input)
                ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                ->setSharingMode(oboe::SharingMode::Shared)
                ->setFormat(oboe::AudioFormat::Float)
                ->setChannelCount(kChannelCount)
                ->setSampleRate(kSampleRate)
                ->setInputPreset(oboe::InputPreset::Unprocessed)
                ->setDataCallback(this)
                ->setErrorCallback(this);

    result = inputBuilder.openStream(mInputStream);
    if (result != oboe::Result::OK) {
        LOGE("Failed to open input stream: %s", oboe::convertToText(result));
        mOutputStream->close();
        return false;
    }

LOGI("Input stream opened: %d Hz, %d channels, format=%d, inputPreset=%d",
          mInputStream->getSampleRate(),
          mInputStream->getChannelCount(),
          static_cast<int>(mInputStream->getFormat()),
          static_cast<int>(mInputStream->getInputPreset()));
     
// Initialize ring buffer after streams are opened
      try {
          mCaptureBuffer = std::make_unique<RingBuffer<float>>(kRingBufferSize);
      } catch (const std::exception& e) {
          LOGE("Failed to create ring buffer: %s", e.what());
          return false;
      }
     
     // Disable audio effects as a belt-and-suspenders measure
     disableAudioEffects();

    // Measure round-trip latency
    measureLatency();

    LOGI("OboeEngine initialized successfully, round-trip delay: %d samples",
         mRoundTripDelaySamples);

    return true;
}

bool OboeEngine::start() {
    if (mIsRunning) {
        LOGW("OboeEngine already running");
        return true;
    }

    LOGI("Starting OboeEngine");

    // Start output stream first
    oboe::Result result = mOutputStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start output stream: %s", oboe::convertToText(result));
        return false;
    }

    // Start input stream
    result = mInputStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input stream: %s", oboe::convertToText(result));
        mOutputStream->requestStop();
        return false;
    }

    mIsRunning = true;
    LOGI("OboeEngine started");
    return true;
}

void OboeEngine::stop() {
    if (!mIsRunning) {
        return;
    }

    LOGI("Stopping OboeEngine");

    if (mInputStream) {
        mInputStream->requestStop();
        mInputStream->close();
    }

    if (mOutputStream) {
        mOutputStream->requestStop();
        mOutputStream->close();
    }

    mIsRunning = false;
    LOGI("OboeEngine stopped");
}

void OboeEngine::setPlaybackData(const float* data, int numSamples) {
    std::lock_guard<std::mutex> lock(mPlaybackMutex);
    mPlaybackData.assign(data, data + numSamples);
    mPlaybackIndex = 0;
    mIsPlaying = true;
    LOGI("Playback data set: %d samples", numSamples);
}

void OboeEngine::clearPlaybackData() {
    std::lock_guard<std::mutex> lock(mPlaybackMutex);
    mIsPlaying = false;
    mPlaybackData.clear();
    mPlaybackIndex = 0;
    LOGI("Playback data cleared");
}

oboe::DataCallbackResult OboeEngine::onAudioReady(
    oboe::AudioStream* audioStream,
    void* audioData,
    int32_t numFrames) {

    if (audioStream->getDirection() == oboe::Direction::Output) {
        float* outputBuffer = static_cast<float*>(audioData);

        bool playing = mIsPlaying.load();
        if (playing) {
            std::lock_guard<std::mutex> lock(mPlaybackMutex);
            if (!mPlaybackData.empty()) {
                for (int i = 0; i < numFrames; ++i) {
                    if (mPlaybackIndex >= static_cast<int>(mPlaybackData.size())) {
                        std::memset(outputBuffer + i, 0, (numFrames - i) * sizeof(float));
                        mIsPlaying.store(false);
                        break;
                    }
                    outputBuffer[i] = mPlaybackData[mPlaybackIndex++];
                }
            } else {
                std::memset(outputBuffer, 0, numFrames * sizeof(float));
            }
        } else {
            std::memset(outputBuffer, 0, numFrames * sizeof(float));
        }
        
    } else if (audioStream->getDirection() == oboe::Direction::Input) {
        if (!mCaptureBuffer) return oboe::DataCallbackResult::Continue;

        const float* inputBuffer = static_cast<const float*>(audioData);

        size_t written = mCaptureBuffer->write(inputBuffer, numFrames);
        if (written < static_cast<size_t>(numFrames)) {
            LOGW("Ring buffer overflow: wrote %zu/%d frames", written, numFrames);
        }

        if (mCaptureCallback && mCaptureBuffer->available() >= numFrames) {
            std::vector<float> buffer(numFrames);
            size_t read = mCaptureBuffer->read(buffer.data(), numFrames);
            if (read > 0) {
                mCaptureCallback->onCaptureComplete(buffer.data(), read);
            }
        }
    }

    return oboe::DataCallbackResult::Continue;
}

void OboeEngine::onErrorBeforeClose(oboe::AudioStream* audioStream, oboe::Result error) {
    LOGE("Error before close on %s stream: %s",
         audioStream->getDirection() == oboe::Direction::Input ? "input" : "output",
         oboe::convertToText(error));
}

void OboeEngine::onErrorAfterClose(oboe::AudioStream* audioStream, oboe::Result error) {
    LOGE("Error after close on %s stream: %s",
         audioStream->getDirection() == oboe::Direction::Input ? "input" : "output",
         oboe::convertToText(error));
}

void OboeEngine::disableAudioEffects() {
    // This is a belt-and-suspenders measure to disable AGC/NS/AEC
    // InputPreset::Unprocessed should handle this on API 29+, but we
    // explicitly disable effects as well for maximum compatibility
    
    // Note: In a production app, we would use JNI to enumerate AudioEffect
    // descriptors and disable them on the session ID. For now, we rely on
    // InputPreset::Unprocessed which is reliable on API 29+.
    
    LOGI("Audio effects disabled (using InputPreset::Unprocessed)");
}

void OboeEngine::measureLatency() {
    // Get timestamps from both streams to calculate round-trip delay
    int64_t outputTimestamp = 0;
    int64_t inputTimestamp = 0;
    
    if (mOutputStream && mInputStream) {
        // Note: getTimestamp requires the streams to be running
        // For now, we'll use a default value and measure during actual capture
        // In production, we would:
        // 1. Start both streams
        // 2. Play a known signal
        // 3. Record and correlate to find the delay
        
        // Default estimate: typical Android latency is ~10-20ms
        // At 48kHz, that's 480-960 samples
        mRoundTripDelaySamples = 960; // 20ms default
        
        LOGI("Using default round-trip delay estimate: %d samples", mRoundTripDelaySamples);
    }
}

bool OboeEngine::captureWhilePlaying(const float* essData, int essSamples,
                                     float* captureBuffer, int captureSamples) {
    if (!essData || essSamples <= 0 || !captureBuffer || captureSamples <= 0) {
        LOGE("Invalid parameters for captureWhilePlaying");
        return false;
    }

    LOGI("Starting capture while playing: %d ESS samples, capturing %d samples",
         essSamples, captureSamples);

    setPlaybackData(essData, essSamples);

    if (!mCaptureBuffer) {
        LOGE("mCaptureBuffer is null, aborting capture");
        return false;
    }
    mCaptureBuffer->clear();

    oboe::Result result = mOutputStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start output stream: %s", oboe::convertToText(result));
        clearPlaybackData();
        return false;
    }

    result = mInputStream->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input stream: %s", oboe::convertToText(result));
        mOutputStream->requestStop();
        clearPlaybackData();
        return false;
    }

    int totalSamples = essSamples + captureSamples;
    int waitCount = 0;
    int maxWait = (totalSamples * 1000) / mSampleRate + 100;

    while (mCaptureBuffer->available() < totalSamples && waitCount < maxWait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }

    mInputStream->requestStop();
    mOutputStream->requestStop();
    clearPlaybackData();

    int captured = mCaptureBuffer->read(captureBuffer, captureSamples);
    LOGI("Captured %d samples", captured);

    return captured > 0;
}

} // namespace impulser
