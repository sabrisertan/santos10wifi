#!/usr/bin/python3
"""Santos standalone mpv: scoped graphics/audio defaults, no controller or respawn."""
import os
from pathlib import Path
import pwd
import sys

PLAYER = "/usr/libexec/mpv-santos/mpv"
LIBDIR = "/usr/lib/mpv-santos-cli"
HYBRIS = "/usr/lib/santos-hybris"
SESSION_EXE = "/root/santos/gnome51/usr/bin/gnome-shell"
SESSION_KEYS = {
    "DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY",
    "PULSE_SERVER", "LANG", "LC_ALL", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP",
}
INFO_OPTIONS = {
    "--version", "-V", "--help", "-h", "--list-options", "--list-properties",
    "--vo=help", "--ao=help", "--vd=help", "--gpu-context=help",
}


def info_only(arguments):
    for argument in arguments:
        if argument == "--":
            break
        if argument in INFO_OPTIONS:
            return True
    return False


def find_session(uid, proc_root=Path("/proc")):
    matches = []
    for proc in proc_root.iterdir():
        if not proc.name.isdecimal():
            continue
        try:
            if proc.stat().st_uid != uid or os.readlink(proc / "exe") != SESSION_EXE:
                continue
            values = {}
            for item in (proc / "environ").read_bytes().split(b"\0"):
                key, separator, value = item.partition(b"=")
                if separator and key.decode(errors="replace") in SESSION_KEYS:
                    values[key.decode()] = value.decode()
            matches.append(values)
        except (OSError, UnicodeError):
            continue
    if len(matches) != 1:
        raise RuntimeError("expected one live Santos GNOME session")
    return matches[0]


def player_environment(ambient, uid):
    # Never inherit the GTK renderer's library path or an unrelated EGL preload.
    prefixes = ("LD_", "HYBRIS_", "GDK_", "GSK_", "QT_", "MESA_", "LIBGL_",
                "EGL_", "GL4ES_", "SANTOS_", "PIPELINE_", "PVR_", "PVRSRV_")
    env = {key: value for key, value in ambient.items()
           if not key.startswith(prefixes) and key not in
           {"LIBEGL", "LIBGLESV1", "LIBGLESV2", "WAYLAND_SOCKET"}}
    stage = LIBDIR + "/wayland"
    env.update({
        "PATH": "/usr/local/bin:/usr/bin:/bin",
        "LD_LIBRARY_PATH": f"{stage}:{HYBRIS}:/usr/lib:/lib",
        "LD_PRELOAD": ":".join((
            LIBDIR + "/libsantos-tlsfix.so", stage + "/libegl-platform-compat.so",
            HYBRIS + "/libEGL.so.1", HYBRIS + "/libGLESv2.so.2",
            stage + "/libwayland-egl.so.1",
        )),
        "LIBEGL": "/system/vendor/lib/egl/libEGL_POWERVR_SGX544_115.so",
        "LIBGLESV2": "/system/vendor/lib/egl/libGLESv2_POWERVR_SGX544_115.so",
        "HYBRIS_ANDROID_SDK_VERSION": "25",
        "HYBRIS_LINKER_DIR": HYBRIS + "/libhybris/linker",
        "HYBRIS_EGLPLATFORM": "wayland",
        "HYBRIS_EGLPLATFORM_DIR": stage + "/platforms",
        "EGL_PLATFORM": "wayland", "SANTOS_EGL_PLATFORM": "wayland",
        "SANTOS_WAYLAND_LEGACY_EGL": "1",
        "ANDROID_ROOT": "/system", "ANDROID_DATA": "/data",
        "LD_SHIM_LIBS": "/system/vendor/lib/libmultidisplay.so|/system/lib/libshim_mds.so:"
                        "/system/vendor/lib/libsepdrm.so|/system/lib/libshim_drm.so",
    })
    env.setdefault("XDG_RUNTIME_DIR", f"/run/user/{uid}")
    env.setdefault("WAYLAND_DISPLAY", "wayland-0")
    env.setdefault("PULSE_SERVER", "unix:" + env["XDG_RUNTIME_DIR"] + "/pulse/native")
    return env


def player_arguments(arguments, vdx_available):
    # Device defaults come first: explicit user arguments continue to win.
    return [PLAYER, "--vo=gpu", "--gpu-context=wayland", "--opengl-es=yes",
            "--profile=fast", "--demuxer-lavf-probe-info=yes",
            "--vd=" + ("santos_vdx" if vdx_available else "h264"),
            "--ao=pulse", "--audio-device=auto", "--autofit=960x540",
            "--wayland-app-id=mpv"] + list(arguments)


def main(arguments=None):
    arguments = list(sys.argv[1:] if arguments is None else arguments)
    metadata = info_only(arguments)
    uid = os.geteuid()
    account = None
    if uid == 0 and not metadata:
        account = pwd.getpwnam("santos")
        ambient = find_session(account.pw_uid)
        ambient.update(HOME=account.pw_dir, USER=account.pw_name, LOGNAME=account.pw_name)
        uid = account.pw_uid
    else:
        ambient = dict(os.environ)
    env = player_environment(ambient, uid)
    if not metadata:
        display = Path(env["WAYLAND_DISPLAY"])
        if not display.is_absolute():
            display = Path(env["XDG_RUNTIME_DIR"]) / display
        if not display.is_socket():
            raise RuntimeError("no live Wayland socket; start the desktop session first")
    if not Path(PLAYER).is_file():
        raise RuntimeError("standalone mpv binary is missing")
    if account is not None:
        # The process becomes the desktop user, then execs mpv. No background
        # controller, stale-player scan, signals or automatic retry are involved.
        os.initgroups(account.pw_name, account.pw_gid)
        os.setgid(account.pw_gid)
        os.setuid(account.pw_uid)
    os.execve(PLAYER, player_arguments(arguments, Path("/sys/module/santos_vdx").is_dir()), env)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, KeyError) as error:
        print(f"mpv: {error}", file=sys.stderr)
        sys.exit(2)
