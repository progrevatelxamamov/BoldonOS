#!/usr/bin/env python3
"""Create a bootable BoldonOS main.img (GPT + ESP + ext4 root) in pure Python.

This mirrors the ``make main.img`` recipe, but avoids sgdisk/mtools so that the
disk can also be produced on machines where those utilities are missing.
The guest root filesystem is filled from a directory using debugfs.
"""
import argparse
import os
import struct
import subprocess
import shutil
import sys
import tempfile
import uuid

SECTOR = 512
MIB = 1024 * 1024

GPT_LINUX_FS = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")
GPT_ESP = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B")
GPT_BIOS_BOOT = uuid.UUID("21686148-6449-6E6F-744E-656564454649")
GPT_DATA = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")


def guid_bytes(u: uuid.UUID) -> bytes:
    """GPT stores the first three UUID fields little endian (mixed endian)."""
    b = u.bytes
    return (b[0:4][::-1]) + (b[4:6][::-1]) + (b[6:8][::-1]) + b[8:]


def chs(lba: int) -> bytes:
    # 64 heads, 32 sectors per track (matches the Makefile geometry)
    heads, spt = 64, 32
    cyl = lba // (heads * spt)
    head = (lba // spt) % heads
    sect = lba % spt + 1
    if cyl > 1023:
        return b"\xff\xff\xff"
    return bytes([head, (sect & 0x3F) | ((cyl >> 2) & 0xC0), cyl & 0xFF])


def pmbr(disk_guid: uuid.UUID, first_lba: int, last_lba: int, part_type=0xEE) -> bytes:
    mbr = bytearray(SECTOR)
    # boot code: jump + the conventional protective MBR message
    code = b"\x33\xc0\xfa\x8e\xd0\xbc\x00\x7c"
    mbr[0:len(code)] = code
    mbr[0x1BE] = 0x00
    mbr[0x1BF:0x1C2] = chs(first_lba)
    mbr[0x1C2] = part_type
    mbr[0x1C3:0x1C6] = chs(last_lba)
    mbr[0x1C6:0x1CA] = struct.pack("<I", min(first_lba, 0xFFFFFFFF))
    mbr[0x1CA:0x1CE] = struct.pack("<I", min(last_lba - first_lba + 1, 0xFFFFFFFF))
    mbr[0x1FE:0x200] = b"\x55\xaa"
    return bytes(mbr)


def gpt_entry(type_guid: uuid.UUID, part_guid: uuid.UUID, first: int, last: int, name: str) -> bytes:
    e = bytearray(128)
    e[0:16] = guid_bytes(type_guid)
    e[16:32] = guid_bytes(part_guid)
    e[32:40] = struct.pack("<Q", first)
    e[40:48] = struct.pack("<Q", last)
    e[48:56] = struct.pack("<Q", 0)
    encoded = name.encode("utf-16-le")[:72]
    e[56:56 + len(encoded)] = encoded
    return bytes(e)


def crc32(data: bytes) -> int:
    import zlib
    return zlib.crc32(data) & 0xFFFFFFFF


def build_gpt(img_path: str, entries):
    """entries: list of (type_guid, first_lba, last_lba, name)."""
    total_sectors = os.path.getsize(img_path) // SECTOR
    last_lba = total_sectors - 1
    disk_guid = uuid.uuid4()

    table = bytearray(128 * 128)
    for i, (type_guid, first, last, name) in enumerate(entries):
        table[i * 128:(i + 1) * 128] = gpt_entry(type_guid, uuid.uuid4(), first, last, name)
    table_crc = crc32(bytes(table))

    header = bytearray(SECTOR)
    header[0:8] = b"EFI PART"
    struct.pack_into("<I", header, 8, 0x00010000)
    struct.pack_into("<I", header, 12, 92)
    struct.pack_into("<I", header, 16, 0)
    struct.pack_into("<I", header, 20, 0)
    struct.pack_into("<Q", header, 24, 1)
    struct.pack_into("<Q", header, 32, last_lba)
    struct.pack_into("<Q", header, 40, 34)
    struct.pack_into("<Q", header, 48, last_lba - 33)
    header[56:72] = guid_bytes(disk_guid)
    struct.pack_into("<Q", header, 72, 2)
    struct.pack_into("<I", header, 80, 128)
    struct.pack_into("<I", header, 84, 128)
    struct.pack_into("<I", header, 88, table_crc)
    struct.pack_into("<I", header, 92, 0)
    header_crc = crc32(bytes(header[:92]))
    struct.pack_into("<I", header, 16, header_crc)

    # backup header / entries at the end of the disk
    backup_header = bytearray(header)
    struct.pack_into("<Q", backup_header, 24, last_lba)
    struct.pack_into("<Q", backup_header, 32, 1)
    struct.pack_into("<Q", backup_header, 48, 2)
    struct.pack_into("<I", backup_header, 16, 0)
    struct.pack_into("<I", backup_header, 88, table_crc)
    struct.pack_into("<I", backup_header, 16, crc32(bytes(backup_header[:92])))

    with open(img_path, "r+b") as f:
        f.seek(0)
        f.write(pmbr(disk_guid, 1, last_lba))
        f.seek(SECTOR)
        f.write(bytes(header))
        f.seek(2 * SECTOR)
        f.write(bytes(table))
        f.seek((last_lba - 32) * SECTOR)
        f.write(bytes(table))
        f.seek(last_lba * SECTOR)
        f.write(bytes(backup_header))


def read_file(path):
    with open(path, "rb") as handle:
        return handle.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="main.img")
    ap.add_argument("--splash", default="boldonsplash.bmp")
    ap.add_argument("--kernel", default="kernel/bin/kernel")
    ap.add_argument("--licenses", default="kernel/third_party/libpayload/LICENSES.md")
    ap.add_argument("--root", default="osdata", help="directory copied into the ext4 root")
    ap.add_argument("--size-mib", type=int, default=512)
    ap.add_argument("--uefi", default="limine-binary/BOOTX64.EFI")
    ap.add_argument("--esp-mib", type=int, default=128)
    ap.add_argument("--mkfs", default="/sbin/mke2fs")
    ap.add_argument("--debugfs", default="/sbin/debugfs")
    ap.add_argument("--e2fsck", default="/sbin/e2fsck")
    ap.add_argument("--allow-empty-root", action="store_true",
                    help="build even if the root directory has no shell")
    ap.add_argument("--limine", default="limine-binary/limine")
    ap.add_argument("--limine-conf", default="limine.conf")
    args = ap.parse_args()

    if not args.allow_empty_root:
        verify_root_payload(args.root)

    size_bytes = args.size_mib * MIB
    with open(args.out, "wb") as f:
        f.truncate(size_bytes)

    # Partition layout mirrors the GNUmakefile: 1 MiB BIOS boot, 128 MiB ESP,
    # then the ext4 root with the remaining space.
    bios_first, bios_last = 2048, 2048 + 2048 - 1
    esp_first = bios_last + 1
    esp_last = esp_first + args.esp_mib * 2048 - 1
    root_first = esp_last + 1
    root_last = size_bytes // SECTOR - 34

    build_gpt(args.out, [
        (GPT_BIOS_BOOT, bios_first, bios_last, "BIOSBOOT"),
        (GPT_ESP, esp_first, esp_last, "BOOT"),
        (GPT_LINUX_FS, root_first, root_last, "BOLDON_ROOT"),
    ])

    # FAT32 ESP, written by the dependency-free writer in scripts/fat32.py so
    # the image can be produced without mtools/dosfstools/sgdisk.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import fat32

    esp_offset = esp_first * SECTOR
    esp_bytes = (esp_last - esp_first + 1) * SECTOR
    esp = bytearray(esp_bytes)
    esp_files = {"/boot/kernel": read_file(args.kernel)}
    if os.path.exists(args.splash):
        esp_files["/boot/boldonsplash.bmp"] = read_file(args.splash)
    if os.path.exists(args.licenses):
        esp_files["/boot/libpayload-LICENSES.md"] = read_file(args.licenses)
    if os.path.exists(args.limine_conf):
        esp_files["/boot/limine/limine.conf"] = read_file(args.limine_conf)
    # Limine's BIOS stage 1 only looks for stage 3 in /, /boot, /limine and
    # /boot/limine -- /EFI/BOOT is a UEFI convention it does not search.
    for name in ("limine-bios.sys",):
        path = os.path.join("limine-binary", name)
        if os.path.exists(path):
            esp_files[f"/boot/limine/{name}"] = read_file(path)
    if os.path.exists(args.uefi):
        esp_files["/EFI/BOOT/BOOTX64.EFI"] = read_file(args.uefi)
    fat32.build_fat32(esp, esp_files, volume_label="BOOT")
    with open(args.out, "r+b") as dst:
        dst.seek(esp_offset)
        dst.write(esp)

    # ext4 root filesystem
    root_img = os.path.join(tempfile.mkdtemp(prefix="boldon-root-"), "root.img")
    root_kib = (root_last - root_first + 1) * SECTOR // 1024
    subprocess.run([args.mkfs, "-q", "-F", "-t", "ext4", "-L", "BOLDON_ROOT",
                    "-U", "00000000-0000-4000-8000-00000000B01D",
                    root_img, str(root_kib)], check=True)
    copy_tree(args.debugfs, root_img, args.root)
    verify_root_image(args.e2fsck, root_img)
    with open(root_img, "rb") as src, open(args.out, "r+b") as dst:
        dst.seek(root_first * SECTOR)
        while True:
            chunk = src.read(MIB)
            if not chunk:
                break
            dst.write(chunk)
    shutil.rmtree(os.path.dirname(root_img), ignore_errors=True)

    # BIOS boot: limine's first stage lives in the BIOSBOOT partition
    # Limine's installers exec helper tools from its own datadir, and it may
    # not inherit a closed descriptor; run it with a plain environment.
    env = dict(os.environ)
    env.setdefault("PATH", "/usr/bin:/bin")
    subprocess.run([os.path.abspath(args.limine), "bios-install",
                    os.path.abspath(args.out)], check=True, env=env)
    print(f"wrote {args.out}: {args.size_mib} MiB, root {root_kib // 1024} MiB")


def verify_root_image(e2fsck: str, image: str):
    """Fail the build if the root filesystem we just populated is unsound.

    A silently corrupt root is the worst failure mode for this image: the
    bootloader and the kernel both come up clean, and only the shell is
    missing, which looks like an unrelated driver problem.
    """
    result = subprocess.run([e2fsck, "-fn", image], capture_output=True,
                            text=True)
    if result.returncode != 0:
        raise SystemExit(f"root filesystem check failed for {image}:\n"
                         f"{result.stdout}{result.stderr}")


def verify_root_payload(root_dir: str):
    """Refuse to build an image whose root cannot start a shell.

    osdata/ is intentionally not tracked by git, so a fresh clone has an empty
    or missing root directory. The resulting image boots all the way through
    the bootloader and the kernel and then sits there with no shell, which
    looks exactly like a storage-driver failure. Say what is actually wrong
    while the build is still running instead.
    """
    missing = []
    for required in ("bin/sh", "etc/init.sh", "bin/busybox"):
        path = os.path.join(root_dir, required)
        if not os.path.exists(path):
            missing.append(required)
    if not missing:
        return
    raise SystemExit(
        "refusing to build: the root directory "
        f"{root_dir!r} is missing {', '.join(missing)}.\n"
        "osdata/ is not part of the git checkout; populate it first, e.g.\n"
        "  mkdir -p osdata/bin osdata/etc\n"
        "  cp /path/to/busybox osdata/bin/busybox && ln -sf busybox osdata/bin/sh\n"
        "  printf '#!/bin/sh\\nexec /bin/sh\\n' > osdata/etc/init.sh\n"
        "then run make main.img again. Pass --allow-empty-root to build a\n"
        "rootless image on purpose (it will boot to the kernel and stop).")


def copy_tree(debugfs: str, image: str, root_dir: str):
    """Recursively copy root_dir into the ext4 image with debugfs."""
    def run(cmd):
        subprocess.run([debugfs, "-w", "-R", cmd, image], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    run("mkdir /bin")
    run("mkdir /etc")
    run("mkdir /dev")
    run("mkdir /proc")
    run("mkdir /sys")
    run("mkdir /tmp")
    run("mkdir /root")
    run("mkdir /var")
    run("mkdir /usr")
    run("mkdir /opt")
    run("mkdir /home")

    # write regular files, then fix modes/symlinks
    pending_modes = []
    for dirpath, dirnames, filenames in os.walk(root_dir):
        rel = os.path.relpath(dirpath, root_dir)
        base = "" if rel == "." else "/" + rel.replace(os.sep, "/")
        for name in dirnames:
            target = f"{base}/{name}"
            run(f"mkdir {target}")
            pending_modes.append((os.path.join(dirpath, name), target))
        for name in filenames:
            src = os.path.join(dirpath, name)
            if os.path.islink(src):
                target = f"{base}/{name}"
                dest = os.readlink(src)
                run(f"symlink {target} {dest}")
                continue
            target = f"{base}/{name}"
            run(f"write {src} {target}")
            pending_modes.append((src, target))

    # chmod/chown is a separate pass so directories copied above keep the mode
    def mode_spec(path: str) -> str:
        st = os.lstat(path)
        kind = 0o40000 if os.path.isdir(path) else 0o100000
        return f"{kind | (st.st_mode & 0o7777):o}"

    # debugfs parses set_inode_field values as decimal (or 0x-prefixed hex),
    # never as bare octal: "40755" silently became 0o117463 and left every
    # inode with an unknown file type. Emit a 0x prefix instead.
    for src, target in pending_modes:
        run(f"set_inode_field {target} mode 0x{int(mode_spec(src), 8):x}")
        uid = os.lstat(src).st_uid
        gid = os.lstat(src).st_gid
        if uid != 0:
            run(f"set_inode_field {target} uid {uid}")
        if gid != 0:
            run(f"set_inode_field {target} gid {gid}")


if __name__ == "__main__":
    sys.exit(main())
