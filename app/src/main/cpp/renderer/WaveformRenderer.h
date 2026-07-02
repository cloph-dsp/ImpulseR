#pragma once

#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <vector>
#include <mutex>

namespace impulser {

/**
 * Waveform renderer for impulse response display.
 * 
 * Renders IR waveform as GL_LINES with trim markers.
 * Supports both live monitoring and final IR display.
 */
class WaveformRenderer {
public:
    WaveformRenderer();
    ~WaveformRenderer();

    /**
     * Initialize OpenGL resources.
     * 
     * @return true if initialization succeeded
     */
    bool initialize();

    /**
     * Render the waveform.
     * 
     * @param width Viewport width
     * @param height Viewport height
     */
    void render(int width, int height);

    /**
     * Update the waveform data.
     * 
     * @param samples Pointer to sample data
     * @param numSamples Number of samples
     */
    void updateWaveform(const float* samples, int numSamples);

    /**
     * Update data with trim markers (for JNI).
     */
    void updateData(const float* samples, int numSamples, int trimStart, int trimEnd);

    bool initialize(ANativeWindow* window);

    /**
     * Set trim marker positions.
     * 
     * @param start Trim start position in samples
     * @param end Trim end position in samples
     */
    void setTrimMarkers(int start, int end);

    /**
     * Set zoom window for display.
     * 
     * @param start Start sample index
     * @param end End sample index
     */
    void setZoomWindow(int start, int end);

    /**
     * Set whether to show trim markers.
     * 
     * @param show true to show markers
     */
    void setShowTrimMarkers(bool show) { mShowTrimMarkers = show; }

private:
    /**
     * Compile and link the shader program.
     * 
     * @return true if compilation succeeded
     */
    bool createShaderProgram();

    /**
     * Create vertex buffer and vertex array.
     */
    void createGeometry();

    /**
     * Update the vertex buffer with current waveform data.
     */
    void updateVertexBuffer();

    /**
     * Render trim markers.
     */
    void renderTrimMarkers();

    // OpenGL resources
    GLuint mShaderProgram = 0;
    GLuint mVAO = 0;
    GLuint mVBO = 0;
    GLuint mTrimVAO = 0;
    GLuint mTrimVBO = 0;

    // Uniform locations
    GLint mZoomStartUniform = -1;
    GLint mZoomEndUniform = -1;
    GLint mColorUniform = -1;

    // Waveform data
    std::vector<float> mWaveformData;
    std::vector<float> mVertexBuffer;
    std::mutex mDataMutex;

    // Trim markers
    int mTrimStart = 0;
    int mTrimEnd = 0;
    bool mShowTrimMarkers = false;

    // Zoom window
    int mZoomStart = 0;
    int mZoomEnd = 0;

    // Shader sources
    static const char* kVertexShaderSource;
    static const char* kFragmentShaderSource;
};

} // namespace impulser
