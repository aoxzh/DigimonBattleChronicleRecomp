#pragma once
#include <stdint.h>
#include <jni.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ANativeWindow ANativeWindow;

ANativeWindow *ANativeWindow_fromSurface(JNIEnv *, jobject);
void ANativeWindow_acquire(ANativeWindow *);
void ANativeWindow_release(ANativeWindow *);
int32_t ANativeWindow_getWidth(ANativeWindow *);
int32_t ANativeWindow_getHeight(ANativeWindow *);
#ifdef __cplusplus
}
#endif
