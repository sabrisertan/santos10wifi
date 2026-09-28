// SPDX-License-Identifier: MIT
// Scoped Qt Wayland loader compatibility, not an EGL 1.5 implementation.
// Do not advertise EGL 1.5 or any new extension: retain the vendor's version.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>

EGLDisplay eglGetPlatformDisplay(EGLenum platform, void *native_display,
                                 const EGLAttrib *attribs)
{
    if (platform != EGL_PLATFORM_WAYLAND_KHR || (attribs && attribs[0] != EGL_NONE)) {
        fprintf(stderr, "SANTOS_EGL_COMPAT unsupported display platform/attributes\n");
        return EGL_NO_DISPLAY;
    }
    fprintf(stderr, "SANTOS_EGL_COMPAT Wayland display via EGL 1.4\n");
    return eglGetDisplay((EGLNativeDisplayType)native_display);
}

EGLSurface eglCreatePlatformWindowSurface(EGLDisplay display, EGLConfig config,
                                          void *native_window, const EGLAttrib *attribs)
{
    EGLint converted[65];
    if (attribs) {
        int i;
        for (i = 0; i < 64 && attribs[i] != EGL_NONE; i += 2) {
            if (attribs[i] < INT_MIN || attribs[i] > INT_MAX ||
                attribs[i+1] < INT_MIN || attribs[i+1] > INT_MAX) {
                fprintf(stderr, "SANTOS_EGL_COMPAT non-EGLint window attribute\n");
                return EGL_NO_SURFACE;
            }
            converted[i] = (EGLint)attribs[i];
            converted[i+1] = (EGLint)attribs[i+1];
        }
        if (i == 64) {
            fprintf(stderr, "SANTOS_EGL_COMPAT too many window attributes\n");
            return EGL_NO_SURFACE;
        }
        converted[i] = EGL_NONE;
    }
    fprintf(stderr, "SANTOS_EGL_COMPAT Wayland window via EGL 1.4\n");
    return eglCreateWindowSurface(display, config, (EGLNativeWindowType)native_window,
                                  attribs ? converted : NULL);
}
