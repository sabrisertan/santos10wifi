# Contributing

Start with the architecture, known issues and source manifest. Keep source changes,
build results and device observations distinct. Include the exact base, config,
commands and relevant hashes in a change description.

For device tests, record kernel release, boot identity and **loaded** module build
IDs. An on-disk module hash does not prove which module is running. Test normal
operation, errors, concurrency and recovery as separate conditions.

Do not attach credentials, private profiles or vendor images to issues. Keep raw
local evidence outside this public repository. Report regressions with a short
reproducer, expected/observed behavior and component IDs.

Markdown-only edits (including externally hosted README screenshots) do not require
updating `SHA256SUMS`. Link and private-data checks still apply to documentation.

After reviewing source, patch, configuration or tool changes, run
`python3 tools/update-manifest.py`, then `python3 tools/verify-release.py` and
`python3 -B -m unittest discover -s tests -v`. Commit the regenerated manifest
with the source change. CI checks source integrity and hygiene; it cannot certify
hardware behavior. See [REPRODUCING.md](REPRODUCING.md) for the build boundaries.
