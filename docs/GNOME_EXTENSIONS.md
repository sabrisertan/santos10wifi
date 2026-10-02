# GNOME 51 touch gestures and an autohiding dock

The private GNOME 51 desktop was tested with two unmodified upstream extensions:

| Extension | Accepted version | Official source |
|---|---|---|
| Dash to Dock | 109 | [GNOME Extensions](https://extensions.gnome.org/extension/307/dash-to-dock/) |
| TouchUp | 1.7.1 / 22 | [GNOME Extensions](https://extensions.gnome.org/extension/8102/touchup/), [author repository](https://github.com/mityax/gnome-extension-touchup) |

Both archives declare Shell 51 support. Their download URLs, SHA256 hashes and
runtime scope are recorded in
[gnome-extensions-validation.json](../configs/gnome-extensions-validation.json).
No Shell-version validation bypass was enabled. The existing session and loaded
GPU/video modules remained active throughout the tests.

The dock sits at the bottom with 48 px icons, autohide, pressure activation
disabled, a 150 ms transition and fixed opacity. Intellihide, dynamic colors,
mount/trash icons and window thumbnails are disabled. TouchUp reserves a 22 px
gesture strip, which also stops its recurring navbar style polling. The profile
keeps navigation, panel/notification gestures and keyboard gestures. Virtual
touchpad, floating rotation, key popups, quick paste and double-tap sleep are
disabled.

From the bottom strip, a short upward swipe reveals the dock; it hides again
after release. A longer upward swipe opens the overview. Swipe upward from the
leftmost 100 px to open the keyboard. These routes passed through a native
Clutter virtual touchscreen, with gesture callbacks and actual Shell state
readback; screenshots confirmed the visible dock, overview and keyboard.
The test returned to the ordinary desktop and removed the probe device.
Physical finger feel, long sessions, battery use and application FPS remain
outside this bounded test.

Install the official archive chosen for Shell 51 into the normal per-user
extension directory using the Shell extension installer. This also compiles
the schema; raw archive extraction alone needs a native
`glib-compile-schemas --strict EXTENSION_DIRECTORY/schemas` step. Enable Dash to
Dock first and TouchUp second. On a fresh running session, use the supported
extension manager/catalogue path to load them or log in again; copying a
directory alone does not register a live extension. The reference device used
its running 51 manager and did not restart the desktop.

The exact settings are in
[gnome-extension-profile.json](../integration/desktop/gnome-extension-profile.json).
For each extension, back up its `dconf_path` using `dconf dump`, then apply
the listed values with `gsettings --schemadir EXTENSION_DIRECTORY/schemas set
SCHEMA KEY VALUE`. To revert, disable only these two extensions, restore their
scoped dconf backups and the prior enabled/disabled UUID membership. Keep other
extensions and user preferences intact.

A warm comparison in the same Shell process used 20 idle seconds per mode, with
no application windows or concurrent package, schema or screenshot work:

| Enabled features | Shell CPU, one core | Shell RSS |
|---|---:|---:|
| Both disabled | 1.48% | 161.3 MiB |
| Dock only | 1.93% | 162.0 MiB |
| Dock + TouchUp | 1.63% | 162.6 MiB |

The dock-only CPU difference was 0.45 percentage points of one core; both
extensions added 1.3 MiB resident memory in this warm toggle comparison. The
small CPU differences are a short idle sample, with normal sampling variation.
Imported code and caches remain resident when extensions are disabled, so this
is not a cold-start memory-cost measurement or a general animation/FPS result.
Both extensions were re-enabled and remained free of manager errors afterward.
