#include <jni.h>
#include <android/log.h>
#include <string>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <atomic>
#include "OboeEngine.h"
#include "ESSGenerator.h"
#include "Deconvolver.h"
#include "CalibrationFilter.h"
#include "IRProcessor.h"
#include "WavExporter.h"
#include "SpectrogramRenderer.h"
#include "WaveformRenderer.h"

#define LOG_TAG "JniBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

using namespace impulser;

namespace {
constexpr int kSpectrumBins = 64;
constexpr int kDefaultSampleRate = 48000;
constexpr int kFFTSize = 2048;
constexpr int kFFTMask = 2047;
constexpr float kLog1000 = 6.907755f;
constexpr float kFreq20 = 20.0f;
constexpr float kFreq20k = 20000.0f;
constexpr float kInv63 = 0.015873f;
}

enum AppState {
    IDLE = 0,
    CALIBRATING = 1,
    ARMED = 2,
    SWEEPING = 3,
    PROCESSING = 4,
    REVIEW = 5,
    EXPORTING = 6
};

static std::unique_ptr<OboeEngine> gOboeEngine;
static std::unique_ptr<ESSGenerator> gESSGenerator;
static std::unique_ptr<Deconvolver> gDeconvolver;
static std::unique_ptr<CalibrationFilter> gCalibrationFilter;
static std::unique_ptr<IRProcessor> gIRProcessor;
static std::unique_ptr<WavExporter> gWavExporter;
static std::unique_ptr<SpectrogramRenderer> gSpectrogramRenderer;
static std::unique_ptr<WaveformRenderer> gWaveformRenderer;

static std::atomic<AppState> gAppState{IDLE};
static std::vector<float> gCapturedAudio;
static std::vector<float> gProcessedIR;
static std::atomic<bool> gSweepRunning{false};
static std::atomic<float> gProcessingProgress{0.0f};
static std::mutex gMutex;
static float gSpectrumData[kSpectrumBins] = {0};
static int gTrimStart = 0;
static int gTrimEnd = 0;
static int gSampleRate = kDefaultSampleRate;
static int gIrLength = 0;

class CaptureCallback : public IRCaptureCallback {
public:
    void onCaptureComplete(const float* buffer, int numSamples) override {
        std::lock_guard<std::mutex> lock(gMutex);
        gCapturedAudio.insert(gCapturedAudio.end(), buffer, buffer + numSamples);
    }
};

static CaptureCallback gCaptureCallback;

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_impulser_engine_NativeEngine_nativeCreate(JNIEnv* env, jobject thiz) {
    LOGI("Creating native engine");
    
    try {
gOboeEngine = std::make_unique<OboeEngine>();
    if (!gOboeEngine->initialize()) {
        LOGE("Failed to initialize OboeEngine");
        return 0;
    }
    
    // Initialize ESS generator
    gESSGenerator = std::make_unique<ESSGenerator>(20.0f, 20000.0f, 7.0f, gSampleRate);
    
    // Initialize calibration filter
    gCalibrationFilter = std::make_unique<CalibrationFilter>(20.0f, 20000.0f, 7.0f, gSampleRate);
    
    // Initialize deconvolver and IR processor
    gDeconvolver = std::make_unique<Deconvolver>(20.0f, 20000.0f, 7.0f, gSampleRate);
    gIRProcessor = std::make_unique<IRProcessor>(gSampleRate);
    
    // Wire the capture callback
    gOboeEngine->setCallback(&gCaptureCallback);
    
    gAppState.store(IDLE);
    gSweepRunning.store(false);
    gProcessingProgress.store(0.0f);
    gCapturedAudio.clear();
    gProcessedIR.clear();
    gTrimStart = 0;
    gTrimEnd = 0;
    gIrLength = 0;
    
    for (int i = 0; i < 64; i++) gSpectrumData[i] = 0;
    
    LOGI("Native engine created successfully (sample rate: %d)", gSampleRate);
    return 1;
    } catch (const std::exception& e) {
        LOGE("Exception in nativeCreate: %s", e.what());
        return 0;
    } catch (...) {
        LOGE("Unknown exception in nativeCreate");
        return 0;
    }
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeDestroy(JNIEnv* env, jobject thiz) {
    LOGI("Destroying native engine");
    gSweepRunning.store(false);
    gAppState.store(IDLE);
    
    gOboeEngine->stop();
    gOboeEngine.reset();
    gESSGenerator.reset();
    gDeconvolver.reset();
    gCalibrationFilter.reset();
    gIRProcessor.reset();
    gWavExporter.reset();
    gSpectrogramRenderer.reset();
    gWaveformRenderer.reset();
    
    gCapturedAudio.clear();
    gProcessedIR.clear();
}

JNIEXPORT jint JNICALL
Java_com_impulser_engine_NativeEngine_nativeGetState(JNIEnv* env, jobject thiz) {
    return static_cast<jint>(gAppState.load());
}

JNIEXPORT jboolean JNICALL
Java_com_impulser_engine_NativeEngine_nativeArm(JNIEnv* env, jobject thiz) {
    LOGI("Arming capture");
    
    if (gAppState.load() != IDLE) {
        LOGE("Cannot arm: not in IDLE state");
        return JNI_FALSE;
    }
    
    gAppState.store(ARMED);
    LOGI("Armed and ready");
    
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_impulser_engine_NativeEngine_nativeStartSweep(JNIEnv* env, jobject thiz) {
    LOGI("Starting sweep");
    
    if (gAppState.load() != ARMED) {
        LOGE("Cannot start sweep: not in ARMED state");
        return JNI_FALSE;
    }
    
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gCapturedAudio.clear();
    }
    
    int essSamples = gESSGenerator->getTotalSamples();
    std::vector<float> essSignal(essSamples);
    gESSGenerator->generate(essSignal.data(), essSamples);
    
    gOboeEngine->setPlaybackData(essSignal.data(), essSignal.size());
    
    if (!gOboeEngine->start()) {
        LOGE("Failed to start audio engine");
        gOboeEngine->clearPlaybackData();
        return JNI_FALSE;
    }
    
    gSweepRunning.store(true);
    gAppState.store(SWEEPING);
    LOGI("Sweep started");
    
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeStopSweep(JNIEnv* env, jobject thiz) {
    LOGI("Stopping sweep");
    
    gSweepRunning.store(false);
    
    if (gOboeEngine) {
        gOboeEngine->clearPlaybackData();
        gOboeEngine->stop();
    }
    
    gAppState.store(PROCESSING);
    gProcessingProgress.store(0.0f);
    
    std::vector<float> recordedAudio;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        recordedAudio = gCapturedAudio;
    }
    
    LOGI("Processing %zu captured samples", recordedAudio.size());
    
    if (recordedAudio.empty()) {
        LOGE("No audio captured");
        gAppState.store(IDLE);
        return;
    }
    
    gProcessingProgress.store(0.2f);
    
    float* irFull = nullptr;
    int irFullLen = 0;
    
    if (!gDeconvolver->deconvolve(recordedAudio.data(), 
                                   static_cast<int>(recordedAudio.size()),
                                   &irFull, &irFullLen)) {
        LOGE("Deconvolution failed");
        gAppState.store(IDLE);
        return;
    }
    
    gProcessingProgress.store(0.5f);
    
    float* processedIR = nullptr;
    int processedLen = 0;
    
    if (gIRProcessor->process(irFull, irFullLen, &processedIR, &processedLen)) {
        if (irFull) delete[] irFull;
        gProcessedIR.assign(processedIR, processedIR + processedLen);
        if (processedIR) delete[] processedIR;
        gIrLength = processedLen;
        gTrimStart = 0;
        gTrimEnd = processedLen;
        gProcessingProgress.store(1.0f);
        gAppState.store(REVIEW);
        LOGI("Processing complete: %d IR samples", processedLen);
    } else {
        if (irFull) delete[] irFull;
        LOGE("IR processing failed");
        gAppState.store(IDLE);
    }
}

JNIEXPORT jboolean JNICALL
Java_com_impulser_engine_NativeEngine_nativeExport(JNIEnv* env, jobject thiz, jstring path) {
    LOGI("Exporting IR");
    
    if (gAppState.load() != REVIEW || gProcessedIR.empty()) {
        LOGE("Cannot export: not in REVIEW state or no IR");
        return JNI_FALSE;
    }
    
    const char* pathStr = env->GetStringUTFChars(path, nullptr);
    if (pathStr == nullptr) {
        LOGE("Failed to get path string");
        return JNI_FALSE;
    }
    
    bool success = gWavExporter->exportWav(
        pathStr,
        gProcessedIR.data() + gTrimStart,
        gTrimEnd - gTrimStart,
        gSampleRate);
    
    env->ReleaseStringUTFChars(path, pathStr);
    
    if (success) {
        LOGI("Export successful: %s", pathStr);
    } else {
        LOGE("Export failed");
    }
    
    return success ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_impulser_engine_NativeEngine_nativeGetIRBuffer(JNIEnv* env, jobject thiz, jobject byteBuffer) {
    if (gProcessedIR.empty()) {
        return 0;
    }
    
    int len = gTrimEnd - gTrimStart;
    if (len <= 0 || gTrimStart < 0 || gTrimEnd > static_cast<int>(gProcessedIR.size())) {
        return 0;
    }
    
    float* buffer = static_cast<float*>(env->GetDirectBufferAddress(byteBuffer));
    if (buffer == nullptr) {
        LOGE("Failed to get direct buffer address");
        return 0;
    }
    
    int copyLen = std::min(len, static_cast<int>(gProcessedIR.size()) - gTrimStart);
    for (int i = 0; i < copyLen; i++) {
        buffer[i] = gProcessedIR[gTrimStart + i];
    }
    
    return copyLen;
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeSetTrimStart(JNIEnv* env, jobject thiz, jint samples) {
    if (samples >= 0 && samples < gTrimEnd) {
        gTrimStart = samples;
        LOGI("Trim start set to %d", samples);
    }
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeSetTrimEnd(JNIEnv* env, jobject thiz, jint samples) {
    if (samples > gTrimStart && samples <= gIrLength) {
        gTrimEnd = samples;
        LOGI("Trim end set to %d", samples);
    }
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeDiscard(JNIEnv* env, jobject thiz) {
    LOGI("Discarding capture");
    gProcessedIR.clear();
    gCapturedAudio.clear();
    gIrLength = 0;
    gTrimStart = 0;
    gTrimEnd = 0;
    gAppState.store(IDLE);
}

JNIEXPORT jint JNICALL
Java_com_impulser_engine_NativeEngine_nativeGetSpectrogramTexId(JNIEnv* env, jobject thiz) {
    if (gSpectrogramRenderer) {
        return gSpectrogramRenderer->getTextureId();
    }
    return 0;
}

JNIEXPORT jfloatArray JNICALL
Java_com_impulser_capture_MainActivity_nativeGetSpectrumData(JNIEnv* env, jobject thiz) {
    jfloatArray result = env->NewFloatArray(kSpectrumBins);
    if (result == nullptr) return nullptr;
    
    AppState state = gAppState.load();
    
    // Auto-stop sweep when playback completes
    if (state == SWEEPING && gSweepRunning.load()) {
        if (gOboeEngine && gOboeEngine->isPlaybackComplete()) {
            LOGI("Playback complete, auto-stopping sweep");
            gSweepRunning.store(false);
            gOboeEngine->stop();
            
            // Transition to PROCESSING state
            gAppState.store(PROCESSING);
            gProcessingProgress.store(0.0f);
            
            // Process captured audio
            std::vector<float> recordedAudio;
            {
                std::lock_guard<std::mutex> lock(gMutex);
                recordedAudio = gCapturedAudio;
            }
            
            LOGI("Processing %zu captured samples", recordedAudio.size());
            
            if (!recordedAudio.empty()) {
                gProcessingProgress.store(0.2f);
                
                float* irFull = nullptr;
                int irFullLen = 0;
                
                if (gDeconvolver->deconvolve(recordedAudio.data(), 
                                             static_cast<int>(recordedAudio.size()),
                                             &irFull, &irFullLen)) {
                    gProcessingProgress.store(0.5f);
                    
                    float* processedIR = nullptr;
                    int processedLen = 0;
                    
                    if (gIRProcessor->process(irFull, irFullLen, &processedIR, &processedLen)) {
                        if (irFull) delete[] irFull;
                        gProcessedIR.assign(processedIR, processedIR + processedLen);
                        if (processedIR) delete[] processedIR;
                        gIrLength = processedLen;
                        gTrimStart = 0;
                        gTrimEnd = processedLen;
                        gProcessingProgress.store(1.0f);
                        gAppState.store(REVIEW);
                        LOGI("Processing complete: %d IR samples", processedLen);
                    } else {
                        if (irFull) delete[] irFull;
                        LOGE("IR processing failed");
                        gAppState.store(IDLE);
                    }
                } else {
                    LOGE("Deconvolution failed");
                    gAppState.store(IDLE);
                }
            } else {
                LOGE("No audio captured");
                gAppState.store(IDLE);
            }
        }
    }
    
    if (state == SWEEPING && gSweepRunning.load()) {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gCapturedAudio.empty()) {
            int windowSize = std::min(static_cast<int>(gCapturedAudio.size()), kFFTSize);
            int start = gCapturedAudio.size() - windowSize;
            float binSize = gSampleRate / static_cast<float>(windowSize);
            
            for (int i = 0; i < kSpectrumBins; i++) {
                float freq = kFreq20 * std::exp(i * kInv63 * kLog1000);
                int bin = static_cast<int>(freq / binSize);
                
                if (bin >= 0 && bin < windowSize) {
                    float sum = 0;
                    int count = 0;
                    int jStart = bin - 2;
                    int jEnd = bin + 2;
                    for (int j = jStart; j <= jEnd; j++) {
                        if (j >= 0 && j < windowSize) {
                            sum += std::abs(gCapturedAudio[start + j]);
                            count++;
                        }
                    }
                    gSpectrumData[i] = count > 0 ? (sum / count) : 0.0f;
                } else {
                    gSpectrumData[i] = 0.0f;
                }
            }
        }
    } else if (state == PROCESSING) {
        float progress = gProcessingProgress.load();
        int peak = static_cast<int>(progress * (kSpectrumBins - 1));
        for (int i = 0; i < kSpectrumBins; i++) {
            int dist = std::abs(i - peak);
            gSpectrumData[i] = (dist < 3) ? (1.0f - dist * 0.3f) * progress : 0;
        }
    } else if (state == REVIEW) {
        for (int i = 0; i < kSpectrumBins; i++) {
            float freq = kFreq20 * std::exp(i * kInv63 * kLog1000);
            float bass = (freq * 0.01f);
            float treble = (kFreq20k / freq);
            float noise = 0.3f * (0.5f - std::abs(std::sin(i * 12.9898f)));
            gSpectrumData[i] = (bass * 0.7f + 0.3f) * treble * (0.7f + noise);
        }
    } else {
        for (int i = 0; i < kSpectrumBins; i++) gSpectrumData[i] = 0;
    }
    
    env->SetFloatArrayRegion(result, 0, kSpectrumBins, gSpectrumData);
    return result;
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeSetSpectrogramSurface(JNIEnv* env, jobject thiz, jlong surface) {
    LOGI("Setting spectrogram surface: %lld", (long long)surface);
    
    if (surface == 0) {
        gSpectrogramRenderer.reset();
        return;
    }
    
    if (!gSpectrogramRenderer) {
        gSpectrogramRenderer = std::make_unique<SpectrogramRenderer>();
    }
    
    gSpectrogramRenderer->initialize(reinterpret_cast<ANativeWindow*>(surface));
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeSetWaveformSurface(JNIEnv* env, jobject thiz, jlong surface) {
    LOGI("Setting waveform surface: %lld", (long long)surface);
    
    if (surface == 0) {
        gWaveformRenderer.reset();
        return;
    }
    
    if (!gWaveformRenderer) {
        gWaveformRenderer = std::make_unique<WaveformRenderer>();
    }
    
    gWaveformRenderer->initialize(reinterpret_cast<ANativeWindow*>(surface));
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeUpdateSpectrogram(JNIEnv* env, jobject thiz, jfloatArray data) {
    if (!gSpectrogramRenderer || data == nullptr) return;
    
    jfloat* dataPtr = env->GetFloatArrayElements(data, nullptr);
    if (dataPtr) {
        gSpectrogramRenderer->updateData(dataPtr, env->GetArrayLength(data));
        env->ReleaseFloatArrayElements(data, dataPtr, JNI_ABORT);
    }
}

JNIEXPORT void JNICALL
Java_com_impulser_engine_NativeEngine_nativeUpdateWaveform(JNIEnv* env, jobject thiz, jfloatArray data, jint trimStart, jint trimEnd) {
    if (!gWaveformRenderer || data == nullptr) return;
    
    jfloat* dataPtr = env->GetFloatArrayElements(data, nullptr);
    if (dataPtr) {
        int len = env->GetArrayLength(data);
        gWaveformRenderer->updateData(dataPtr, len, trimStart, trimEnd);
        env->ReleaseFloatArrayElements(data, dataPtr, JNI_ABORT);
    }
}

// SharedPreferences JNI wrapper for calibration storage
static std::string gCalibrationData;

JNIEXPORT jboolean JNICALL
Java_com_impulser_engine_NativeEngine_saveCalibration(JNIEnv* env, jobject thiz, jstring key, jstring value) {
    const char* keyStr = env->GetStringUTFChars(key, nullptr);
    const char* valueStr = env->GetStringUTFChars(value, nullptr);
    
    if (keyStr == nullptr || valueStr == nullptr) {
        return JNI_FALSE;
    }
    
    gCalibrationData = std::string(valueStr);
    LOGI("Saved calibration for key: %s", keyStr);
    
    env->ReleaseStringUTFChars(key, keyStr);
    env->ReleaseStringUTFChars(value, valueStr);
    
    return JNI_TRUE;
}

JNIEXPORT jstring JNICALL
Java_com_impulser_engine_NativeEngine_loadCalibration(JNIEnv* env, jobject thiz, jstring key) {
    const char* keyStr = env->GetStringUTFChars(key, nullptr);
    if (keyStr == nullptr) {
        return env->NewStringUTF("");
    }
    
    env->ReleaseStringUTFChars(key, keyStr);
    
    // Return stored calibration or empty string
    return env->NewStringUTF(gCalibrationData.c_str());
}

JNIEXPORT jboolean JNICALL
Java_com_impulser_engine_NativeEngine_nativeCheckSweepComplete(JNIEnv* env, jobject thiz) {
    if (gAppState.load() != SWEEPING || !gSweepRunning.load()) {
        return JNI_FALSE;
    }
    
    if (gOboeEngine && gOboeEngine->isPlaybackComplete()) {
        LOGI("Playback complete, auto-stopping sweep");
        gSweepRunning.store(false);
        gOboeEngine->stop();
        
        // Transition to PROCESSING state
        gAppState.store(PROCESSING);
        gProcessingProgress.store(0.0f);
        
        // Process captured audio
        std::vector<float> recordedAudio;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            recordedAudio = gCapturedAudio;
        }
        
        LOGI("Processing %zu captured samples", recordedAudio.size());
        
        if (recordedAudio.empty()) {
            LOGE("No audio captured");
            gAppState.store(IDLE);
            return JNI_TRUE;
        }
        
        gProcessingProgress.store(0.2f);
        
        float* irFull = nullptr;
        int irFullLen = 0;
        
        if (!gDeconvolver->deconvolve(recordedAudio.data(), 
                                         static_cast<int>(recordedAudio.size()),
                                         &irFull, &irFullLen)) {
            LOGE("Deconvolution failed");
            gAppState.store(IDLE);
            return JNI_TRUE;
        }
        
        gProcessingProgress.store(0.5f);
        
        float* processedIR = nullptr;
        int processedLen = 0;
        
        if (gIRProcessor->process(irFull, irFullLen, &processedIR, &processedLen)) {
            if (irFull) delete[] irFull;
            gProcessedIR.assign(processedIR, processedIR + processedLen);
            if (processedIR) delete[] processedIR;
            gIrLength = processedLen;
            gTrimStart = 0;
            gTrimEnd = processedLen;
            gProcessingProgress.store(1.0f);
            gAppState.store(REVIEW);
            LOGI("Processing complete: %d IR samples", processedLen);
        } else {
            if (irFull) delete[] irFull;
            LOGE("IR processing failed");
            gAppState.store(IDLE);
        }
        
        return JNI_TRUE;
    }
    
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_impulser_engine_NativeEngine_nativeRunCalibration(JNIEnv* env, jobject thiz) {
    if (gAppState.load() != IDLE) {
        LOGE("Cannot calibrate: not in IDLE state");
        return JNI_FALSE;
    }
    
    if (!gCalibrationFilter || !gOboeEngine) {
        LOGE("Calibration filter or Oboe engine not initialized");
        return JNI_FALSE;
    }
    
    LOGI("Running calibration...");
    gAppState.store(CALIBRATING);
    
    bool success = gCalibrationFilter->runCalibration(*gOboeEngine);
    
    if (success) {
        gAppState.store(IDLE);
        LOGI("Calibration completed successfully");
    } else {
        gAppState.store(IDLE);
        LOGE("Calibration failed");
    }
    
    return success ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
