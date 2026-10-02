# Pipeline playback and system yt-dlp

The October 3 source update includes the public Pipeline name/icon, source-quality
selection, resolver environment, recovery and navigation fixes. The recipe is
`pipeline-santos-4.1.0_6`; its cumulative patch reconstructs the reviewed source
used for the accepted private application build.

## Resolver updates

Pipeline executes the `yt-dlp` command and parses its `--dump-single-json` output.
The static application-specific network helper forwards arguments to
`/usr/bin/yt-dlp`. It removes the graphical application's preload/library
environment before Python starts, retaining the known networking-only TLS shim.
No yt-dlp release number, bundled extractor snapshot or equality dependency is
encoded in Pipeline. The Void recipe depends on `yt-dlp` without a version pin.
Normal yt-dlp package updates are intended to remain enabled.

Streaming preferences default to a maximum of 720p and offer 360/480/720/1080.
This is a hardware compatibility policy, independent of the yt-dlp version:
accepted video is H.264/MP4 at no more than 30 fps. A source with only 60 fps HD
formats may select a smaller compatible stream. Actual source/decoded dimensions
are separate from the viewport; selecting 720 does not upscale a smaller source.

## Card navigation

Popping the video page into the bottom card previously stopped playback during
GtkGLArea unrealize. Freeing libmpv's render context also destroys the active
video output, so retaining only the media URL was insufficient. The page now
retains its exact GDK GL context and libmpv renderer across card navigation.
Background state/time updates continue without hidden framebuffer rendering.

Returning waits for a valid decoded frame and GTK paint. A seekable paused stream
refreshes a frame at its existing position while keeping pause and media
generation. Explicit stop, card dismissal, replacement and window close cancel
pending work and release the renderer with its owning context current.

The matching build passed 36 native Rust tests and six device cases: repeated
minimize/expand with background play/pause, paused return with checked pixels,
hidden close, hidden EOF, dismissal and real YouTube playback. Hidden EOF checks
the final state; it does not accept playback restart after EOF. General paused
recovery, unseekable live streams, corrupt media, long network outages,
concurrency/pressure and Fullscreen remain separate.

## Packaging

The recipe owns the private application/libmpv/resource/schema/network-helper
layout and the public launcher. It has no dependency on the standalone mpv
package. Its install hook requires Pipeline to be closed normally before
replacement; it does not kill a player or restart GNOME. The private libmpv does
not advertise a system-wide SONAME provider. See [custom package protection](UPDATING.md)
and [build instructions](BUILDING.md).
