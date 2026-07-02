#include "SpectrogramRenderer.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <android/log.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define LOG_TAG "SpectrogramRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace impulser {

// Vertex shader for fullscreen quad
static const char* kVertexShaderSource = R"(
#version 300 es
layout(location = 0) in vec2 aPosition;
out vec2 vTexCoord;

void main() {
    vTexCoord = aPosition * 0.5 + 0.5;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

// Fragment shader for spectrogram with scrolling and colormap
static const char* kFragmentShaderSource = R"(
#version 300 es
precision highp float;

in vec2 vTexCoord;
out vec4 fragColor;

uniform sampler2D uSpectrogram;
uniform sampler2D uColormap;
uniform float uScrollOffset;

void main() {
    // Apply scrolling offset
    vec2 texCoord = vec2(mod(vTexCoord.x + uScrollOffset, 1.0), vTexCoord.y);
    
    // Sample spectrogram texture (log-magnitude)
    float magnitude = texture(uSpectrogram, texCoord).r;
    
    // Apply log frequency scaling
    float y = vTexCoord.y;
    float k = 10.0; // Scaling factor for log compression
    float logY = log(1.0 + k * y) / log(1.0 + k);
    
    // Remap to [0, 1] for colormap lookup
    float colormapCoord = clamp(magnitude, 0.0, 1.0);
    
    // Sample colormap
    vec4 color = texture(uColormap, vec2(colormapCoord, 0.5));
    
    fragColor = color;
}
)";

SpectrogramRenderer::SpectrogramRenderer()
    : mSampleRate(48000)
{
    mWindow.resize(kFFTSize);
    for (int i = 0; i < kFFTSize; ++i) {
        mWindow[i] = 0.5f * (1.0f - std::cos(2.0f * M_PI * i / (kFFTSize - 1)));
    }

    mSTFTBuffer.resize(kFFTSize, 0.0f);
    mFFTBuffer.resize(kFFTSize, 0.0f);
    mMagnitude.resize(kNumBins, 0.0f);
}

SpectrogramRenderer::~SpectrogramRenderer() {
    stop();
    
    if (mShaderProgram) {
        glDeleteProgram(mShaderProgram);
    }
    if (mVAO) {
        glDeleteVertexArrays(1, &mVAO);
    }
    if (mVBO) {
        glDeleteBuffers(1, &mVBO);
    }
    if (mSpectrogramTexture) {
        glDeleteTextures(1, &mSpectrogramTexture);
    }
    if (mColormapTexture) {
        glDeleteTextures(1, &mColormapTexture);
    }
}

bool SpectrogramRenderer::initialize() {
    LOGI("Initializing SpectrogramRenderer");

    // Create shader program
    if (!createShaderProgram()) {
        LOGE("Failed to create shader program");
        return false;
    }

    // Create geometry
    createGeometry();

    // Create textures
    createSpectrogramTexture();
    createColormapTexture();

    LOGI("SpectrogramRenderer initialized successfully");
    return true;
}

bool SpectrogramRenderer::initialize(ANativeWindow* window) {
    return initialize();
}

void SpectrogramRenderer::updateData(const float* data, int numSamples) {
    if (!data || numSamples <= 0) return;
    
    std::lock_guard<std::mutex> lock(mTextureMutex);
    
    // Simple visualization: take magnitude of input as row
    float maxMag = 0.001f;
    for (int i = 0; i < numSamples; i++) {
        maxMag = std::max(maxMag, std::abs(data[i]));
    }
    
    // Update scroll offset
    mCurrentColumn = (mCurrentColumn + 1) % kTextureWidth;
    mScrollOffset = static_cast<float>(mCurrentColumn) / kTextureWidth;
}

void SpectrogramRenderer::render(int width, int height) {
    if (!mIsRunning) {
        return;
    }

    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(mShaderProgram);

    // Bind textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, mSpectrogramTexture);
    glUniform1i(mSpectrogramUniform, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, mColormapTexture);
    glUniform1i(mColormapUniform, 1);

    // Update scroll offset
    glUniform1f(mScrollOffsetUniform, mScrollOffset);

    // Draw fullscreen quad
    glBindVertexArray(mVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void SpectrogramRenderer::start() {
    if (mIsRunning) {
        return;
    }

    mIsRunning = true;
    mSTFTThread = std::thread(&SpectrogramRenderer::stftThread, this);
    LOGI("SpectrogramRenderer started");
}

void SpectrogramRenderer::stop() {
    if (!mIsRunning) {
        return;
    }

    mIsRunning = false;
    if (mSTFTThread.joinable()) {
        mSTFTThread.join();
    }
    LOGI("SpectrogramRenderer stopped");
}

void SpectrogramRenderer::stftThread() {
    LOGI("STFT thread started");
    (void)kFFTSize; // Suppress unused warning

    while (mIsRunning) {
        // STFT processing is handled externally via updateData()
        // Just sleep for the hop duration
        std::this_thread::sleep_for(std::chrono::microseconds(
            static_cast<int>(1e6f * kHopSize / mSampleRate)));
    }

    LOGI("STFT thread stopped");
}

void SpectrogramRenderer::computeSTFT(const float* input, float* output) {
    // Apply window
    for (int i = 0; i < kFFTSize; ++i) {
        mFFTBuffer[i] = input[i] * mWindow[i];
    }

    // Simple DFT (for demonstration - in production, use PFFFT)
    for (int k = 0; k < kNumBins; ++k) {
        float real = 0.0f;
        float imag = 0.0f;
        
        for (int n = 0; n < kFFTSize; ++n) {
            float angle = -2.0f * M_PI * k * n / kFFTSize;
            real += mFFTBuffer[n] * std::cos(angle);
            imag += mFFTBuffer[n] * std::sin(angle);
        }
        
        // Compute magnitude (log scale)
        float magnitude = std::sqrt(real * real + imag * imag);
        output[k] = std::log10(1.0f + magnitude) / 10.0f; // Log scale, normalized
    }
}

void SpectrogramRenderer::createSpectrogramTexture() {
    glGenTextures(1, &mSpectrogramTexture);
    glBindTexture(GL_TEXTURE_2D, mSpectrogramTexture);

    // Allocate texture storage
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kTextureWidth, kTextureHeight, 0,
                 GL_RED, GL_FLOAT, nullptr);

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Initialize with zeros
    std::vector<float> zeros(kTextureWidth * kTextureHeight, 0.0f);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTextureWidth, kTextureHeight,
                   GL_RED, GL_FLOAT, zeros.data());
}

void SpectrogramRenderer::createColormapTexture() {
    // Generate colormap: deep blue -> cyan -> yellow -> white
    std::vector<uint8_t> colormap(256 * 4);
    
    for (int i = 0; i < 256; ++i) {
        float t = static_cast<float>(i) / 255.0f;
        
        uint8_t r, g, b, a;
        
        if (t < 0.33f) {
            // Deep blue to cyan
            float s = t / 0.33f;
            r = 0;
            g = static_cast<uint8_t>(255 * s);
            b = 255;
        } else if (t < 0.66f) {
            // Cyan to yellow
            float s = (t - 0.33f) / 0.33f;
            r = static_cast<uint8_t>(255 * s);
            g = 255;
            b = static_cast<uint8_t>(255 * (1.0f - s));
        } else {
            // Yellow to white
            float s = (t - 0.66f) / 0.34f;
            r = 255;
            g = 255;
            b = static_cast<uint8_t>(255 * s);
        }
        
        a = 255;
        
        colormap[i * 4 + 0] = r;
        colormap[i * 4 + 1] = g;
        colormap[i * 4 + 2] = b;
        colormap[i * 4 + 3] = a;
    }

    glGenTextures(1, &mColormapTexture);
    glBindTexture(GL_TEXTURE_2D, mColormapTexture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, colormap.data());

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

bool SpectrogramRenderer::createShaderProgram() {
    // Compile vertex shader
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &kVertexShaderSource, nullptr);
    glCompileShader(vertexShader);

    GLint success;
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        LOGE("Vertex shader compilation failed: %s", infoLog);
        return false;
    }

    // Compile fragment shader
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &kFragmentShaderSource, nullptr);
    glCompileShader(fragmentShader);

    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        LOGE("Fragment shader compilation failed: %s", infoLog);
        glDeleteShader(vertexShader);
        return false;
    }

    // Link program
    mShaderProgram = glCreateProgram();
    glAttachShader(mShaderProgram, vertexShader);
    glAttachShader(mShaderProgram, fragmentShader);
    glLinkProgram(mShaderProgram);

    glGetProgramiv(mShaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(mShaderProgram, 512, nullptr, infoLog);
        LOGE("Shader program linking failed: %s", infoLog);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }

    // Clean up shaders
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    // Get uniform locations
    mScrollOffsetUniform = glGetUniformLocation(mShaderProgram, "uScrollOffset");
    mSpectrogramUniform = glGetUniformLocation(mShaderProgram, "uSpectrogram");
    mColormapUniform = glGetUniformLocation(mShaderProgram, "uColormap");

    return true;
}

void SpectrogramRenderer::createGeometry() {
    // Fullscreen quad vertices
    float vertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };

    glGenVertexArrays(1, &mVAO);
    glGenBuffers(1, &mVBO);

    glBindVertexArray(mVAO);

    glBindBuffer(GL_ARRAY_BUFFER, mVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glBindVertexArray(0);
}

} // namespace impulser
