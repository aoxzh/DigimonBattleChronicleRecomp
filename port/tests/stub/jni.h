/* 本目录仅供本机（macOS）clang -fsyntax-only 语法检查 android/ 原生源用，
 * 不参与真实构建（真实构建用 NDK 自带头）。 */
#pragma once
#include <stdint.h>

struct _JNIEnv;
typedef struct _JNIEnv JNIEnv;
struct _JavaVM;
typedef struct _JavaVM JavaVM;

struct _jobject;
typedef struct _jobject *jobject;
struct _jstring;
typedef struct _jstring *jstring;
struct _jclass;
typedef struct _jclass *jclass;

typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef uint8_t jboolean;

#ifndef JNIEXPORT
#define JNIEXPORT __attribute__((visibility("default")))
#endif
#ifndef JNICALL
#define JNICALL
#endif

/* 模拟真实 jni.h C++ 侧的成员调用语法（env->Method(...)）：
 * 函数指针签名省去 JNIEnv* 首参，使 env->GetStringUTFChars(s, nullptr)
 * 在桩与真实头下语法一致（真实头里它们是 JNIEnv 的成员函数）。 */
struct _JNIEnv {
    jstring (*NewStringUTF)(const char *);
    const char *(*GetStringUTFChars)(jstring, jboolean *);
    void (*ReleaseStringUTFChars)(jstring, const char *);
};
