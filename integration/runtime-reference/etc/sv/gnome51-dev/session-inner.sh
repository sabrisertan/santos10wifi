#!/bin/sh
# Product single-user GNOME Shell 51 session (L128 v3).
# run (root, supervised): setup + hard gates, then supervises ONE session
# process group. session-inner.sh (this file): drops to the product user.
#
# Stop/restart semantics: the session owns a private process group
# (setsid). runit's TERM is trapped in `run` and forwarded to that group,
# so one stop/start/restart always converges to exactly one generation.
# runuser itself ignores TERM, which is why the bare `exec runuser`
# model (v1/v2) could not stop. No PID-list bounce: group kill is the
# mechanism. D-Bus-activated session children die with the group;
# setsid-detached test processes are test hygiene, not session state.

set -u

# Private process group for this generation; outer `run` kills THIS group.
# $$ == pgid after setsid (verified pattern, not an assumption about $!).
echo $$ > "$SESSION_PGID_FILE"

# Privilege boundary: everything below runs as the product user.
# runuser applies uid/gid + supplementary groups (video/input/audio/network/plugdev via initgroups).
exec runuser -u "$PRODUCT_USER" -- dbus-run-session -- sh -c '
    set -u
    # Standard-session allowlist: publish only intended client/session variables to D-Bus activation environment
    dbus-update-activation-environment         WAYLAND_DISPLAY         XDG_RUNTIME_DIR         DBUS_SESSION_BUS_ADDRESS         XDG_SESSION_TYPE         PATH         XDG_DATA_DIRS         XDG_CURRENT_DESKTOP         XDG_SESSION_DESKTOP         PULSE_SERVER         QT_QPA_PLATFORM 2>/dev/null || true
    {
        echo "session_bus_start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS-<unset>}"
        echo "WAYLAND_DISPLAY=${WAYLAND_DISPLAY-<unset>}"
        echo "XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR-<unset>}"
        echo "XDG_SESSION_TYPE=${XDG_SESSION_TYPE-<unset>}"
        echo "QT_QPA_PLATFORM=${QT_QPA_PLATFORM-<unset>}"
        echo "PULSE_SERVER=${PULSE_SERVER-<unset>}"
        id
    } >> "$STATE_DIR/session-bus.txt"

    # Per-user PipeWire audio stack (starts wireplumber & pipewire-pulse via pipewire.conf.d/10-exec.conf)
    if [ -x /usr/bin/pipewire ]; then
        pipewire &
        for _ in $(seq 1 25); do
            [ -e "$XDG_RUNTIME_DIR/pulse/native" ] && break
            sleep 0.2
        done
    fi

    # GNOME media-keys daemon for physical volume/media keys and OSD.
    # Starts as soon as GNOME Shell registers on D-Bus.
    if [ -x /usr/libexec/gsd-media-keys ]; then
        (
            for _ in $(seq 1 50); do
                gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.GetNameOwner org.gnome.Shell >/dev/null 2>&1 && break
                sleep 0.2
            done
            sleep 0.5
            exec /usr/libexec/gsd-media-keys
        ) &
    fi

    # Shell exec boundary: vendor/hybris/private graphics variables ONLY for Shell process
    INSTALL_ROOT=/root/santos/gnome51
    SHELL_BIN=$INSTALL_ROOT/usr/bin/gnome-shell
    SHELL_LIB_DIR=$INSTALL_ROOT/usr/lib/gnome-shell
    DEPS_DIR=$INSTALL_ROOT/usr/lib/gnome-shell-deps
    GJS_PRIVATE_DIR=$INSTALL_ROOT/usr/lib/gjs/girepository-1.0
    MUTTER_LIB_DIR=$INSTALL_ROOT/usr/lib/mutter-51
    R=/usr/lib/santos-hybris
    TLSFIX=/root/libsantos-tlsfix.so

    export LIBEGL=/system/vendor/lib/egl/libEGL_POWERVR_SGX544_115.so
    export LIBGLESV2=/system/vendor/lib/egl/libGLESv2_POWERVR_SGX544_115.so
    export LD_LIBRARY_PATH="$SHELL_LIB_DIR:$DEPS_DIR:$INSTALL_ROOT/usr/lib:$MUTTER_LIB_DIR:$R:/usr/lib:/lib"
    export HYBRIS_ANDROID_SDK_VERSION=25
    export HYBRIS_LINKER_DIR=$R/libhybris/linker
    export HYBRIS_EGLPLATFORM=hwcomposer
    export EGL_PLATFORM=hwcomposer
    export HYBRIS_EGLPLATFORM_DIR=$R/libhybris
    export ANDROID_ROOT=/system
    export ANDROID_DATA=/data
    export LD_SHIM_LIBS="/system/vendor/lib/libmultidisplay.so|/system/lib/libshim_mds.so:/system/vendor/lib/libsepdrm.so|/system/lib/libshim_drm.so"
    export LD_PRELOAD=$TLSFIX:$R/libEGL.so.1
    export COGL_DEBUG=winsys
    export ICU_DATA="$DEPS_DIR/icu/77.1"
    export GI_TYPELIB_PATH="$SHELL_LIB_DIR:$MUTTER_LIB_DIR:$GJS_PRIVATE_DIR:$DEPS_DIR/girepository-1.0:/usr/lib/girepository-1.0"
    export XDG_DATA_DIRS="$INSTALL_ROOT/usr/share:/usr/lib/santos-gtk-client-runtime/share:/usr/local/share:/usr/share"
    export GNOME_SHELL_DATADIR="$INSTALL_ROOT/usr/share/gnome-shell"
    export GSETTINGS_SCHEMA_DIR="$INSTALL_ROOT/usr/share/glib-2.0/schemas"
    export GNOME_SHELL_SESSION_MODE=user

    exec "$SHELL_BIN" --wayland --santos --wayland-display wayland-0 --unsafe-mode
'
