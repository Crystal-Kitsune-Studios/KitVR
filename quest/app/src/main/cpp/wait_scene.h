#pragma once

#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

class WaitScene {
public:
    bool initialize(XrInstance instance, XrSystemId system, XrSession session);
    const XrCompositionLayerBaseHeader* render(XrSession session, XrSpace space, XrTime time);
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
    GLint sizeUniform_ = -1;
    uint64_t startUs_ = 0;
    bool renderFailed_ = false;
};
