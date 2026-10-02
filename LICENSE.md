# Licenses and upstream credits

This repository contains multiple upstream projects. There is no blanket license
replacement for imported code. Preserve each file's copyright, license header
and upstream notices when redistributing or changing it.

| Component | License reference |
|---|---|
| Linux platform changes and VDX | GPL-2.0; see `LICENSES/Linux-COPYING`, `LICENSES/GPL-2.0`, file SPDX identifiers and the upstream Linux license tree |
| Santos CPUFreq driver, manager and tests | Project-specific GPL-2.0-only; hardware oracle is the Intel/Samsung 3.4 SFI driver, not a redistributed vendor binary |
| PowerVR Services4 | Original per-file terms, including Dual MIT/GPLv2 notices; retained in the provider sources |
| GTK | Upstream LGPL terms; `LICENSES/GTK-COPYING` and upstream per-file notices |
| Mutter | `LICENSES/Mutter-COPYING` and upstream per-file notices |
| GNOME Shell | `LICENSES/GNOME-Shell-COPYING` and upstream per-file notices |
| Pipeline | GPL-3.0-or-later; `LICENSES/Pipeline-LICENSE` |
| mpv, libhybris, Android headers and Void recipes | Their upstream licenses, recorded in each recipe and source header; patches remain modifications of those projects |
| Firefox modifications | Mozilla upstream file licenses; fetch the matching upstream source with its license notices |

New publication helpers under `tools/` and publication documentation are provided
under GPL-2.0-only. Other imported integration files retain their existing terms;
files without explicit notices need provenance clarification before claiming a
blanket license for them.

Credit belongs to the Linux, openpvrsgx, Samsung/Intel/Imagination, GNOME, libhybris,
Void Linux, mpv, Pipeline and Mozilla contributors and the Santos port contributors.
Upstream authors are not represented as endorsing this port.

Vendor graphics blobs, firmware, boot images and private rootfs content are not
included or granted redistribution rights by this repository. A binary release
needs a separate inventory of its licenses and corresponding source obligations.
