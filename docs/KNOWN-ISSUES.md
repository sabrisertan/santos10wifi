# Known bugs and limitations

Snapshot: 2026-09-28. IDs refer to the development project's findings registry.
“Accepted” is scoped to the tests performed on one GT-P5210.

| Area / ID | Impact and current boundary |
|---|---|
| Browser / L243 | ESR 78.15 compatibility build, content sandbox disabled, private EGL and matching private `libwayland-egl` required. Final launcher visual acceptance remains pending. No secure modern-browser claim. |
| Video / L247–L248 | VO synchronous wait and periodic main-thread metric stalls were fixed in the installed candidate. Captured YouTube runs had zero strict deadline drops and no mid-playback >100 ms submit gaps, but application supersede drops remain. Observed submission rate was about 20–23 fps. A 720p30 fixture still had 58 strict drops and 251 supersedes. Final physical acceptance is pending. |
| Allocation / L194, L213 | Long-uptime memory fragmentation can prevent contiguous PVR/VDX allocations and video startup. A clean boot was required in affected runs. Do not live-unload GPU/VDX modules to recover. |
| Memory pressure | September 28 read-only inspection found historical GNOME order-0 allocation warnings in the current boot's log. They do not establish a new root cause; no new stress test was performed. |
| Session restart / L205–L206 | Earlier service-restart runs presented three frames then stalled. Service restart is not accepted as equivalent to a clean boot. |
| Video recovery / L188, L197, L220 | Provider/network long-tail recovery, opening-frame corruption and some recovery cover behavior remain incompletely accepted. Recent timing fixes do not close these independent gates. |
| High resolution / L195 | Real-time 1080p presentation is not accepted. Decoder throughput and presentation cadence are separate limits. |
| External player / L246 | Rejected external-player experiment has a crash-budget reset defect. Retained source is experimental; the embedded Pipeline path is the publication baseline. |
| Renderer retry / L210 | A failed GtkGLArea context cannot be restored by merely clearing player state; widget/context recreation remains a separate gap. |
| Synchronization / L72 | HWC native-window retire-fence stub remains deferred. No general synchronization-correctness claim. |
| GTK / L137–L139, L184–L185 | Direct DMABUF import is unimplemented; upload fallback and conservative offscreen opacity are used. Startup latency remains noticeable. |
| Desktop integration / L129 | Settings Chassis query lacks a hostname1 activation provider. |
| Recovery | This source preview has no universal flash procedure. Development p11 is a preserved rollback candidate, not an Android 3.4 recovery installation. |
| Packaging / L251 | Original local Pipeline recipe omitted later source changes. The publication recipe now uses one verified cumulative patch; a fresh complete desktop package build is still untested. |
| Portability | Some session/GTK/Firefox references use `/root/santos`, `/opt`, user `santos` and UID 1001. They document the current integration, not portable installation defaults. |

System suspend, cameras, Bluetooth, external monitors and other tablet models are
not covered by this release's acceptance claims. The frozen Qt fallback is not
part of this GNOME source preview.

A stable end-user image would require a clean desktop build/bootstrap, a new
30-minute regression run, cold-boot network/power checks, physical video/browser
acceptance, and an independently tested recovery procedure for its exact image.
These pending gates do not prevent sharing this experimental source snapshot.
