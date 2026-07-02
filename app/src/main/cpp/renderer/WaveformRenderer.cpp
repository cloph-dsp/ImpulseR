#include "WaveformRenderer.h"
#include <algorithm>
#include <android/log.h>

#define LOG_TAG "WaveformRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace impulser {

// Vertex shader for waveform
const char* WaveformRenderer::kVertexShaderSource = R"(
#version 300 es
layout(location = 0) in vec2 aPosition;
uniform float uZoomStart;
uniform float uZoomEnd;

void main() {
    // Map sample index to NDC X
    float x = (aPosition.x - uZoomStart) / (uZoomEnd - uZoomStart) * 2.0 - 1.0;
    
    // Map amplitude to NDC Y
    float y = aPosition.y;
    
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)";

// Fragment shader for waveform
const char* WaveformRenderer::kFragmentShaderSource = R"(
#version 300 es
precision highp float;
uniform vec4 uColor;
out vec4 fragColor;

void main() {
    fragColor = uColor;
}
)";

WaveformRenderer::WaveformRenderer() {
}

WaveformRenderer::~WaveformRenderer() {
    if (mShaderProgram) {
        glDeleteProgram(mShaderProgram);
    }
    if (mVAO) {
        glDeleteVertexArrays(1, &mVAO);
    }
    if (mVBO) {
        glDeleteBuffers(1, &mVBO);
    }
    if (mTrimVAO) {
        glDeleteVertexArrays(1, &mTrimVAO);
    }
    if (mTrimVBO) {
        glDeleteBuffers(1, &mTrimVBO);
    }
}

bool WaveformRenderer::initialize() {
    LOGI("Initializing WaveformRenderer");

    // Create shader program
    if (!createShaderProgram()) {
        LOGE("Failed to create shader program");
        return false;
    }

    // Create geometry
    createGeometry();

    LOGI("WaveformRenderer initialized successfully");
    return true;
}

void WaveformRenderer::render(int width, int height) {
    glViewport(0, 0, width, height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(mShaderProgram);

    // Set zoom window uniforms
    glUniform1f(mZoomStartUniform, static_cast<float>(mZoomStart));
    glUniform1f(mZoomEndUniform, static_cast<float>(mZoomEnd));

    // Set waveform color (white)
    glUniform4f(mColorUniform, 1.0f, 1.0f, 1.0f, 1.0f);

    // Render waveform
    {
        std::lock_guard<std::mutex> lock(mDataMutex);
        if (!mVertexBuffer.empty()) {
            glBindVertexArray(mVAO);
            glDrawArrays(GL_LINES, 0, mVertexBuffer.size() / 2);
            glBindVertexArray(0);
        }
    }

    // Render trim markers if enabled
    if (mShowTrimMarkers) {
        renderTrimMarkers();
    }
}

void WaveformRenderer::updateWaveform(const float* samples, int numSamples) {
    if (!samples || numSamples <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mDataMutex);

    // Store waveform data
    mWaveformData.assign(samples, samples + numSamples);

    // Update zoom window if not set
    if (mZoomEnd == 0) {
        mZoomEnd = numSamples;
    }

    // Update vertex buffer
    updateVertexBuffer();
}

void WaveformRenderer::updateData(const float* samples, int numSamples, int trimStart, int trimEnd) {
    updateWaveform(samples, numSamples);
    setTrimMarkers(trimStart, trimEnd);
    mShowTrimMarkers = true;
}

bool WaveformRenderer::initialize(ANativeWindow* window) {
    return initialize();
}

void WaveformRenderer::setTrimMarkers(int start, int end) {
    mTrimStart = start;
    mTrimEnd = end;
}

void WaveformRenderer::setZoomWindow(int start, int end) {
    mZoomStart = start;
    mZoomEnd = end;
}

bool WaveformRenderer::createShaderProgram() {
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
    mZoomStartUniform = glGetUniformLocation(mShaderProgram, "uZoomStart");
    mZoomEndUniform = glGetUniformLocation(mShaderProgram, "uZoomEnd");
    mColorUniform = glGetUniformLocation(mShaderProgram, "uColor");

    return true;
}

void WaveformRenderer::createGeometry() {
    // Create VAO and VBO for waveform
    glGenVertexArrays(1, &mVAO);
    glGenBuffers(1, &mVBO);

    glBindVertexArray(mVAO);

    glBindBuffer(GL_ARRAY_BUFFER, mVBO);
    // Allocate with initial size (will be updated later)
    glBufferData(GL_ARRAY_BUFFER, 1024 * 2 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glBindVertexArray(0);

    // Create VAO and VBO for trim markers
    glGenVertexArrays(1, &mTrimVAO);
    glGenBuffers(1, &mTrimVBO);

    glBindVertexArray(mTrimVAO);

    glBindBuffer(GL_ARRAY_BUFFER, mTrimVBO);
    // 4 vertices for 2 vertical lines (start and end markers)
    glBufferData(GL_ARRAY_BUFFER, 4 * 2 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    glBindVertexArray(0);
}

void WaveformRenderer::updateVertexBuffer() {
    if (mWaveformData.empty()) {
        return;
    }

    // Convert waveform to line strip vertices
    int numSamples = mWaveformData.size();
    mVertexBuffer.resize(numSamples * 2); // 2 components per vertex (x, y)

    for (int i = 0; i < numSamples; ++i) {
        mVertexBuffer[i * 2 + 0] = static_cast<float>(i); // x = sample index
        mVertexBuffer[i * 2 + 1] = mWaveformData[i];      // y = amplitude
    }

    // Update VBO
    glBindBuffer(GL_ARRAY_BUFFER, mVBO);
    glBufferData(GL_ARRAY_BUFFER, mVertexBuffer.size() * sizeof(float),
                 mVertexBuffer.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void WaveformRenderer::renderTrimMarkers() {
    if (mTrimStart == mTrimEnd) {
        return;
    }

    // Update trim marker vertices
    float trimVertices[] = {
        static_cast<float>(mTrimStart), -1.0f, // Start marker bottom
        static_cast<float>(mTrimStart),  1.0f, // Start marker top
        static_cast<float>(mTrimEnd),   -1.0f, // End marker bottom
        static_cast<float>(mTrimEnd),    1.0f  // End marker top
    };

    glBindBuffer(GL_ARRAY_BUFFER, mTrimVBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(trimVertices), trimVertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Set trim marker color (orange)
    glUniform4f(mColorUniform, 1.0f, 0.5f, 0.0f, 1.0f);

    // Render trim markers
    glBindVertexArray(mTrimVAO);
    glDrawArrays(GL_LINES, 0, 4);
    glBindVertexArray(0);
}

} // namespace impulser
