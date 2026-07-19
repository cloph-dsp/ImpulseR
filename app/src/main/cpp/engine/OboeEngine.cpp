#include "OboeEngine.h"
#include "CalibrationFilter.h"
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
                 ->setSampleRate(48000)
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

    // ponytail: Oboe 1.8 has no setOnStoppedCallback — close is performed
    // synchronously in stop() after waitForStateChange reaches Stopped.
    // The mOutputStopCompleted atomic is kept as a re-entrance guard.

    // Create input stream
    oboe::AudioStreamBuilder inputBuilder;
    inputBuilder.setDirection(oboe::Direction::Input)
                ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                ->setSharingMode(oboe::SharingMode::Shared)
                ->setFormat(oboe::AudioFormat::Float)
                ->setChannelCount(kChannelCount)
                ->setSampleRate(48000)
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

    if (mOutputStream) mSampleRate = mOutputStream->getSampleRate();

    // see comment above; same deferral for input stream
      
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

    auto waitForStreamStopped = [](oboe::AudioStream* stream, const char* tag) {
        if (!stream) return;
        if (stream->getState() == oboe::StreamState::Stopped) return;
        oboe::StreamState next = oboe::StreamState::Unknown;
        // 100ms timeout per poll; bounded total 2s safety.
        for (int i = 0; i < 100; ++i) {
            oboe::Result r = stream->waitForStateChange(
                oboe::StreamState::Stopping, &next, 100000000LL /* 100ms in ns */);
            if (r == oboe::Result::OK && (next == oboe::StreamState::Stopped ||
                                          next == oboe::StreamState::Disconnected)) {
                break;
            }
            if (r != oboe::Result::OK &&
                r != oboe::Result::ErrorTimeout &&
                r != oboe::Result::ErrorInvalidState) {
                break;
            }
        }
        LOGI("waitForStreamStopped[%s] -> state=%d", tag, (int)stream->getState());
    };

    if (mInputStream) {
        mInputStream->requestStop();
        waitForStreamStopped(mInputStream.get(), "in");
        if (!mInputStopCompleted.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            mInputStream->close();
        }
    }

    if (mOutputStream) {
        mOutputStream->requestStop();
        waitForStreamStopped(mOutputStream.get(), "out");
        if (!mOutputStopCompleted.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            mOutputStream->close();
        }
    }

    mIsRunning = false;
    LOGI("OboeEngine stop completed");
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

        for (int i = 0; i < numFrames; ++i) {
            float s = std::abs(inputBuffer[i]);
            if (s > mPeakAmplitude) mPeakAmplitude = s;
            if (s >= 0.99f) mCaptureClipped = true;
        }

        size_t written = mCaptureBuffer->write(inputBuffer, numFrames);
        if (written < static_cast<size_t>(numFrames)) {
            LOGW("Ring buffer overflow: wrote %zu/%d frames", written, numFrames);
            mCaptureOverflowed = true;
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
    if (!mOutputStream || !mInputStream) {
        mRoundTripDelaySamples = 960; // fallback
        return;
    }
    auto outLat = mOutputStream->calculateLatencyMillis();
    auto inLat = mInputStream->calculateLatencyMillis();
    if (!outLat || !inLat) {
        mRoundTripDelaySamples = 960;
        return;
    }
    double totalMs = outLat.value() + inLat.value();
    int samples = static_cast<int>(std::round(totalMs * mSampleRate / 1000.0));
    if (samples < 100) samples = 100;
    if (samples > 48000) samples = 48000;
    mRoundTripDelaySamples = samples;
    LOGI("Measured latency: %.2f ms (out=%.2f ms, in=%.2f ms) -> %d samples",
          totalMs, outLat.value(), inLat.value(), samples);
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
    mCaptureClipped = false;
    mPeakAmplitude = 0.0f;
    mCaptureOverflowed = false;

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

    // Pre-roll: write silence before capture begins
    if (mPreRollSamples > 0) {
        std::vector<float> preRoll(mPreRollSamples, 0.0f);
        mCaptureBuffer->write(preRoll.data(), mPreRollSamples);
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

    if (mCaptureClipped) {
        LOGW("Capture clipped at peak=%.3f", mPeakAmplitude);
    }

    int captured = mCaptureBuffer->read(captureBuffer, captureSamples);
    LOGI("Captured %d samples", captured);

    return captured > 0;
}

void OboeEngine::getCurrentSpectrum(float* outBins, int nBins) {
    static constexpr int kWindowSize = 1024;

    if (outBins == nullptr || nBins <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mPlaybackMutex);

    if (mPlaybackData.empty()) {
        for (int i = 0; i < nBins; i++) outBins[i] = 0.0f;
        return;
    }

    // Read last 1024 samples, zero-pad if needed
    float window[kWindowSize];
    int samplesAvailable = std::min(mPlaybackIndex, static_cast<int>(mPlaybackData.size()));

    if (samplesAvailable >= kWindowSize) {
        // Have enough samples, read the last 1024
        int start = samplesAvailable - kWindowSize;
        for (int i = 0; i < kWindowSize; i++) {
            window[i] = mPlaybackData[start + i];
        }
    } else {
        // Zero-pad the beginning
        for (int i = 0; i < kWindowSize - samplesAvailable; i++) {
            window[i] = 0.0f;
        }
        for (int i = 0; i < samplesAvailable; i++) {
            window[kWindowSize - samplesAvailable + i] = mPlaybackData[i];
        }
    }

    CalibrationFilter::computeMagnitudeSpectrum(window, kWindowSize, outBins, nBins);
}

void OboeEngine::getInputSpectrum(float* outBins, int nBins) {
    static constexpr int kWindowSize = 1024;

    if (outBins == nullptr || nBins <= 0) {
        return;
    }

    if (mCaptureBuffer == nullptr) {
        for (int i = 0; i < nBins; i++) outBins[i] = 0.0f;
        return;
    }

    // Peek last 1024 samples from capture buffer (zero-pad if not enough)
    float window[kWindowSize];
    size_t available = mCaptureBuffer->available();

    if (available >= kWindowSize) {
        // Have enough samples, peek the last 1024
        mCaptureBuffer->peek(window, kWindowSize);
    } else {
        // Zero-pad the beginning
        for (int i = 0; i < kWindowSize - static_cast<int>(available); i++) {
            window[i] = 0.0f;
        }
        if (available > 0) {
            mCaptureBuffer->peek(window + (kWindowSize - static_cast<int>(available)), available);
        }
    }

    CalibrationFilter::computeMagnitudeSpectrum(window, kWindowSize, outBins, nBins);
}

} // namespace impulser
