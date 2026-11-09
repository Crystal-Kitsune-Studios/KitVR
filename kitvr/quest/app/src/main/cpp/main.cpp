// KitVR Quest client: OpenXR session that shows the streamed desktop on a quad layer.
//
// Video path: the server's HEVC stream is decoded by MediaCodec directly into the Surface of an
// XR_KHR_android_surface_swapchain, and that swapchain is submitted as a quad layer. We never
// render video ourselves, so there is no GL draw call and no extra copy.
// A tiny EGL context exists only because OpenXR needs a graphics binding to create a session.

#include <android/log.h>
#include <android/native_window_jni.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "log.h"
#include "session.h"

#define CHECK(call)                                                           \
    do {                                                                      \
        XrResult res_ = (call);                                               \
        if (XR_FAILED(res_)) {                                                \
            LOGE("%s failed: %d (line %d)", #call, static_cast<int>(res_), __LINE__); \
            return false;                                                     \
        }                                                                     \
    } while (0)

namespace {

constexpr float kQuadHeightM = 1.6f;
constexpr float kQuadDistanceM = 2.0f;
constexpr const char* kDefaultHost = "192.168.1.100";
constexpr const char* kHostFile = "/sdcard/Android/data/com.crystalkitsune.kitvr/files/host.txt";

struct AppState {
    android_app* app = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool sessionBegun = false;
    bool exitRequested = false;

    PFN_xrCreateSwapchainAndroidSurfaceKHR createSurfaceSwapchain = nullptr;
    bool hasFbSurfaceCreate = false;

    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig eglConfig = nullptr;
    EGLContext eglContext = EGL_NO_CONTEXT;
    EGLSurface eglSurface = EGL_NO_SURFACE;

    XrSwapchain videoSwapchain = XR_NULL_HANDLE;
    ANativeWindow* videoWindow = nullptr;

    bool haveCfg = false;
    kitvr::StreamConfig cfg;

    kitvr::Session net;
};

// ---------------------------------------------------------------- host lookup

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

std::string hostFromIntent(android_app* app) {
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return "";
    jobject activity = app->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getIntent = env->GetMethodID(activityClass, "getIntent", "()Landroid/content/Intent;");
    jobject intent = getIntent ? env->CallObjectMethod(activity, getIntent) : nullptr;
    std::string out;
    if (intent) {
        jclass intentClass = env->GetObjectClass(intent);
        jmethodID getStringExtra = env->GetMethodID(intentClass, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
        jstring key = env->NewStringUTF("host");
        jstring val = getStringExtra ? static_cast<jstring>(env->CallObjectMethod(intent, getStringExtra, key)) : nullptr;
        if (val) {
            const char* c = env->GetStringUTFChars(val, nullptr);
            if (c) out = c;
            env->ReleaseStringUTFChars(val, c);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    return trim(out);
}

std::string hostFromFile() {
    std::ifstream f(kHostFile);
    std::string s;
    if (f) std::getline(f, s);
    return trim(s);
}

std::string resolveHost(android_app* app) {
    std::string h = hostFromIntent(app);
    if (h.empty()) h = hostFromFile();
    if (h.empty()) {
        LOGW("no server host given (intent extra 'host' or %s), using %s", kHostFile, kDefaultHost);
        h = kDefaultHost;
    }
    return h;
}

// ---------------------------------------------------------------- OpenXR setup

bool initLoader(AppState& S) {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader)));
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = S.app->activity->vm;
    li.applicationContext = S.app->activity->clazz;
    CHECK(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li)));
    return true;
}

bool createInstance(AppState& S) {
    uint32_t n = 0;
    CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr));
    std::vector<XrExtensionProperties> props(n, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    CHECK(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data()));
    auto has = [&](const char* name) {
        for (const auto& p : props) {
            if (std::strcmp(p.extensionName, name) == 0) return true;
        }
        return false;
    };

    std::vector<const char*> exts;
    for (const char* required : {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                                 XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                 XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME}) {
        if (!has(required)) {
            LOGE("required OpenXR extension missing: %s", required);
            return false;
        }
        exts.push_back(required);
    }
    if (has(XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME)) {
        exts.push_back(XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME);
        S.hasFbSurfaceCreate = true;
    }

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = S.app->activity->vm;
    androidInfo.applicationActivity = S.app->activity->clazz;

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &androidInfo;
    std::strncpy(ci.applicationInfo.applicationName, "KitVR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "KitVR", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.enabledExtensionNames = exts.data();
    CHECK(xrCreateInstance(&ci, &S.instance));

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    CHECK(xrGetSystem(S.instance, &si, &S.systemId));

    CHECK(xrGetInstanceProcAddr(S.instance, "xrCreateSwapchainAndroidSurfaceKHR",
                                reinterpret_cast<PFN_xrVoidFunction*>(&S.createSurfaceSwapchain)));
    return true;
}

bool initEgl(AppState& S) {
    S.eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (S.eglDisplay == EGL_NO_DISPLAY || !eglInitialize(S.eglDisplay, nullptr, nullptr)) {
        LOGE("eglInitialize failed");
        return false;
    }
    const EGLint cfgAttrs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                               EGL_DEPTH_SIZE, 0, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint num = 0;
    if (!eglChooseConfig(S.eglDisplay, cfgAttrs, &S.eglConfig, 1, &num) || num < 1) {
        LOGE("no suitable EGL config");
        return false;
    }
    const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    S.eglContext = eglCreateContext(S.eglDisplay, S.eglConfig, EGL_NO_CONTEXT, ctxAttrs);
    const EGLint pbAttrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    S.eglSurface = eglCreatePbufferSurface(S.eglDisplay, S.eglConfig, pbAttrs);
    if (S.eglContext == EGL_NO_CONTEXT || S.eglSurface == EGL_NO_SURFACE ||
        !eglMakeCurrent(S.eglDisplay, S.eglSurface, S.eglSurface, S.eglContext)) {
        LOGE("EGL context setup failed");
        return false;
    }
    return true;
}

bool createSession(AppState& S) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    CHECK(xrGetInstanceProcAddr(S.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                reinterpret_cast<PFN_xrVoidFunction*>(&getReq)));
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    CHECK(getReq(S.instance, S.systemId, &req));

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = S.eglDisplay;
    binding.config = S.eglConfig;
    binding.context = S.eglContext;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = S.systemId;
    CHECK(xrCreateSession(S.instance, &sci, &S.session));

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    CHECK(xrCreateReferenceSpace(S.session, &rs, &S.localSpace));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    CHECK(xrCreateReferenceSpace(S.session, &rs, &S.viewSpace));
    return true;
}

// ---------------------------------------------------------------- video lifecycle

bool setupVideo(AppState& S) {
    XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    sc.width = S.cfg.width;
    sc.height = S.cfg.height;
    // format, sampleCount, faceCount, arraySize and mipCount must stay 0 for surface swapchains.

    XrAndroidSurfaceSwapchainCreateInfoFB fb{XR_TYPE_ANDROID_SURFACE_SWAPCHAIN_CREATE_INFO_FB};
    fb.createFlags = 0;
    if (S.hasFbSurfaceCreate) sc.next = &fb;

    jobject surface = nullptr;
    CHECK(S.createSurfaceSwapchain(S.session, &sc, &S.videoSwapchain, &surface));

    JNIEnv* env = nullptr;
    S.app->activity->vm->AttachCurrentThread(&env, nullptr);
    S.videoWindow = ANativeWindow_fromSurface(env, surface);
    if (!S.videoWindow) {
        LOGE("ANativeWindow_fromSurface failed");
        xrDestroySwapchain(S.videoSwapchain);
        S.videoSwapchain = XR_NULL_HANDLE;
        return false;
    }
    if (!S.net.startMedia(S.cfg, S.videoWindow)) {
        ANativeWindow_release(S.videoWindow);
        S.videoWindow = nullptr;
        xrDestroySwapchain(S.videoSwapchain);
        S.videoSwapchain = XR_NULL_HANDLE;
        return false;
    }
    LOGI("video ready: %ux%u", S.cfg.width, S.cfg.height);
    return true;
}

// Must run before xrEndSession: nothing may write to the surface once the session stops.
void teardownVideo(AppState& S) {
    S.net.stopMedia();
    if (S.videoSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(S.videoSwapchain);
        S.videoSwapchain = XR_NULL_HANDLE;
    }
    if (S.videoWindow) {
        ANativeWindow_release(S.videoWindow);
        S.videoWindow = nullptr;
    }
}

// ---------------------------------------------------------------- events and frames

void onSessionState(AppState& S, XrSessionState newState) {
    S.state = newState;
    LOGI("session state %d", static_cast<int>(newState));
    switch (newState) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            if (XR_SUCCEEDED(xrBeginSession(S.session, &bi))) S.sessionBegun = true;
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            teardownVideo(S);
            xrEndSession(S.session);
            S.sessionBegun = false;
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            S.exitRequested = true;
            break;
        default:
            break;
    }
}

void pollXrEvents(AppState& S) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(S.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            onSessionState(S, reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state);
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            S.exitRequested = true;
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void maintainVideo(AppState& S) {
    kitvr::StreamConfig c;
    if (S.net.takeConfig(c)) {
        bool changed = !S.haveCfg || c.codec != S.cfg.codec || c.width != S.cfg.width ||
                       c.height != S.cfg.height || c.fps != S.cfg.fps;
        if (changed) {
            if (S.videoSwapchain != XR_NULL_HANDLE) teardownVideo(S);
            S.cfg = c;
            S.haveCfg = true;
        }
    }
    bool visible = S.state == XR_SESSION_STATE_VISIBLE || S.state == XR_SESSION_STATE_FOCUSED;
    if (S.sessionBegun && visible && S.haveCfg && S.videoSwapchain == XR_NULL_HANDLE) {
        if (!setupVideo(S)) {
            S.haveCfg = false;  // wait for the server to send the config again
        }
    }
}

void renderFrame(AppState& S) {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(S.session, &wi, &fs))) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(S.session, &bi))) return;

    if (fs.shouldRender && S.net.mediaRunning()) {
        float head[7] = {0, 0, 0, 0, 0, 0, 1};
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(S.viewSpace, S.localSpace, fs.predictedDisplayTime, &loc)) &&
            (loc.locationFlags & need) == need) {
            head[0] = loc.pose.position.x;
            head[1] = loc.pose.position.y;
            head[2] = loc.pose.position.z;
            head[3] = loc.pose.orientation.x;
            head[4] = loc.pose.orientation.y;
            head[5] = loc.pose.orientation.z;
            head[6] = loc.pose.orientation.w;
        }
        S.net.sendTracking(kitvr::nowUs(), head);
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[1] = {nullptr};
    uint32_t layerCount = 0;
    if (fs.shouldRender && S.videoSwapchain != XR_NULL_HANDLE && S.cfg.height > 0) {
        quad.layerFlags = 0;
        quad.space = S.localSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = S.videoSwapchain;
        quad.subImage.imageRect.offset = {0, 0};
        quad.subImage.imageRect.extent = {static_cast<int32_t>(S.cfg.width), static_cast<int32_t>(S.cfg.height)};
        quad.subImage.imageArrayIndex = 0;
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = {0.0f, 0.0f, -kQuadDistanceM};
        quad.size = {kQuadHeightM * static_cast<float>(S.cfg.width) / static_cast<float>(S.cfg.height), kQuadHeightM};
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    xrEndFrame(S.session, &ei);
}

void cleanup(AppState& S) {
    S.net.shutdown();
    teardownVideo(S);
    if (S.viewSpace != XR_NULL_HANDLE) xrDestroySpace(S.viewSpace);
    if (S.localSpace != XR_NULL_HANDLE) xrDestroySpace(S.localSpace);
    if (S.session != XR_NULL_HANDLE) xrDestroySession(S.session);
    if (S.instance != XR_NULL_HANDLE) xrDestroyInstance(S.instance);
    if (S.eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(S.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (S.eglSurface != EGL_NO_SURFACE) eglDestroySurface(S.eglDisplay, S.eglSurface);
        if (S.eglContext != EGL_NO_CONTEXT) eglDestroyContext(S.eglDisplay, S.eglContext);
        eglTerminate(S.eglDisplay);
    }
}

}  // namespace

void android_main(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    AppState S;
    S.app = app;
    const std::string host = resolveHost(app);
    LOGI("KitVR starting, server host %s", host.c_str());

    if (!initLoader(S) || !createInstance(S) || !initEgl(S) || !createSession(S)) {
        LOGE("OpenXR initialization failed");
        cleanup(S);
        ANativeActivity_finish(app->activity);
        return;
    }
    S.net.startControl(host);

    while (!app->destroyRequested && !S.exitRequested) {
        int events = 0;
        android_poll_source* source = nullptr;
        int timeoutMs = S.sessionBegun ? 0 : 100;
        while (ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(app, source);
            timeoutMs = 0;
        }

        pollXrEvents(S);
        maintainVideo(S);
        if (S.sessionBegun) renderFrame(S);
    }

    cleanup(S);
    if (!app->destroyRequested) ANativeActivity_finish(app->activity);
}
