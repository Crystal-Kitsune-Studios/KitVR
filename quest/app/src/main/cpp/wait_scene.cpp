#include "wait_scene.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "log.h"
#include "proto.h"

namespace {

const char* kVertexShader = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 300 es
precision highp float;
precision highp int;
uniform float uTime;
uniform vec2 uSize;
out vec4 color;

uint glyph(int letter, int row) {
    const uint C[7] = uint[7](14u,17u,16u,16u,16u,17u,14u);
    const uint O[7] = uint[7](14u,17u,17u,17u,17u,17u,14u);
    const uint N[7] = uint[7](17u,25u,25u,21u,19u,19u,17u);
    const uint E[7] = uint[7](31u,16u,16u,30u,16u,16u,31u);
    const uint T[7] = uint[7](31u,4u,4u,4u,4u,4u,4u);
    const uint I[7] = uint[7](14u,4u,4u,4u,4u,4u,14u);
    const uint G[7] = uint[7](14u,17u,16u,23u,17u,17u,15u);
    if (letter == 0 || letter == 5) return C[row];
    if (letter == 1) return O[row];
    if (letter == 2 || letter == 3 || letter == 8) return N[row];
    if (letter == 4) return E[row];
    if (letter == 6) return T[row];
    if (letter == 7) return I[row];
    return G[row];
}

void main() {
    vec2 p = (gl_FragCoord.xy - 0.5 * uSize) / uSize.y;
    float glow = exp(-3.2 * length(p - vec2(0.0, 0.08)));
    vec3 bg = mix(vec3(0.025, 0.045, 0.052), vec3(0.075, 0.105, 0.105), glow);
    float radius = length(p - vec2(0.0, 0.08));
    float ring = 1.0 - smoothstep(0.002, 0.005, abs(radius - 0.14));
    float angle = atan(p.y - 0.08, p.x);
    float sweep = pow(max(0.0, 0.5 + 0.5 * cos(angle - uTime * 1.2)), 5.0);
    bg += vec3(0.95, 0.55, 0.25) * ring * (0.18 + 0.82 * sweep);
    float inner = 1.0 - smoothstep(0.001, 0.003, abs(radius - 0.105));
    bg += vec3(0.35, 0.65, 0.70) * inner * 0.35;
    float core = 1.0 - smoothstep(0.015, 0.025, radius);
    bg += vec3(0.95, 0.60, 0.32) * core * (0.55 + 0.15 * sin(uTime * 2.0));

    // A compact bitmap label avoids depending on platform fonts or external assets.
    vec2 pixel = (p - vec2(-0.15, -0.15)) / 0.005;
    int col = int(floor(pixel.x));
    int row = 6 - int(floor(pixel.y));
    if (col >= 0 && col < 59 && row >= 0 && row < 7 && col % 6 < 5) {
        uint bits = glyph(col / 6, row);
        if ((bits & (1u << uint(4 - col % 6))) != 0u) bg = vec3(0.86, 0.90, 0.88);
    }
    for (int i = 0; i < 3; ++i) {
        float dot = 1.0 - smoothstep(0.003, 0.006,
            length(p - vec2((float(i) - 1.0) * 0.025, -0.205)));
        float pulse = 0.35 + 0.65 * (0.5 + 0.5 * sin(uTime * 3.0 - float(i) * 0.8));
        bg += vec3(0.95, 0.55, 0.25) * dot * pulse;
    }
    color = vec4(bg, 1.0);
}
)";

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char message[1024]{};
        glGetShaderInfoLog(shader, sizeof(message), nullptr, message);
        LOGE("waiting scene shader compilation failed: %s", message);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

bool WaitScene::initialize(XrInstance instance, XrSystemId system, XrSession session) {
    startUs_ = kitvr::nowUs();
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateViewConfigurationViews(instance, system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr)) || count == 0) {
        LOGE("cannot enumerate waiting scene views");
        return false;
    }
    std::vector<XrViewConfigurationView> configs(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (XR_FAILED(xrEnumerateViewConfigurationViews(instance, system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, configs.data()))) {
        LOGE("cannot read waiting scene view configuration");
        return false;
    }
    uint32_t formatCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr)) ||
        formatCount == 0) {
        LOGE("cannot enumerate waiting scene swapchain formats");
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (XR_FAILED(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()))) {
        LOGE("cannot read waiting scene swapchain formats");
        return false;
    }
    int64_t format = 0;
    for (int64_t candidate : {static_cast<int64_t>(GL_SRGB8_ALPHA8),
                              static_cast<int64_t>(GL_RGBA8)}) {
        if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) {
            format = candidate;
            break;
        }
    }
    if (!format) {
        LOGE("no compatible waiting scene swapchain format");
        return false;
    }

    GLuint vertex = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char message[1024]{};
        glGetProgramInfoLog(program_, sizeof(message), nullptr, message);
        LOGE("waiting scene shader link failed: %s", message);
        destroy();
        return false;
    }
    timeUniform_ = glGetUniformLocation(program_, "uTime");
    sizeUniform_ = glGetUniformLocation(program_, "uSize");
    glGenFramebuffers(1, &framebuffer_);
    glGenVertexArrays(1, &vertexArray_);

    eyes_.resize(count);
    views_.assign(count, {XR_TYPE_VIEW});
    projectionViews_.assign(count, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
    for (uint32_t i = 0; i < count; ++i) {
        Eye& eye = eyes_[i];
        // The procedural scene does not need the full resolution of the streamed video.
        float scale = std::min(1.0f, 1024.0f / std::max(configs[i].recommendedImageRectWidth,
                                                        configs[i].recommendedImageRectHeight));
        eye.width = std::max(1, static_cast<int32_t>(configs[i].recommendedImageRectWidth * scale));
        eye.height = std::max(1, static_cast<int32_t>(configs[i].recommendedImageRectHeight * scale));
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = eye.width;
        info.height = eye.height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(session, &info, &eye.swapchain))) {
            LOGE("cannot create waiting scene swapchain for eye %u", i);
            destroy();
            return false;
        }
        uint32_t imageCount = 0;
        XrResult result = xrEnumerateSwapchainImages(eye.swapchain, 0, &imageCount, nullptr);
        if (XR_FAILED(result) || !imageCount) {
            LOGE("cannot enumerate waiting scene images");
            destroy();
            return false;
        }
        eye.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        result = xrEnumerateSwapchainImages(eye.swapchain, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
        if (XR_FAILED(result)) {
            LOGE("cannot read waiting scene images");
            destroy();
            return false;
        }
    }
    return true;
}

const XrCompositionLayerBaseHeader* WaitScene::render(XrSession session, XrSpace space, XrTime time) {
    if (!program_ || eyes_.empty() || renderFailed_) return nullptr;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = time;
    locate.space = space;
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(session, &locate, &state, views_.size(), &count, views_.data())) ||
        count != eyes_.size() ||
        !(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) ||
        !(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) return nullptr;

    glUseProgram(program_);
    glBindVertexArray(vertexArray_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUniform1f(timeUniform_, static_cast<float>(kitvr::nowUs() - startUs_) / 1000000.0f);
    bool rendered = true;
    for (uint32_t i = 0; i < count; ++i) {
        Eye& eye = eyes_[i];
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t index = 0;
        if (XR_FAILED(xrAcquireSwapchainImage(eye.swapchain, &acquire, &index))) {
            LOGE("cannot acquire waiting scene image");
            rendered = false;
            break;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrWaitSwapchainImage(eye.swapchain, &wait))) {
            LOGE("cannot wait for waiting scene image");
            rendered = false;
            break;
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               eye.images[index].image, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LOGE("waiting scene framebuffer incomplete");
            rendered = false;
        } else {
            glViewport(0, 0, eye.width, eye.height);
            glUniform2f(sizeUniform_, static_cast<float>(eye.width), static_cast<float>(eye.height));
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glFlush();
        }
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(xrReleaseSwapchainImage(eye.swapchain, &release))) {
            LOGE("cannot release waiting scene image");
            rendered = false;
        }
        if (!rendered) break;
        projectionViews_[i].pose = views_[i].pose;
        projectionViews_[i].fov = views_[i].fov;
        projectionViews_[i].subImage.swapchain = eye.swapchain;
        projectionViews_[i].subImage.imageRect = {{0, 0}, {eye.width, eye.height}};
        projectionViews_[i].subImage.imageArrayIndex = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    if (!rendered) {
        renderFailed_ = true;
        return nullptr;
    }
    layer_.space = space;
    layer_.viewCount = count;
    layer_.views = projectionViews_.data();
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer_);
}

void WaitScene::destroy() {
    for (Eye& eye : eyes_) {
        if (eye.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(eye.swapchain);
    }
    eyes_.clear();
    if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    if (vertexArray_) glDeleteVertexArrays(1, &vertexArray_);
    if (program_) glDeleteProgram(program_);
    framebuffer_ = vertexArray_ = program_ = 0;
}
