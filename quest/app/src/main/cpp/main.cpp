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

#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
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

struct HandActions {
    XrAction pose = XR_NULL_HANDLE;
    XrAction trigger = XR_NULL_HANDLE;
    XrAction triggerClick = XR_NULL_HANDLE;
    XrAction squeeze = XR_NULL_HANDLE;
    XrAction thumbstick = XR_NULL_HANDLE;
    XrAction thumbstickClick = XR_NULL_HANDLE;
    XrAction primary = XR_NULL_HANDLE;
    XrAction secondary = XR_NULL_HANDLE;
    XrAction menu = XR_NULL_HANDLE;
};

struct AppState {
    android_app* app = nullptr;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrActionSet actionSet = XR_NULL_HANDLE;
    std::array<XrPath, 2> handPaths{XR_NULL_PATH, XR_NULL_PATH};
    std::array<HandActions, 2> handActions{};
    std::array<XrSpace, 2> handSpaces{XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool hasHandActions = false;
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

    bool micRequested = false;
    uint64_t lastPermCheckUs = 0;

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

// ---------------------------------------------------------------- microphone permission

constexpr const char* kMicPermission = "android.permission.RECORD_AUDIO";

bool hasMicPermission(android_app* app) {
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
    jobject activity = app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID check = env->GetMethodID(cls, "checkSelfPermission", "(Ljava/lang/String;)I");
    if (!check) return false;
    jstring perm = env->NewStringUTF(kMicPermission);
    jint result = env->CallIntMethod(activity, check, perm);
    env->DeleteLocalRef(perm);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return result == 0;  // PackageManager.PERMISSION_GRANTED
}

void requestMicPermission(android_app* app) {
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    jobject activity = app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID request = env->GetMethodID(cls, "requestPermissions", "([Ljava/lang/String;I)V");
    if (!request) return;
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray perms = env->NewObjectArray(1, stringClass, env->NewStringUTF(kMicPermission));
    env->CallVoidMethod(activity, request, perms, 1);
    if (env->ExceptionCheck()) env->ExceptionClear();
    LOGI("requested microphone permission");
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

bool createHandAction(AppState& S, size_t handIndex, const char* actionName,
                      const char* localizedName,
                      XrActionType type, XrAction& action) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, actionName, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, localizedName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = type;
    info.countSubactionPaths = 1;
    info.subactionPaths = &S.handPaths[handIndex];
    CHECK(xrCreateAction(S.actionSet, &info, &action));
    return true;
}

bool addBinding(AppState& S, XrAction action, const char* path,
                std::vector<XrActionSuggestedBinding>& bindings) {
    XrPath xrPath = XR_NULL_PATH;
    CHECK(xrStringToPath(S.instance, path, &xrPath));
    bindings.push_back({action, xrPath});
    return true;
}

bool createActions(AppState& S) {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(setInfo.actionSetName, "kitvr_controls", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "KitVR Controls",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    CHECK(xrCreateActionSet(S.instance, &setInfo, &S.actionSet));
    CHECK(xrStringToPath(S.instance, "/user/hand/left", &S.handPaths[0]));
    CHECK(xrStringToPath(S.instance, "/user/hand/right", &S.handPaths[1]));

    std::vector<XrActionSuggestedBinding> bindings;
    for (size_t i = 0; i < S.handActions.size(); ++i) {
        const char* hand = i == 0 ? "left" : "right";
        const char* base = i == 0 ? "/user/hand/left" : "/user/hand/right";
        auto& actions = S.handActions[i];
        std::string actionName = std::string(hand) + "_grip_pose";
        CHECK(createHandAction(S, i, actionName.c_str(), "Grip pose",
                              XR_ACTION_TYPE_POSE_INPUT, actions.pose));
        actionName = std::string(hand) + "_trigger_value";
        CHECK(createHandAction(S, i, actionName.c_str(), "Trigger",
                              XR_ACTION_TYPE_FLOAT_INPUT, actions.trigger));
        actionName = std::string(hand) + "_trigger_click";
        CHECK(createHandAction(S, i, actionName.c_str(), "Trigger click",
                              XR_ACTION_TYPE_BOOLEAN_INPUT, actions.triggerClick));
        actionName = std::string(hand) + "_squeeze";
        CHECK(createHandAction(S, i, actionName.c_str(), "Grip",
                              XR_ACTION_TYPE_FLOAT_INPUT, actions.squeeze));
        actionName = std::string(hand) + "_thumbstick";
        CHECK(createHandAction(S, i, actionName.c_str(), "Thumbstick",
                              XR_ACTION_TYPE_VECTOR2F_INPUT, actions.thumbstick));
        actionName = std::string(hand) + "_thumbstick_click";
        CHECK(createHandAction(S, i, actionName.c_str(), "Thumbstick click",
                              XR_ACTION_TYPE_BOOLEAN_INPUT, actions.thumbstickClick));
        actionName = std::string(hand) + "_primary";
        CHECK(createHandAction(S, i, actionName.c_str(), "Primary button",
                              XR_ACTION_TYPE_BOOLEAN_INPUT, actions.primary));
        actionName = std::string(hand) + "_secondary";
        CHECK(createHandAction(S, i, actionName.c_str(), "Secondary button",
                              XR_ACTION_TYPE_BOOLEAN_INPUT, actions.secondary));
        actionName = std::string(hand) + "_menu";
        CHECK(createHandAction(S, i, actionName.c_str(), "Menu button",
                              XR_ACTION_TYPE_BOOLEAN_INPUT, actions.menu));

        CHECK(addBinding(S, actions.pose, (std::string(base) + "/input/grip/pose").c_str(), bindings));
        CHECK(addBinding(S, actions.trigger, (std::string(base) + "/input/trigger/value").c_str(), bindings));
        CHECK(addBinding(S, actions.triggerClick, (std::string(base) + "/input/trigger/click").c_str(), bindings));
        CHECK(addBinding(S, actions.squeeze, (std::string(base) + "/input/squeeze/value").c_str(), bindings));
        CHECK(addBinding(S, actions.thumbstick, (std::string(base) + "/input/thumbstick").c_str(), bindings));
        CHECK(addBinding(S, actions.thumbstickClick, (std::string(base) + "/input/thumbstick/click").c_str(), bindings));
        const char* primary = i == 0 ? "/input/x/click" : "/input/a/click";
        const char* secondary = i == 0 ? "/input/y/click" : "/input/b/click";
        CHECK(addBinding(S, actions.primary, (std::string(base) + primary).c_str(), bindings));
        CHECK(addBinding(S, actions.secondary, (std::string(base) + secondary).c_str(), bindings));
        if (i == 0) {
            CHECK(addBinding(S, actions.menu, (std::string(base) + "/input/menu/click").c_str(), bindings));
        }
    }

    XrPath profile = XR_NULL_PATH;
    CHECK(xrStringToPath(S.instance, "/interaction_profiles/oculus/touch_controller", &profile));
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = profile;
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    CHECK(xrSuggestInteractionProfileBindings(S.instance, &suggested));

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &S.actionSet;
    CHECK(xrAttachSessionActionSets(S.session, &attach));

    for (size_t i = 0; i < S.handActions.size(); ++i) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = S.handActions[i].pose;
        spaceInfo.subactionPath = S.handPaths[i];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        CHECK(xrCreateActionSpace(S.session, &spaceInfo, &S.handSpaces[i]));
    }
    S.hasHandActions = true;
    return true;
}

bool actionBoolean(AppState& S, XrAction action, XrPath hand) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    info.subactionPath = hand;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(S.session, &info, &state)) &&
           state.isActive && state.currentState;
}

kitvr::TrackingHand readHand(AppState& S, size_t index, XrTime displayTime) {
    kitvr::TrackingHand hand{};
    const XrPath path = S.handPaths[index];
    const auto& actions = S.handActions[index];

    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = actions.pose;
    getInfo.subactionPath = path;
    XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
    if (XR_SUCCEEDED(xrGetActionStatePose(S.session, &getInfo, &poseState)) && poseState.isActive) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags required = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                              XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(S.handSpaces[index], S.localSpace, displayTime, &location)) &&
            (location.locationFlags & required) == required) {
            hand.active = true;
            hand.pose[0] = location.pose.position.x;
            hand.pose[1] = location.pose.position.y;
            hand.pose[2] = location.pose.position.z;
            hand.pose[3] = location.pose.orientation.x;
            hand.pose[4] = location.pose.orientation.y;
            hand.pose[5] = location.pose.orientation.z;
            hand.pose[6] = location.pose.orientation.w;
        }
    }

    XrActionStateFloat floatState{XR_TYPE_ACTION_STATE_FLOAT};
    getInfo.action = actions.trigger;
    if (XR_SUCCEEDED(xrGetActionStateFloat(S.session, &getInfo, &floatState)) && floatState.isActive) {
        hand.trigger = std::clamp(floatState.currentState, 0.0f, 1.0f);
    }
    getInfo.action = actions.squeeze;
    if (XR_SUCCEEDED(xrGetActionStateFloat(S.session, &getInfo, &floatState)) && floatState.isActive) {
        hand.grip = std::clamp(floatState.currentState, 0.0f, 1.0f);
    }
    XrActionStateVector2f vectorState{XR_TYPE_ACTION_STATE_VECTOR2F};
    getInfo.action = actions.thumbstick;
    if (XR_SUCCEEDED(xrGetActionStateVector2f(S.session, &getInfo, &vectorState)) && vectorState.isActive) {
        hand.stickX = vectorState.currentState.x;
        hand.stickY = vectorState.currentState.y;
    }
    if (actionBoolean(S, actions.triggerClick, path)) hand.buttons |= 1u << 0;
    if (hand.grip > 0.5f) hand.buttons |= 1u << 1;
    if (actionBoolean(S, actions.primary, path)) hand.buttons |= 1u << 2;
    if (actionBoolean(S, actions.secondary, path)) hand.buttons |= 1u << 3;
    if (actionBoolean(S, actions.menu, path)) hand.buttons |= 1u << 4;
    if (actionBoolean(S, actions.thumbstickClick, path)) hand.buttons |= 1u << 5;
    return hand;
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

void maintainAudio(AppState& S) {
    S.net.tick();
    if (!S.net.wantsMic()) return;
    uint64_t now = kitvr::nowUs();
    if (now - S.lastPermCheckUs < 1000000) return;
    S.lastPermCheckUs = now;
    if (hasMicPermission(S.app)) {
        S.net.setMicPermission(true);
    } else if (!S.micRequested) {
        requestMicPermission(S.app);
        S.micRequested = true;
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
        kitvr::TrackingHand left;
        kitvr::TrackingHand right;
        if (S.hasHandActions) {
            XrActiveActionSet activeSet{};
            activeSet.actionSet = S.actionSet;
            XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
            syncInfo.countActiveActionSets = 1;
            syncInfo.activeActionSets = &activeSet;
            if (XR_SUCCEEDED(xrSyncActions(S.session, &syncInfo))) {
                left = readHand(S, 0, fs.predictedDisplayTime);
                right = readHand(S, 1, fs.predictedDisplayTime);
            }
        }
        S.net.sendTracking(kitvr::nowUs(), head, left, right);
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
    for (XrSpace& space : S.handSpaces) {
        if (space != XR_NULL_HANDLE) xrDestroySpace(space);
        space = XR_NULL_HANDLE;
    }
    if (S.viewSpace != XR_NULL_HANDLE) xrDestroySpace(S.viewSpace);
    if (S.localSpace != XR_NULL_HANDLE) xrDestroySpace(S.localSpace);
    if (S.session != XR_NULL_HANDLE) xrDestroySession(S.session);
    if (S.actionSet != XR_NULL_HANDLE) xrDestroyActionSet(S.actionSet);
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
    if (!createActions(S)) LOGW("controller actions unavailable; continuing with head tracking");
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
        maintainAudio(S);
        if (S.sessionBegun) renderFrame(S);
    }

    cleanup(S);
    if (!app->destroyRequested) ANativeActivity_finish(app->activity);
}
