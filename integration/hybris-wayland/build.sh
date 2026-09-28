#!/bin/sh
# Build only the Wayland-facing modules against the installed BSP libraries.
# Input: matching eaa6884 libhybris source in src/{include,egl,common}.
set -eu
cd "$(dirname "$0")"
mkdir -p obj lib/platforms
cc -fPIC -shared -O2 -Wall -Wextra egl-platform-compat.c \
    -L/usr/lib/santos-hybris -lEGL -o lib/libegl-platform-compat.so
src=$PWD/src
common=$src/egl/platforms/common
wayland=$src/egl/platforms/wayland
cp "$PWD/wayland_window.cpp" "$wayland/wayland_window.cpp"
cp "$PWD/wayland_window_pool.h" "$wayland/wayland_window_pool.h"
printf '#define WANT_WAYLAND 1\n' > obj/config.h
wayland-scanner public-code "$common/wayland-android.xml" obj/wayland-android-protocol.c
wayland-scanner client-header "$common/wayland-android.xml" obj/wayland-android-client-protocol.h
wayland-scanner server-header "$common/wayland-android.xml" obj/wayland-android-server-protocol.h
set -- -fPIC -O2 -g -D_LARGEFILE64_SOURCE -I"$PWD/obj" -I"$src/include" \
    -I"$src/egl" -I"$src/common" -I"$common" -I/usr/include/android-7.1 \
    -I/usr/include/santos-hybris
for name in native_handle wayland-egl; do
    cc "$@" -c "$common/$name.c" -o "obj/$name.o"
done
cc "$@" -c obj/wayland-android-protocol.c -o obj/wayland-android-protocol.o
for name in nativewindowbase eglplatformcommon windowbuffer server_wlegl server_wlegl_handle server_wlegl_buffer; do
    g++ -std=gnu++11 "$@" -c "$common/$name.cpp" -o "obj/$name.o"
done
g++ -shared -Wl,-z,defs -Wl,-soname,libhybris-eglplatformcommon.so.1 \
    -o lib/libhybris-eglplatformcommon.so.1 obj/native_handle.o obj/nativewindowbase.o \
    obj/eglplatformcommon.o obj/windowbuffer.o obj/server_wlegl.o \
    obj/server_wlegl_handle.o obj/server_wlegl_buffer.o obj/wayland-android-protocol.o \
    -L/usr/lib/santos-hybris -lhybris-common -lgralloc -lsync -lwayland-client -lwayland-server -pthread
cc -shared -Wl,-z,defs -Wl,-soname,libwayland-egl.so.1 \
    -o lib/libwayland-egl.so.1 obj/wayland-egl.o -lwayland-client
for name in eglplatform_wayland wayland_window; do
    g++ -std=gnu++11 "$@" -c "$wayland/$name.cpp" -o "obj/$name.o"
done
g++ -shared -Wl,-z,defs -o lib/platforms/eglplatform_wayland.so \
    obj/eglplatform_wayland.o obj/wayland_window.o -L"$PWD/lib" \
    -l:libhybris-eglplatformcommon.so.1 -L/usr/lib/santos-hybris \
    -lgralloc -lsync -lhybris-common -lwayland-client -pthread
ln -sfn /usr/lib/santos-hybris/libhybris/eglplatform_hwcomposer.so lib/platforms/eglplatform_hwcomposer.so
echo SANTOS_WAYLAND_MODULES_BUILT
