# BusyBox build input

The exact Alpine Linux v3.22 aarch64 `busybox-static-1.37.0-r20.apk` is stored in Git LFS because the boot image depends on its precise bytes. It is GPL-2.0-only according to the package metadata. The corresponding BusyBox 1.37.0 source tarball and the Alpine packaging directory (APKBUILD, configuration, scripts, and patches) from aports commit `2e97d754a30d558f15524b8e422303c8a96832df` are included in Git LFS alongside it. The BusyBox source tarball includes its GPL license; unpack the packaging archive to inspect the Alpine build recipe and patches.

| File | SHA-256 |
| --- | --- |
| `busybox-static-1.37.0-r20.apk` | `ee469aee2958feffd7f64dd96655704025ff60af614f77a1d7323dc237c34da2` |
| `busybox-1.37.0.tar.bz2` | `3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4` |
| `alpine-busybox-packaging-2e97d754.tar.gz` | `5934796bd96427168e7a0c6e00c36a2c86e79ba8e1ec00e8819f99e4cf897e4e` |

The BusyBox source tarball's SHA-512 matches the pinned Alpine APKBUILD. Upstream locations: [Alpine package](https://dl-cdn.alpinelinux.org/alpine/v3.22/main/aarch64/busybox-static-1.37.0-r20.apk), [Alpine APKBUILD](https://raw.githubusercontent.com/alpinelinux/aports/2e97d754a30d558f15524b8e422303c8a96832df/main/busybox/APKBUILD), and [BusyBox source](https://busybox.net/downloads/busybox-1.37.0.tar.bz2). The APK is used as a build input; rebuilding the Alpine package requires its documented Alpine build environment and dependencies.
