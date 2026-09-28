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

After an intentional release edit, run `python3 tools/update-manifest.py` in a reviewed change
and run `python3 tools/verify-release.py`. CI checks source-package integrity and
hygiene; it cannot certify hardware behavior.
