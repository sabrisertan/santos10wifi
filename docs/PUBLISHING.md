# Publishing updates

Canonical repository: [sabrisertan/santos10wifi](https://github.com/sabrisertan/santos10wifi).
The source preview was published on 2026-09-28. The repository contains source,
selected integration references and documentation; images and private evidence
remain outside it.

For intentional updates in a clone:

```sh
python3 tools/update-manifest.py
python3 tools/verify-release.py
python3 -B -m unittest discover -s tests -v
git diff --check
git status --short
```

Review the changes, commit them, then push the prepared `main` branch:

```sh
git push origin main
```

The owner's SSH remote is `git@github.com:sabrisertan/santos10wifi.git`. Publish
only this repository, not the surrounding development workspace. Check the
[Actions results](https://github.com/sabrisertan/santos10wifi/actions) for the
pushed commit.

Use “experimental source preview” in release descriptions. The
[image-release assessment](IMAGE-RELEASE.md) records why a user-installable image
is deferred. Leave firmware and boot/rootfs images out of release attachments
until their independent build, licensing, installation and recovery gates are
complete. Do not describe pending physical and full-system tests as passed.
