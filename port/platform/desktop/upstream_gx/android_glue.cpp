#include "android_glue.h"
#include <SDL.h>
#include <SDL_system.h>
#include <jni.h>
#include <cstdio>
#include <mutex>
#include <string>

namespace {
std::mutex picked_lock;
std::string picked_path, picked_name;
bool picked;
}

// GameActivity.onActivityResult (UI thread) -> read by the launcher on the SDL thread.
extern "C" JNIEXPORT void JNICALL
Java_jp_digimonbc_port_GameActivity_nativeDiscPicked(JNIEnv *env, jclass, jstring path, jstring name) {
    const char *p = env->GetStringUTFChars(path, nullptr);
    const char *n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
    {
        std::lock_guard<std::mutex> g(picked_lock);
        picked_path = p; picked_name = n ? n : p; picked = true;
    }
    env->ReleaseStringUTFChars(path, p);
    if (n) env->ReleaseStringUTFChars(name, n);
}

extern "C" void android_pick_disc(void) {
    JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    jmethodID pick = env->GetMethodID(cls, "pickDisc", "()V");
    if (pick) env->CallVoidMethod(activity, pick);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

extern "C" int android_take_picked_disc(char *path, size_t path_size, char *name, size_t name_size) {
    std::lock_guard<std::mutex> g(picked_lock);
    if (!picked) return 0;
    picked = false;
    snprintf(path, path_size, "%s", picked_path.c_str());
    snprintf(name, name_size, "%s", picked_name.c_str());
    return 1;
}
