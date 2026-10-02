# Santos standalone mpv

`mpv-santos-cli` restores the normal `/usr/bin/mpv` command and `mpv.desktop`.
It is a CLI-only binary package: it does not replace system libmpv, Pipeline,
GTK, libhybris, the compositor, the kernel or the audio service.

The mpv 0.41.0 executable is the existing hardened Santos build: custom VDX
H.264 decoder, legacy Wayland EGL support, explicit decoder runtime errors,
and padded-NV12 upload fixes. The private Wayland/TLS compatibility files are
verified copies of the retained accepted runtime, under `/usr/lib/mpv-santos-cli`.
No library under `/usr/lib/santos-hybris` is overwritten.

## Use

```sh
mpv video.mp4
mpv --fs video.mp4
mpv --vd=h264 video.mp4  # explicit software H.264 comparison
mpv --version
```

Video needs a live desktop Wayland session. From root SSH, the launcher finds
the exact Santos GNOME executable, takes only its session connection variables,
drops to the `santos` user and execs mpv. It never signals an existing player,
starts a controller, retries, respawns or restarts a desktop/audio service.
Version/help work without a desktop connection.

Audio uses the desktop user's PipeWire Pulse socket, not the old global Pulse
socket or direct ALSA. The launcher strips inherited GTK/Mesa library routing
and supplies scoped PowerVR/hybris routing. User config loading is unchanged;
device defaults are prepended, so explicitly supplied later options still win.
No timing-offset or frame-rate workaround is added.

VDX is preferred when its module is already loaded. Unsupported streams can
fall back at decoder initialization; runtime decoder errors terminate clearly.
There is no seamless runtime software fallback or automatic crash recovery.
720p and other supported H.264 formats require actual acceptance tests; a
successful build or version output does not prove playback or frame cadence.

## Rollback

```sh
xbps-remove -y mpv-santos-cli
update-desktop-database -q /usr/share/applications
```

Do not remove libhybris, unload GPU/video modules or restart GNOME as part of
this rollback. Pipeline and its private libmpv remain independent.

Source/build provenance and distribution hashes are in the restoration task's
report and frozen artifact manifest. This binary overlay is not a clean
upstream-to-device xbps-src rebuild claim.
