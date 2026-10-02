# Known bugs and limitations

Baseline snapshot: 2026-09-28; source/runtime follow-up: 2026-10-03. IDs refer to the development project's findings registry.
“Accepted” is scoped to the tests performed on one GT-P5210.

| Area / ID | Impact and current boundary |
|---|---|
| Browser / L243 | ESR 78.15 compatibility build, content sandbox disabled, private EGL and matching private `libwayland-egl` required. Final launcher visual acceptance remains pending. No secure modern-browser claim. |
| Video / L247–L248 | VO synchronous wait and periodic main-thread metric stalls were fixed in the installed candidate. Captured YouTube runs had zero strict deadline drops and no mid-playback >100 ms submit gaps, but application supersede drops remain. Observed submission rate was about 20–23 fps. A 720p30 fixture still had 58 strict drops and 251 supersedes. Final physical acceptance is pending. |
| Allocation / L194, L213 | r7 large VDX BO/RENDEC allocation now uses page-backed storage and per-page MMU mapping; normal 720/1080/repeat and EOF release passed after clean boot. Forced fragmentation/pressure and other PVR allocations remain separate. Do not live-unload GPU/VDX modules to recover. |
| Memory pressure | September 28 read-only inspection found historical GNOME order-0 allocation warnings in the current boot's log. They do not establish a new root cause; no new stress test was performed. |
| Session restart / L205–L206 | Earlier service-restart runs presented three frames then stalled. Service restart is not accepted as equivalent to a clean boot. |
| Video recovery / L188, L197, L220 | Provider/network long-tail recovery, opening-frame corruption and some recovery cover behavior remain incompletely accepted. Recent timing fixes do not close these independent gates. |
| High resolution / L195 | One matched 1080p30 fixture at 1.6 GHz passed 300/300, zero sampled drops and maintainer smoothness; its 800 MHz control had 190 drops on a different boot. No general media/allocator/long-tail acceptance. |
| CPU frequency / L260, L267 | Missing SFI driver and incorrect split-policy model corrected; managed performance active with package/load/lease/recovery and final-service coldboot tests. Sustained thermal, hotplug/suspend and fresh full-system installation remain separate. |
| VDX port / L261–L263 | r7 has opt-in shared-MSI notification with real IRQ delivery, corrected WAITIDLE reservation ordering and quiet hot-path logging. Polling remains the default/recovery path. Concurrency/fault/pressure and completion-latency/CPU/FPS benefits remain unaccepted. |
| Native idle / L268 | Cloverview C-state path is absent from the current config/model support, with no active cpuidle driver/states. Idle-power/thermal effect is a candidate, not a proven FPS cap; fallback idle is not equivalent to a busy-loop. |
| External player / L246 | Rejected external-player experiment has a crash-budget reset defect. Retained source is experimental; the embedded Pipeline path is the publication baseline. |
| Renderer retry / L210 | Fresh-widget/context Retry has a scoped device gate in the current Pipeline build. General first-surface, paused recovery, corrupt media and provider long-tail remain separate. |
| Pipeline / L272–L278 | Quality/resolver/recovery/source-lifecycle fixes and repeated background/card-return, paused pixels, explicit cleanup and real YouTube gates passed. General live/unseekable stream, long-network, Fullscreen and concurrency acceptance remains separate. |
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
