# Persistent r7 VDX integration reference

`santos-vdx-runtime.py` is the accepted future-boot selection helper. It does not
unload or replace a live SGX/VDX module. It validates the matching kernel/module
ELF identities, file ownership/hashes, complete preserved baseline and loader
before selecting a bundle. An existing attempted/ready boot is retained; an
incomplete hardware start is not silently retried. Rollback changes the next
boot's loader and leaves current modules alone.

The public r7 sources add page-backed large BO/RENDEC storage with per-page MMU
mapping, guarded debug breadcrumbs, corrected WAITIDLE reservation ordering and
opt-in shared-MSI completion. Polling remains the default (`eng_use_irq=0`) and
the recovery path; the accepted matching device profile explicitly selects IRQ.
Fault diagnostics remain available with quiet hot-path logging.

The existing integration uses `/opt/santos-vdx-r7`, `/etc/sv/santos-pvr` and
root-owned generated manifests, copied matching modules and baseline backups.
Those installation inputs, binary modules and vendor firmware are not included
here. Copying this helper alone is not an installer. Generate identities from
your actual build and preserve an independently usable recovery baseline before
any device deployment. Never treat a failed DMA wait as proof that hardware has
stopped.

Normal matching 720/1080/repeat playback and actual IRQ delivery passed on the
development tablet after a clean boot. Forced fragmentation, fault/recovery,
concurrency/pressure and completion-latency/CPU/FPS improvement remain separate.
