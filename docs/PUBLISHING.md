# Publishing this source preview

The prepared repository is a standalone Git repository. Publish **this directory**,
not its parent development workspace. It contains source, selected integration
references and documentation; images and private evidence remain outside it.

1. Run `python3 tools/verify-release.py` and check `git status --short`.
2. Create an empty GitHub repository named `santos10wifi` in the `sabrisertan` account.
   Do not initialize a second README/license in the GitHub creation form.
3. Use the configured target below to push the prepared `main` branch:

```sh
git remote add origin git@github.com:sabrisertan/santos10wifi.git
git push -u origin main
```

Suggested description: “Experimental Linux 7.2 and GNOME port for the Samsung
Galaxy Tab 3 10.1 Wi-Fi (GT-P5210), with SGX544 and MSVDX integration.”

Use “experimental source preview” in the release description. Leave firmware and
boot/rootfs images out of release attachments until their independent build,
licensing, installation and recovery gates are complete. Do not describe the
pending physical and full-system tests as passed.
