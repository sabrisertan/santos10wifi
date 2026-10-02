# Keeping custom packages during Void updates

Custom packages sharing names with official Void packages need explicit
protection. Installing from a temporary local repository is not a version pin.
Inspect the installed packages and set hold for the actual custom ones:

```sh
xbps-query --list-hold-pkgs
xbps-query -p pkgver ptyxis
xbps-pkgdb -m hold ptyxis libadwaita gnome-control-center
xbps-query --list-hold-pkgs
xbps-install -n -u
```

The last command previews the normal update transaction; it does not install
anything. Use only package names actually installed on the target. For the
Santos development layout, custom runtime packages include `libhybris-santos`,
`santos-gtk-client-runtime`, `mpv-santos-cli` and `pipeline-santos`. The rebuilt
GDM/AccountsService ABI group must be assessed together with its installed
dependencies. Normal packages copied from a local mirror are not automatically
custom and should not all be frozen.

Hold skips ordinary update-all upgrades. An explicitly named update may override
the hold, so intentionally replacing a custom package still requires checking the
candidate. For a planned upgrade, remove its hold, supply the reviewed repository
and package, then restore the hold after validation:

```sh
xbps-pkgdb -m unhold PACKAGE
# Install and verify the reviewed replacement.
xbps-pkgdb -m hold PACKAGE
```

Repository locking instead permits updates only from the installation repository;
it is useful when that repository is persistent and curated. It should not bind
packages to discarded temporary build directories. See the
[Void Handbook](https://docs.voidlinux.org/xbps/advanced-usage.html).

Pipeline intentionally uses the normal `/usr/bin/yt-dlp` package. Leave yt-dlp
unheld so upstream extraction fixes can arrive independently of the graphical
application. There is no fixed yt-dlp release in the Pipeline source or recipe.
See [resolver details](PIPELINE.md).

The private GNOME 51 and GTK bundles in this layout are outside official package
paths. Normal package upgrades do not replace those directories, but their
system dependencies still need a coherent update transaction. A held package or
private path alone is not a guarantee of compatibility with every future library
ABI. No whole-system upgrade is accepted by the source checks in this repository.
