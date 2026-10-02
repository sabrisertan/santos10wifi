/* SPDX-License-Identifier: GPL-3.0-or-later
 * Static, single-threaded exec boundary for all Pipeline yt-dlp entry points.
 * No dynamic loader/preloads run before cleanup. Preserve argv and ordinary
 * stdin/stdout/stderr; do not log URLs, cookies, tokens or complete arguments.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const graphics_keys[] = {
    "LD_PRELOAD", "LD_LIBRARY_PATH", "GDK_BACKEND", "GSK_RENDERER", "GDK_DISABLE",
    "LIBEGL", "LIBGLESV2", "HYBRIS_ANDROID_SDK_VERSION", "HYBRIS_LINKER_DIR",
    "HYBRIS_EGLPLATFORM", "HYBRIS_EGLPLATFORM_DIR", "EGL_PLATFORM", "SANTOS_EGL_PLATFORM",
    "ANDROID_ROOT", "ANDROID_DATA", "LD_SHIM_LIBS", "GI_TYPELIB_PATH", "GNOME_SHELL_DATADIR",
    "GSETTINGS_SCHEMA_DIR", "SANTOS_GLES2_TRACE", "SANTOS_VO_LIBMPV_DECOUPLE", NULL
};

int main(int argc, char **argv)
{
    const char *tls = "/root/libsantos-tlsfix.so";
    const char *value = getenv("LD_PRELOAD");
    char *preload = value ? strdup(value) : NULL;
    int keep_tls = 0;
    if (value && !preload)
        return 125;
    if (preload) {
        char *save = NULL;
        for (char *part = strtok_r(preload, ": \t\r\n", &save); part;
             part = strtok_r(NULL, ": \t\r\n", &save)) {
            if (strcmp(part, tls) == 0)
                keep_tls = 1;
        }
    }
    free(preload);
    for (size_t i = 0; graphics_keys[i]; i++) {
        if (unsetenv(graphics_keys[i]) != 0)
            return 125;
    }
    if (keep_tls && setenv("LD_PRELOAD", tls, 1) != 0)
        return 125;
    (void)argc;
    argv[0] = "/usr/bin/yt-dlp";
    execv(argv[0], argv);
    fprintf(stderr, "Pipeline network helper: yt-dlp exec failed: %s\n", strerror(errno));
    return 127;
}
