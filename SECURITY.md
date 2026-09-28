# Security assumptions

This preview preserves a single-user laboratory system. Root SSH control belongs
on a trusted, isolated management network. No SSH credentials or host keys are
provided. GNOME and applications run as an unprivileged desktop user, but the
reference GNOME launcher retains `--unsafe-mode` and private files under `/root`.
This is not a reviewed multi-user isolation design.

The Firefox ESR 78.15 experiment explicitly exports
`MOZ_DISABLE_CONTENT_SANDBOX=1`. Its successful rendering test is not a browser
security claim. Keep it away from sensitive accounts and untrusted browsing.
Changing the sandbox setting requires a new compatibility test; silently removing
the flag from the source would not validate the resulting browser.

Proprietary graphics libraries and firmware are external dependencies. Their
security and redistribution status are not established by this source preview.

When reporting a problem, include relevant component hashes and a minimal
reproducer. Remove credentials, serial numbers, network profiles, browsing data
and private URLs from logs. Do not attach rootfs images or private keys.
