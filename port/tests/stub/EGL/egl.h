#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int32_t EGLint;
typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLConfig;
#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_DISPLAY ((void *)0)
#define EGL_NO_SURFACE ((void *)0)
#define EGL_NO_CONTEXT ((void *)0)
#define EGL_OPENGL_ES2_BIT 4
#define EGL_WINDOW_BIT 1
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_SURFACE_TYPE 0x3033
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
EGLDisplay eglGetDisplay(void *);
int eglInitialize(EGLDisplay, EGLint *, EGLint *);
int eglChooseConfig(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
EGLSurface eglCreateWindowSurface(EGLDisplay, EGLConfig, void *, const EGLint *);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
int eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
int eglSwapBuffers(EGLDisplay, EGLSurface);
int eglDestroySurface(EGLDisplay, EGLSurface);
int eglDestroyContext(EGLDisplay, EGLContext);
int eglTerminate(EGLDisplay);
#ifdef __cplusplus
}
#endif
