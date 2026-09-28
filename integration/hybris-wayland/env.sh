#!/bin/sh
set -eu
stage=$(CDPATH= cd -- "$(dirname "$0")" && pwd)/lib
runtime=/usr/lib/santos-hybris
platform=${SANTOS_EGL_PLATFORM:-hwcomposer}
exec /root/santos/env-hwc.sh env \
    LD_LIBRARY_PATH="$stage:$runtime:/usr/lib:/lib" \
    LD_PRELOAD="$stage/libegl-platform-compat.so:$runtime/libEGL.so.1:$runtime/libGLESv2.so.2:$stage/libwayland-egl.so.1" \
    HYBRIS_EGLPLATFORM_DIR="$stage/platforms" \
    HYBRIS_EGLPLATFORM="$platform" EGL_PLATFORM="$platform" "$@"
