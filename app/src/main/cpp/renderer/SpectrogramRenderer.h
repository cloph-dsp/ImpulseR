#pragma once

#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace impulser {

class SpectrogramRenderer {
public:
    SpectrogramRenderer();

    ~SpectrogramRenderer();

    bool initialize();

    bool initialize(ANativeWindow* window);

    GLuint getTextureId() const { return mSpectrogramTexture; }

    void updateData(const float* data, int numSamples);

    void render(int width, int height);

    void start();

    void stop();

    bool isRunning() const { return mIsRunning; }

private:
    void stftThread();
    void computeSTFT(const float* input, float* output);
    void createColormapTexture();
    void createSpectrogramTexture();
    bool createShaderProgram();
    void createGeometry();

    static constexpr int kFFTSize = 1024;
    static constexpr int kHopSize = 512;
    static constexpr int kNumBins = kFFTSize / 2;
    static constexpr int kTextureWidth = 1024;
    static constexpr int kTextureHeight = kNumBins;

    int mSampleRate = 48000;

    GLuint mShaderProgram = 0;
    GLuint mVAO = 0;
    GLuint mVBO = 0;
    GLuint mSpectrogramTexture = 0;
    GLuint mColormapTexture = 0;

    GLint mScrollOffsetUniform = -1;
    GLint mSpectrogramUniform = -1;
    GLint mColormapUniform = -1;

    std::vector<float> mSTFTBuffer;
    std::vector<float> mWindow;
    std::vector<float> mFFTBuffer;
    std::vector<float> mMagnitude;

    std::thread mSTFTThread;
    std::atomic<bool> mIsRunning{false};
    std::mutex mTextureMutex;

    int mCurrentColumn = 0;
    float mScrollOffset = 0.0f;
};

} // namespace impulser