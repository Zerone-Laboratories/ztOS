/* One-shot GL/EGL allocation failure injection for phone regression checks. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
static int fired, pending;
static int mode(const char *name) {
    const char *value = getenv("ZT_FREEZE_INJECT");
    return !fired && value && !strcmp(value, name);
}
void glClear(GLbitfield mask) {
    static void (*real)(GLbitfield);
    if (!real) real = dlsym(RTLD_NEXT, "glClear");
    real(mask);
    if (mode("clear")) {
        fired = pending = 1;
        fprintf(stderr, "freeze-test: injected GL_OUT_OF_MEMORY after clear\n");
    }
}
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                  GLint border, GLenum format, GLenum type, const void *pixels) {
    static void (*real)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    if (!real) real = dlsym(RTLD_NEXT, "glTexImage2D");
    if (w > 1 && h > 1 && mode("texture")) {
        fired = pending = 1;
        fprintf(stderr, "freeze-test: injected GL_OUT_OF_MEMORY at texture allocation\n");
        return;
    }
    real(target, level, internal, w, h, border, format, type, pixels);
}
void glFinish(void) {
    static void (*real)(void);
    if (!real) real = dlsym(RTLD_NEXT, "glFinish");
    real();
    if (mode("finish")) {
        fired = pending = 1;
        fprintf(stderr, "freeze-test: injected GL_OUT_OF_MEMORY after finish\n");
    }
}
GLenum glGetError(void) {
    static GLenum (*real)(void);
    if (!real) real = dlsym(RTLD_NEXT, "glGetError");
    if (pending == 1) { pending = 0; return GL_OUT_OF_MEMORY; }
    return real();
}
EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
    static EGLBoolean (*real)(EGLDisplay, EGLSurface);
    if (!real) real = dlsym(RTLD_NEXT, "eglSwapBuffers");
    if (mode("swap")) {
        fired = 1;
        pending = 2;
        fprintf(stderr, "freeze-test: injected EGL_BAD_ALLOC at swap\n");
        return EGL_FALSE;
    }
    return real(display, surface);
}
EGLint eglGetError(void) {
    static EGLint (*real)(void);
    if (!real) real = dlsym(RTLD_NEXT, "eglGetError");
    if (pending == 2) { pending = 0; return EGL_BAD_ALLOC; }
    return real();
}
