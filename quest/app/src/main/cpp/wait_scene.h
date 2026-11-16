#pragma once

#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

// Immersive space scene shown while the client connects. It is fixed in `space`, so the user can
// look around it, and fades out with a warp effect over the first video frames.
class WaitScene {
public:
    enum class Phase : int32_t { Connecting = 0, StartingStream = 1, Connected = 2 };

    // How long the scene keeps fading over the video after the first frame.
    static constexpr float kRevealSeconds = 1.2f;

    bool initialize(XrInstance instance, XrSystemId system, XrSession session);
    // `reveal` runs 0..1 during Phase::Connected; the layer is alpha blended over earlier layers.
    const XrCompositionLayerBaseHeader* render(XrSession session, XrSpace space, XrTime time,
                                               Phase phase, float reveal);
    void destroy();

private:
    struct Eye {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
    };

    std::vector<Eye> eyes_;
    std::vector<XrView> views_;
    std::vector<XrCompositionLayerProjectionView> projectionViews_;
    XrCompositionLayerProjection layer_{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    GLuint program_ = 0;
    GLuint framebuffer_ = 0;
    GLuint vertexArray_ = 0;
    GLint timeUniform_ = -1;
    GLint phaseUniform_ = -1;
    GLint phaseTimeUniform_ = -1;
    GLint revealUniform_ = -1;
    GLint sizeUniform_ = -1;
    GLint tangentsUniform_ = -1;
    GLint orientationUniform_ = -1;
    GLint eyePositionUniform_ = -1;
    GLint textUniform_ = -1;
    GLint textLengthUniform_ = -1;
    uint64_t startUs_ = 0;
    uint64_t phaseStartUs_ = 0;
    Phase phase_ = Phase::Connecting;
    bool renderFailed_ = false;
};
