#include "wait_scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <cstdint>
#include <vector>

#include "log.h"
#include "proto.h"
#include "wait_logo.h"

namespace {

const char* kVertexShader = R"(#version 300 es
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Shared declarations come first so the logo slot in wait_logo.h can use them.
const char* kFragmentHeader = R"(#version 300 es
precision highp float;
precision highp int;
)";

const char* kFragmentShader = R"(
uniform float uTime;
uniform float uPhaseTime;
uniform int uPhase;
uniform float uReveal;
uniform vec2 uSize;
uniform vec4 uTangents;     // tan of the left, right, down and up field of view angles
uniform vec4 uOrientation;  // eye orientation quaternion in the scene space
uniform vec3 uEyePosition;
uniform int uText[20];
uniform int uTextLength;
out vec4 color;

// Panels sit on a plane in front of the starting head position, in metres.
const float kPanelDistance = 2.5;
const vec2 kLogoCenter = vec2(0.0, 0.24);
const float kLogoHalfSize = 0.40;
const vec2 kTextCenter = vec2(0.0, -0.24);
const float kTextCell = 0.017;

// 5x7 glyphs, top row first, for the letters C O N E T I G P S A R M D.
const uint kFont[91] = uint[91](
    14u,17u,16u,16u,16u,17u,14u,  14u,17u,17u,17u,17u,17u,14u,  17u,17u,25u,21u,19u,17u,17u,
    31u,16u,16u,30u,16u,16u,31u,  31u,4u,4u,4u,4u,4u,4u,         14u,4u,4u,4u,4u,4u,14u,
    14u,17u,16u,23u,17u,17u,15u,  30u,17u,17u,30u,16u,16u,16u,  15u,16u,16u,14u,1u,1u,30u,
    14u,17u,17u,31u,17u,17u,17u,  30u,17u,17u,30u,20u,18u,17u,  17u,27u,21u,21u,17u,17u,17u,
    28u,18u,17u,17u,17u,18u,28u);

vec3 rotate(vec4 q, vec3 v) {
    vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

float noise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i), hash13(i + vec3(1, 0, 0)), f.x),
                   mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x),
                   mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}

float fbm(vec3 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amplitude * noise(p);
        p = p * 2.03 + vec3(1.7, 9.2, 4.1);
        amplitude *= 0.5;
    }
    return sum;
}

// One layer of stars on the sky sphere. Each grid cell holds at most one star.
vec3 stars(vec3 dir, float scale, float density, float pixel, float seed) {
    vec3 p = dir * scale;
    vec3 cell = floor(p);
    vec3 h = hash33(cell + seed);
    if (hash13(cell + seed + 7.1) > density) return vec3(0.0);
    vec3 star = normalize(cell + 0.2 + 0.6 * h) * scale;
    float size = pixel * scale * mix(0.9, 2.4, h.x * h.x * h.x);
    float core = 1.0 - smoothstep(0.0, size, length(p - star));
    float twinkle = 0.65 + 0.35 * sin(uTime * (0.8 + 2.5 * h.y) + h.z * 6.2831853);
    vec3 tint = mix(vec3(0.70, 0.82, 1.0), vec3(1.0, 0.82, 0.62), h.z);
    return tint * core * core * twinkle * mix(0.6, 2.2, h.x);
}

vec3 nebula(vec3 dir) {
    vec3 q = dir * 2.2 + vec3(0.0, 0.0, uTime * 0.012);
    float shape = fbm(q + 1.3 * fbm(q * 1.6 + 4.0));
    float band = exp(-pow(dot(dir, normalize(vec3(0.35, 1.0, 0.25))) * 2.6, 2.0));
    float density = smoothstep(0.35, 0.85, shape) * (0.25 + 0.75 * band);
    vec3 violet = vec3(0.20, 0.06, 0.32);
    vec3 teal = vec3(0.03, 0.20, 0.26);
    vec3 ember = vec3(0.85, 0.38, 0.12);
    vec3 tint = mix(violet, teal, smoothstep(0.3, 0.7, noise(q * 0.7 + 11.0)));
    tint = mix(tint, ember, smoothstep(0.72, 0.95, shape) * 0.6);
    return vec3(0.004, 0.005, 0.014) + tint * density * 0.55;
}

// A ringless planet low on the left, half lit from the side, with a thin atmosphere.
vec3 planet(vec3 dir, inout float covered) {
    const vec3 center = normalize(vec3(-0.80, -0.38, -0.55));
    const float radius = 0.30;
    vec3 side = normalize(cross(center, vec3(0.0, 1.0, 0.0)));
    vec3 up = cross(side, center);
    vec2 local = vec2(dot(dir, side), dot(dir, up)) / sin(radius);
    float r = length(local);
    if (dot(dir, center) <= 0.0) return vec3(0.0);
    vec3 light = normalize(vec3(0.3, 0.6, -0.75));
    if (r < 1.0) {
        covered = 1.0;
        vec3 normal = local.x * side + local.y * up - sqrt(1.0 - r * r) * center;
        float diffuse = smoothstep(-0.15, 0.7, dot(normal, light));
        float bands = 0.5 + 0.5 * sin(local.y * 14.0 + 3.0 * noise(normal * 3.0 + uTime * 0.02));
        vec3 surface = mix(vec3(0.10, 0.12, 0.30), vec3(0.32, 0.22, 0.45), bands);
        float rim = pow(1.0 - sqrt(1.0 - r * r), 3.0);
        return surface * (0.03 + diffuse) + vec3(0.25, 0.55, 0.65) * rim * (0.2 + diffuse);
    }
    float lit = max(dot(normalize(local.x * side + local.y * up), light), 0.0);
    return vec3(0.20, 0.50, 0.65) * exp(-(r - 1.0) * 28.0) * (0.15 + 0.85 * lit) * 0.6;
}

// Streaks rushing outwards from straight ahead while the stream takes over.
float warp(vec3 dir, float amount) {
    if (amount <= 0.0) return 0.0;
    float fromForward = acos(clamp(-dir.z, -1.0, 1.0));
    float around = atan(dir.y, dir.x) / 6.2831853 + 0.5;
    float lane = floor(around * 220.0);
    float h = hash13(vec3(lane, 3.7, 1.3));
    float across = abs(fract(around * 220.0) - 0.5);
    float thin = 1.0 - smoothstep(0.08, 0.30, across);
    float along = fract(h * 7.0 + fromForward * 1.2 - uPhaseTime * (1.5 + 2.5 * h));
    float dash = smoothstep(0.0, 0.5, along) * (1.0 - smoothstep(0.55, 0.62, along));
    return thin * dash * step(0.7, h) * smoothstep(0.03, 0.35, fromForward) * amount;
}

float text(vec2 plane) {
    int columns = uTextLength * 6 - 1;
    vec2 g = plane / kTextCell + vec2(float(columns) * 0.5, 3.5);
    if (g.x < 0.0 || g.y < 0.0) return 0.0;
    int col = int(floor(g.x));
    int row = 6 - int(floor(g.y));
    if (col >= columns || row < 0 || col % 6 == 5) return 0.0;
    int letter = uText[col / 6];
    if (letter < 0) return 0.0;
    return float((kFont[letter * 7 + row] >> uint(4 - col % 6)) & 1u);
}

vec4 kitvrLogo(vec2 uv, float time, int phase);

void main() {
    vec2 uv = gl_FragCoord.xy / uSize;
    vec3 viewDir = vec3(mix(uTangents.x, uTangents.y, uv.x), mix(uTangents.z, uTangents.w, uv.y), -1.0);
    vec3 dir = normalize(rotate(uOrientation, normalize(viewDir)));
    float pixel = (uTangents.y - uTangents.x) / uSize.x;
    float reveal = uPhase == 2 ? clamp(uReveal, 0.0, 1.0) : 0.0;

    vec3 sky = nebula(dir);
    float covered = 0.0;
    vec3 world = planet(dir, covered);
    vec3 starLight = stars(dir, 45.0, 0.22, pixel, 0.0) + stars(dir, 120.0, 0.10, pixel, 19.0) * 0.35;
    vec3 col = sky * (1.0 - covered) + world + starLight * (1.0 - covered);

    float rush = smoothstep(0.0, 0.35, reveal);
    col += vec3(0.80, 0.88, 1.0) * warp(dir, rush) * 1.4;
    float fromForward = acos(clamp(-dir.z, -1.0, 1.0));
    col += vec3(1.0, 0.85, 0.70) * exp(-fromForward * 5.0) * rush * (1.0 - reveal) * 1.5;

    // Panels are on a vertical plane, so intersect the eye ray with it for correct stereo depth.
    if (dir.z < -0.01) {
        float t = (-kPanelDistance - uEyePosition.z) / dir.z;
        vec2 plane = uEyePosition.xy + dir.xy * t;
        float panelFade = 1.0 - smoothstep(0.0, 0.3, reveal);

        vec2 logoUv = (plane - kLogoCenter) / kLogoHalfSize;
        if (abs(logoUv.x) <= 1.0 && abs(logoUv.y) <= 1.0) {
            vec4 logo = kitvrLogo(logoUv, uTime, uPhase) * panelFade;
            col = col * (1.0 - logo.a) + logo.rgb;
        }

        float letters = text(plane - kTextCenter);
        col = mix(col, vec3(0.88, 0.92, 1.0), letters * panelFade);

        for (int i = 0; i < 3; ++i) {
            vec2 dotCenter = kTextCenter + vec2((float(i) - 1.0) * 0.045, -0.10);
            float dotShape = 1.0 - smoothstep(0.009, 0.014, length(plane - dotCenter));
            float pulse = 0.30 + 0.70 * (0.5 + 0.5 * sin(uTime * 3.0 - float(i) * 0.8));
            col += vec3(0.95, 0.55, 0.25) * dotShape * pulse * panelFade;
        }
    }

    float alpha = smoothstep(0.0, 1.0, uTime) * (1.0 - smoothstep(0.45, 1.0, reveal));
    color = vec4(col * alpha, alpha);
}
)";

constexpr const char* kLetters = "CONETIGPSARMD";
constexpr int kMaxTextLength = 20;

const char* phaseText(WaitScene::Phase phase) {
    switch (phase) {
        case WaitScene::Phase::Connecting: return "CONNECTING TO PC";
        case WaitScene::Phase::StartingStream: return "STARTING STREAM";
        case WaitScene::Phase::Connected: return "CONNECTED";
    }
    return "";
}

GLuint compileShader(GLenum type, std::initializer_list<const char*> sources) {
    GLuint shader = glCreateShader(type);
    std::vector<const char*> parts(sources);
    glShaderSource(shader, static_cast<GLsizei>(parts.size()), parts.data(), nullptr);
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

    GLuint vertex = compileShader(GL_VERTEX_SHADER, {kVertexShader});
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER,
                                    {kFragmentHeader, kWaitLogoShader, kFragmentShader});
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
    phaseUniform_ = glGetUniformLocation(program_, "uPhase");
    phaseTimeUniform_ = glGetUniformLocation(program_, "uPhaseTime");
    revealUniform_ = glGetUniformLocation(program_, "uReveal");
    sizeUniform_ = glGetUniformLocation(program_, "uSize");
    tangentsUniform_ = glGetUniformLocation(program_, "uTangents");
    orientationUniform_ = glGetUniformLocation(program_, "uOrientation");
    eyePositionUniform_ = glGetUniformLocation(program_, "uEyePosition");
    textUniform_ = glGetUniformLocation(program_, "uText");
    textLengthUniform_ = glGetUniformLocation(program_, "uTextLength");
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

const XrCompositionLayerBaseHeader* WaitScene::render(XrSession session, XrSpace space, XrTime time,
                                                     Phase phase, float reveal) {
    if (!program_ || eyes_.empty() || renderFailed_) return nullptr;
    const uint64_t now = kitvr::nowUs();
    if (phase != phase_ || phaseStartUs_ == 0) {
        phase_ = phase;
        phaseStartUs_ = now;
    }
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
    glUniform1f(timeUniform_, static_cast<float>(now - startUs_) / 1000000.0f);
    glUniform1f(phaseTimeUniform_, static_cast<float>(now - phaseStartUs_) / 1000000.0f);
    glUniform1i(phaseUniform_, static_cast<GLint>(phase));
    glUniform1f(revealUniform_, reveal);
    GLint text[kMaxTextLength];
    const char* label = phaseText(phase);
    const GLint textLength = static_cast<GLint>(std::min<size_t>(std::strlen(label), kMaxTextLength));
    for (GLint i = 0; i < textLength; ++i) {
        const char* letter = std::strchr(kLetters, label[i]);
        text[i] = label[i] != ' ' && letter ? static_cast<GLint>(letter - kLetters) : -1;
    }
    glUniform1iv(textUniform_, textLength, text);
    glUniform1i(textLengthUniform_, textLength);
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
            const XrView& view = views_[i];
            glUniform2f(sizeUniform_, static_cast<float>(eye.width), static_cast<float>(eye.height));
            glUniform4f(tangentsUniform_, std::tan(view.fov.angleLeft), std::tan(view.fov.angleRight),
                        std::tan(view.fov.angleDown), std::tan(view.fov.angleUp));
            glUniform4f(orientationUniform_, view.pose.orientation.x, view.pose.orientation.y,
                        view.pose.orientation.z, view.pose.orientation.w);
            glUniform3f(eyePositionUniform_, view.pose.position.x, view.pose.position.y,
                        view.pose.position.z);
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
    // The shader writes premultiplied alpha so the scene can fade out over the video.
    layer_.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
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
