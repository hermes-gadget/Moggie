# Moggie board support (br2-external tree)

This directory is a Buildroot `BR2_EXTERNAL` tree. It produces the squashfs
root filesystem for the Sonos Play:5 Gen 1 (PowerPC MPC8315, e300c3, big
endian, hard float).

Build it through the repository script, which verifies the Buildroot and
toolchain hashes, drives the build, pads the image to the NAND eraseblock and
enforces the slot budget:

```sh
scripts/build-rootfs.sh            # incremental
scripts/build-rootfs.sh --clean    # full rebuild
scripts/build-rootfs.sh --measure  # measure what is already built
```

Artifacts land in `out/`: `rootfs.squashfs` (raw), `rootfs.nand.img` (padded
to 128 KiB) and `rootfs.sizes.txt`.

## Layout

| Path | Purpose |
|---|---|
| `configs/moggie_powerpc_defconfig` | the whole configuration |
| `external.desc`, `external.mk`, `Config.in` | br2-external plumbing |
| `package/sonosd/` | in-repo daemon, built with the target cross compiler |
| `board/moggie/rootfs-overlay/` | `/etc` policy: inittab, rcS, passwd, fstab, udhcpc hook |
| `board/moggie/device_table_dev.txt` | static `/dev` nodes (no devtmpfs in the kernel) |
| `board/moggie/device_table.txt` | ownership/mode table applied under fakeroot |
| `board/moggie/busybox.fragment` | applets the firmware requires |
| `board/moggie/post-build.sh` | modes, writable-path symlinks, provenance, sanity checks |

## Boot and init

`/etc/inittab` (busybox init):

- `::sysinit:/etc/init.d/rcS` — mounts `/proc`, `/sys`, `/dev`, `/tmp`, `/run`,
  `/var/log`; brings up `lo` and `eth0` (DHCP with a 169.254/16 fallback);
  mounts mtd1 as jffs2 on `/jffs` if it is formatted; optionally starts sshd.
- `ttyS0::respawn:...getty -l /bin/sh` — a root shell on the UART, no password.
- `::respawn:/usr/sbin/sonosd` — init supervises the daemon.
- `::shutdown:` entries unmount and sync.

Package-provided `/etc/init.d/S*` scripts are deliberately **not** shipped:
rcS is the single entry point, and it starts sshd only when asked to.

## Login policy

Every password in `/etc/shadow` is locked (`!`). Nothing authenticates with a
password:

- the UART console gives a root shell directly (physical access is the trust
  boundary);
- dropbear is installed but does not listen unless
  `/jffs/ssh/{enable,host_key,authorized_keys}` exist, and it always runs with
  `-s` (password auth disabled).

`/etc/passwd` keeps `/bin/sh` as root's shell because that is what the console
getty execs. There is no other login account.

## Writable state

The rootfs is a read-only squashfs. Everything writable is a tmpfs or jffs2:

| Path | Backing |
|---|---|
| `/dev`, `/tmp`, `/run`, `/var/log` | tmpfs, mounted by rcS |
| `/jffs` | mtd1, 7 MB jffs2 (settings, `sonosd.conf`, ssh keys) |
| `/etc/resolv.conf` | symlink to `/run/resolv.conf` |
| `/etc/sonosd.conf` | symlink to `/jffs/sonosd.conf` (may dangle) |

## Target quirks that shaped this tree

- **No devtmpfs.** The 2.6.35 kernel was built without `CONFIG_DEVTMPFS`, so
  `/dev` is static in the image and rcS layers a tmpfs + `mdev -s` on top.
- **squashfs is zlib only.** The kernel has `CONFIG_SQUASHFS` with no XZ/LZMA
  decoder, so the image is `-comp gzip`, 128 KiB blocks, no xattrs.
- **glibc is newer than the kernel.** See `../MANIFEST.md` — the image is not
  hardware-ready as built.
