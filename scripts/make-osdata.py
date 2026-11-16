"""Populate osdata/, the directory that becomes the ext4 root of main.img.

osdata/ is deliberately not tracked by git (it holds binaries), so a fresh
checkout has no root filesystem to put in the image. Without it the image
boots the bootloader and the kernel and then stops with no shell at all, which
looks like a storage-driver bug rather than a missing file.

This script builds the minimum root that starts a shell: a static busybox with
a `sh` symlink, and /etc/init.sh. busybox is taken from --busybox if given,
otherwise searched for on PATH, and finally downloaded from the busybox.net
static binaries mirror. Everything is written under --out only; nothing else on
the machine is touched.
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys
import urllib.request

DEFAULT_BUSYBOX_URLS = (
    "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox",
    "https://busybox.net/downloads/binaries/1.31.0-i686-uclibc/busybox",
)

INIT_SH = """#!/bin/sh
# Minimal BoldonOS init: bring up the pseudo filesystems, then hand over to a
# shell. /etc/init.sh is executed by launch_init_process() in the kernel.
export PATH=/bin:/usr/bin:/sbin:/usr/sbin
export HOME=/root
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mkdir -p /dev /tmp 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
echo "[INIT] hello from init.sh"
exec /bin/sh
"""


def find_busybox(explicit):
    if explicit:
        if not os.path.isfile(explicit):
            raise SystemExit(f"--busybox {explicit!r} is not a file")
        return explicit
    for candidate in ("busybox", "busybox.static"):
        found = shutil.which(candidate)
        if found:
            return found
    return None


def is_static_elf(path):
    """Reject a dynamically linked busybox: the root has no libc in it."""
    try:
        with open(path, "rb") as handle:
            if handle.read(4) != b"\x7fELF":
                return False
            handle.seek(0)
            data = handle.read(4096)
    except OSError:
        return False
    # PT_INTERP is the dynamic loader; a static binary has no such segment.
    if sys.byteorder == "little":
        return b"/lib/ld-" not in data and b"/lib64/ld-" not in data
    return True


def download_busybox(dest):
    errors = []
    for url in DEFAULT_BUSYBOX_URLS:
        try:
            print(f"==> downloading busybox from {url}")
            with urllib.request.urlopen(url, timeout=60) as response:
                payload = response.read()
            if payload[:4] != b"\x7fELF":
                errors.append(f"{url}: not an ELF file")
                continue
            with open(dest, "wb") as handle:
                handle.write(payload)
            return
        except Exception as exc:                     # noqa: BLE001
            errors.append(f"{url}: {exc}")
    raise SystemExit("could not obtain a static busybox:\n  " +
                     "\n  ".join(errors) +
                     "\nPass --busybox PATH to use one you already have.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="osdata")
    ap.add_argument("--busybox", help="static busybox binary to install")
    args = ap.parse_args()

    bin_dir = os.path.join(args.out, "bin")
    etc_dir = os.path.join(args.out, "etc")
    os.makedirs(bin_dir, exist_ok=True)
    os.makedirs(etc_dir, exist_ok=True)

    target = os.path.join(bin_dir, "busybox")
    source = find_busybox(args.busybox)
    if source and is_static_elf(source):
        print(f"==> copying static busybox from {source}")
        shutil.copyfile(source, target)
    else:
        if source:
            print(f"==> {source} is dynamically linked, fetching a static one")
        download_busybox(target)

    os.chmod(target, 0o755)

    # busybox decides what to run from argv[0]; /bin/sh must be a link to it.
    sh_path = os.path.join(bin_dir, "sh")
    if os.path.islink(sh_path) or os.path.exists(sh_path):
        os.remove(sh_path)
    os.symlink("busybox", sh_path)
    for extra in ("mount", "mkdir", "ls", "cat", "echo"):
        path = os.path.join(bin_dir, extra)
        if not os.path.lexists(path):
            os.symlink("busybox", path)

    init_path = os.path.join(etc_dir, "init.sh")
    with open(init_path, "w") as handle:
        handle.write(INIT_SH)
    os.chmod(init_path, 0o755)

    print(f"==> wrote {args.out}/bin/busybox, /bin/sh and {args.out}/etc/init.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
