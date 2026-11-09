# uMTP Responder directory scan patch

This patch applies to upstream uMTP Responder tag `umtprd-1.3.10`, commit
`52c38a4a756b30efecc6470b5e4d4f22f9187a45`, from
https://github.com/viveris/uMTP-Responder. The upstream code is GPL-3.0-or-later;
its license is included here. The patch skips symlink entries and entries that
cannot be stated, and continues scanning the remaining directory entries. It
also removes a double-close path when the first entry cannot be stated.
Symlinks are deliberately absent from MTP listings, including links to files
within the world. This is a listing fix, not a replacement for the MTP sandbox.

In an ARM64 Debian development environment with Git, a C compiler and make:

```sh
git clone https://github.com/viveris/uMTP-Responder.git /tmp/n20-umtp-source
git -C /tmp/n20-umtp-source checkout 52c38a4a756b30efecc6470b5e4d4f22f9187a45
git -C /tmp/n20-umtp-source apply /absolute/path/to/skip-unreadable-and-symlinks.patch
make -C /tmp/n20-umtp-source
python3 tests/test-umtp-scan.py /tmp/n20-umtp-source
```

The patched source built and passed the isolated scan test on the phone. Windows
enumerated a disposable world with broken and external links and transferred a
marker file successfully. The patched responder is installed at
`/usr/local/lib/n20/umtprd`; the sandbox binds that binary into its jail as
`/usr/bin/umtprd`. The Debian package binary is untouched. Install the patched
binary before the updated sandbox, and restart MTP through the gadget procedure.
The executable and its parent directories must remain root-owned and not
writable by the game account.

To roll back, restore the previous sandbox binary that binds the packaged
`/usr/bin/umtprd`, then restart MTP. The unpatched responder can truncate a
directory listing at a broken symlink. A package upgrade does not update the
custom binary: review and rebuild this patch when updating the responder.
