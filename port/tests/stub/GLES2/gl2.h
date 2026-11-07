#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef int GLsizei;
typedef float GLfloat;
typedef unsigned char GLboolean;

#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_TRIANGLE_STRIP 5
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT_565 0x8363
#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_COLOR_BUFFER_BIT 0x4000

GLuint glCreateShader(GLenum);
void glShaderSource(GLuint, int, const char **, const int *);
void glCompileShader(GLuint);
GLuint glCreateProgram(void);
void glAttachShader(GLuint, GLuint);
void glLinkProgram(GLuint);
void glUseProgram(GLuint);
void glGenTextures(int, GLuint *);
void glBindTexture(GLenum, GLuint);
void glTexParameteri(GLenum, GLenum, GLenum);
void glTexImage2D(GLenum, GLint, GLint, int, int, int, GLenum, GLenum, const void *);
void glTexSubImage2D(GLenum, GLint, int, int, int, int, GLenum, GLenum, const void *);
void glDrawArrays(GLenum, int, int);
GLint glGetAttribLocation(GLuint, const char *);
void glEnableVertexAttribArray(GLuint);
void glVertexAttribPointer(GLuint, int, GLenum, GLboolean, int, const void *);
GLint glGetUniformLocation(GLuint, const char *);
void glUniform1i(GLint, int);
void glViewport(int, int, int, int);
void glClearColor(float, float, float, float);
void glClear(unsigned int);
#ifdef __cplusplus
}
#endif
