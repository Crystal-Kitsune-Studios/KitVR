#pragma once

#include <jni.h>

#include <climits>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace kitvr {

class SecureChannel {
public:
    SecureChannel() = default;
    ~SecureChannel() { reset(); }
    SecureChannel(const SecureChannel&) = delete;
    SecureChannel& operator=(const SecureChannel&) = delete;

    static bool clientAuthenticator(JavaVM* vm, jobject activity, const uint8_t key[32],
                                    const uint8_t clientNonce[32], uint8_t proof[32]) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (!attach(vm, env, attached)) return false;
        jclass cls = loadClass(env, activity);
        jmethodID method = cls ? env->GetStaticMethodID(cls, "clientAuthenticator", "([B[B)[B") : nullptr;
        jbyteArray jKey = byteArray(env, key, 32);
        jbyteArray jNonce = byteArray(env, clientNonce, 32);
        jbyteArray result = method && jKey && jNonce
                ? static_cast<jbyteArray>(env->CallStaticObjectMethod(cls, method, jKey, jNonce))
                : nullptr;
        const bool ok = copyBytes(env, result, proof, 32);
        clearException(env);
        if (result) env->DeleteLocalRef(result);
        if (jKey) env->DeleteLocalRef(jKey);
        if (jNonce) env->DeleteLocalRef(jNonce);
        if (cls) env->DeleteLocalRef(cls);
        detach(vm, attached);
        return ok;
    }

    static bool verifyServerAuthenticator(JavaVM* vm, jobject activity, const uint8_t key[32],
                                         const uint8_t clientNonce[32],
                                         const uint8_t serverNonce[32],
                                         const uint8_t proof[32]) {
        JNIEnv* env = nullptr;
        bool attached = false;
        if (!attach(vm, env, attached)) return false;
        jclass cls = loadClass(env, activity);
        jmethodID method = cls ? env->GetStaticMethodID(
                cls, "verifyServerAuthenticator", "([B[B[B[B)Z") : nullptr;
        jbyteArray jKey = byteArray(env, key, 32);
        jbyteArray jClientNonce = byteArray(env, clientNonce, 32);
        jbyteArray jServerNonce = byteArray(env, serverNonce, 32);
        jbyteArray jProof = byteArray(env, proof, 32);
        const jboolean result = method && jKey && jClientNonce && jServerNonce && jProof
                ? env->CallStaticBooleanMethod(
                        cls, method, jKey, jClientNonce, jServerNonce, jProof)
                : JNI_FALSE;
        const bool ok = result == JNI_TRUE && !env->ExceptionCheck();
        clearException(env);
        if (jKey) env->DeleteLocalRef(jKey);
        if (jClientNonce) env->DeleteLocalRef(jClientNonce);
        if (jServerNonce) env->DeleteLocalRef(jServerNonce);
        if (jProof) env->DeleteLocalRef(jProof);
        if (cls) env->DeleteLocalRef(cls);
        detach(vm, attached);
        return ok;
    }

    bool initialize(JavaVM* vm, jobject activity, const uint8_t key[32],
                    const uint8_t clientNonce[32], const uint8_t serverNonce[32]) {
        std::lock_guard<std::mutex> lock(mutex_);
        resetUnlocked();
        vm_ = vm;
        JNIEnv* env = nullptr;
        bool attached = false;
        if (!attach(vm_, env, attached)) return false;
        jclass cls = loadClass(env, activity);
        jmethodID ctor = cls ? env->GetMethodID(cls, "<init>", "([B[B[BZ)V") : nullptr;
        jbyteArray jKey = byteArray(env, key, 32);
        jbyteArray jClientNonce = byteArray(env, clientNonce, 32);
        jbyteArray jServerNonce = byteArray(env, serverNonce, 32);
        jobject local = ctor && jKey && jClientNonce && jServerNonce
                ? env->NewObject(cls, ctor, jKey, jClientNonce, jServerNonce, JNI_TRUE)
                : nullptr;
        object_ = local ? env->NewGlobalRef(local) : nullptr;
        class_ = cls ? static_cast<jclass>(env->NewGlobalRef(cls)) : nullptr;
        const bool ok = object_ && class_ && !env->ExceptionCheck();
        clearException(env);
        if (local) env->DeleteLocalRef(local);
        if (jKey) env->DeleteLocalRef(jKey);
        if (jClientNonce) env->DeleteLocalRef(jClientNonce);
        if (jServerNonce) env->DeleteLocalRef(jServerNonce);
        if (cls) env->DeleteLocalRef(cls);
        detach(vm_, attached);
        return ok;
    }

    bool sealControl(uint64_t sequence, const uint8_t* input, size_t size,
                     std::vector<uint8_t>& output) {
        return callBytes("sealControl", "(J[B)[B", sequence, nullptr, 0, input, size, output);
    }

    bool openControl(uint64_t sequence, const uint8_t* input, size_t size,
                     std::vector<uint8_t>& output) {
        return callBytes("openControl", "(J[B)[B", sequence, nullptr, 0, input, size, output);
    }

    bool sealMedia(uint64_t sequence, const uint8_t* aad, size_t aadSize,
                   const uint8_t* input, size_t size, std::vector<uint8_t>& output) {
        return callBytes("sealMedia", "(J[B[B)[B", sequence, aad, aadSize, input, size, output);
    }

    bool openMedia(uint64_t sequence, const uint8_t* aad, size_t aadSize,
                   const uint8_t* input, size_t size, std::vector<uint8_t>& output) {
        return callBytes("openMedia", "(J[B[B)[B", sequence, aad, aadSize, input, size, output);
    }

private:
    static bool attach(JavaVM* vm, JNIEnv*& env, bool& attached) {
        if (!vm) return false;
        const jint state = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (state == JNI_OK) return true;
        if (state != JNI_EDETACHED ||
            vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return false;
        attached = true;
        return true;
    }

    static void detach(JavaVM* vm, bool attached) {
        if (attached) vm->DetachCurrentThread();
    }

    static void clearException(JNIEnv* env) {
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    static jclass loadClass(JNIEnv* env, jobject activity) {
        jclass activityClass = env->GetObjectClass(activity);
        jmethodID getClassLoader = activityClass
                ? env->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;")
                : nullptr;
        jobject loader = getClassLoader
                ? env->CallObjectMethod(activity, getClassLoader)
                : nullptr;
        jclass loaderClass = loader ? env->GetObjectClass(loader) : nullptr;
        jmethodID loadClass = loaderClass
                ? env->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;")
                : nullptr;
        jstring name = env->NewStringUTF("com.crystalkitsune.kitvr.SecureChannel");
        jclass result = loadClass && name
                ? static_cast<jclass>(env->CallObjectMethod(loader, loadClass, name))
                : nullptr;
        if (name) env->DeleteLocalRef(name);
        if (loaderClass) env->DeleteLocalRef(loaderClass);
        if (loader) env->DeleteLocalRef(loader);
        if (activityClass) env->DeleteLocalRef(activityClass);
        clearException(env);
        return result;
    }

    static jbyteArray byteArray(JNIEnv* env, const uint8_t* data, size_t size) {
        if (size > static_cast<size_t>(INT32_MAX)) return nullptr;
        jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
        if (array && size) env->SetByteArrayRegion(array, 0, static_cast<jsize>(size),
                reinterpret_cast<const jbyte*>(data));
        return array;
    }

    static bool copyBytes(JNIEnv* env, jbyteArray array, uint8_t* output, size_t size) {
        if (!array || env->GetArrayLength(array) != static_cast<jsize>(size)) return false;
        env->GetByteArrayRegion(array, 0, static_cast<jsize>(size),
                reinterpret_cast<jbyte*>(output));
        return !env->ExceptionCheck();
    }

    bool callBytes(const char* name, const char* signature, uint64_t sequence,
                   const uint8_t* aad, size_t aadSize, const uint8_t* input, size_t size,
                   std::vector<uint8_t>& output) {
        std::lock_guard<std::mutex> lock(mutex_);
        JNIEnv* env = nullptr;
        bool attached = false;
        if (!object_ || !attach(vm_, env, attached)) return false;
        jmethodID method = class_ ? env->GetMethodID(class_, name, signature) : nullptr;
        jbyteArray jAad = aad ? byteArray(env, aad, aadSize) : nullptr;
        jbyteArray jInput = byteArray(env, input, size);
        jobject result = nullptr;
        if (method && jInput && (aad == nullptr || jAad)) {
            if (aad) {
                result = env->CallObjectMethod(object_, method, static_cast<jlong>(sequence),
                                               jAad, jInput);
            } else {
                result = env->CallObjectMethod(object_, method, static_cast<jlong>(sequence),
                                               jInput);
            }
        }
        bool ok = result && !env->ExceptionCheck();
        if (ok) {
            const jsize length = env->GetArrayLength(static_cast<jbyteArray>(result));
            output.resize(static_cast<size_t>(length));
            if (length) env->GetByteArrayRegion(static_cast<jbyteArray>(result), 0, length,
                    reinterpret_cast<jbyte*>(output.data()));
            ok = !env->ExceptionCheck();
        }
        clearException(env);
        if (result) env->DeleteLocalRef(result);
        if (jAad) env->DeleteLocalRef(jAad);
        if (jInput) env->DeleteLocalRef(jInput);
        detach(vm_, attached);
        return ok;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        resetUnlocked();
    }

    void resetUnlocked() {
        if (!vm_) return;
        JNIEnv* env = nullptr;
        bool attached = false;
        if (!attach(vm_, env, attached)) return;
        if (object_) env->DeleteGlobalRef(object_);
        if (class_) env->DeleteGlobalRef(class_);
        detach(vm_, attached);
        object_ = nullptr;
        class_ = nullptr;
        vm_ = nullptr;
    }

    JavaVM* vm_ = nullptr;
    jobject object_ = nullptr;
    jclass class_ = nullptr;
    std::mutex mutex_;
};

}  // namespace kitvr
