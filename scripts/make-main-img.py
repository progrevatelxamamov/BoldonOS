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
    """GPT stores the first three fields little endian."""
    b = u.bytes
    return (b[3::-1]) + (b[5:3:-1]) + (b[7:5:-1]) + b[8:]


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="main.img")
    ap.add_argument("--splash", default="boldonsplash.bmp")
    ap.add_argument("--kernel", default="kernel/bin/kernel")
    ap.add_argument("--licenses", default="kernel/third_party/libpayload/LICENSES.md")
    ap.add_argument("--root", default="osdata", help="directory copied into the ext4 root")
    ap.add_argument("--size-mib", type=int, default=512)
    ap.add_argument("--ufei", default="limine-binary/BOOTX64.EFI")
    ap.add_argument("--esp-mib", type=int, default=128)
    ap.add_argument("--mkfs", default="/sbin/mke2fs")
    ap.add_argument("--debugfs", default="/sbin/debugfs")
    ap.add_argument("--limine", default="limine-binary/limine")
    args = ap.parse_args()

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

    # FAT32 ESP
    esp_offset = esp_first * SECTOR
    esp_size_kib = (esp_last - esp_first + 1) * SECTOR // 1024
    subprocess.run(["/tmp/work/busybox.bin", "mkfs.vfat", "-F", "32", "-n", "BOOT",
                    "-i", "42424242", "-C", "/tmp/work/esp.img", str(esp_size_kib)],
                   check=True, stdout=subprocess.DEVNULL)
    # populate the ESP with the kernel, splash and bootloader
    script = []
    script.append("mmd -i /tmp/work/esp.img ::/EFI ::/EFI/BOOT ::/boot ::/boot/limine")
    script.append(f"mcopy -i /tmp/work/esp.img {args.kernel} ::/boot/kernel")
    script.append(f"mcopy -i /tmp/work/esp.img {args.splash} ::/boot/boldonsplash.bmp")
    if os.path.exists(args.licenses):
        script.append(f"mcopy -i /tmp/work/esp.img {args.licenses} ::/boot/libpayload-LICENSES.md")
    script.append(f"mcopy -i /tmp/work/esp.img limine.conf ::/boot/limine/limine.conf")
    for f in ("limine-binary/limine-bios.sys", args.ufei):
        script.append(f"mcopy -i /tmp/work/esp.img {f} ::/EFI/BOOT/")
    with open("/tmp/work/esp.mtools", "w") as f:
        f.write("\n".join(script) + "\n")
    subprocess.run(["/tmp/work/busybox.bin", "sh", "/tmp/work/esp.mtools"], check=True,
                   stdout=subprocess.DEVNULL)

    # copy the ESP image into the GPT partition
    with open("/tmp/work/esp.img", "rb") as src, open(args.out, "r+b") as dst:
        dst.seek(esp_offset)
        while True:
            chunk = src.read(MIB)
            if not chunk:
                break
            dst.write(chunk)

    # ext4 root filesystem
    root_img = "/tmp/work/root.img"
    root_kib = (root_last - root_first + 1) * SECTOR // 1024
    subprocess.run([args.mkfs, "-q", "-F", "-t", "ext4", "-L", "BOLDON_ROOT",
                    "-U", "00000000-0000-4000-8000-00000000B01D",
                    root_img, str(root_kib)], check=True)
    copy_tree(args.debugfs, root_img, args.root)
    with open(root_img, "rb") as src, open(args.out, "r+b") as dst:
        dst.seek(root_first * SECTOR)
        while True:
            chunk = src.read(MIB)
            if not chunk:
                break
            dst.write(chunk)

    # BIOS boot: limine's first stage lives in the BIOSBOOT partition
    subprocess.run([args.limine, "bios-install", args.out], check=True,
                   stdout=subprocess.STDOUT)
    print(f"wrote {args.out}: {args.size_mib} MiB, root {root_kib // 1024} MiB")


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

    for src, target in pending_modes:
        run(f"set_inode_field {target} mode {mode_spec(src)}")
        uid = os.lstat(src).st_uid
        gid = os.lstat(src).st_gid
        if uid != 0:
            run(f"set_inode_field {target} uid {uid}")
        if gid != 0:
            run(f"set_inode_field {target} gid {gid}")


if __name__ == "__main__":
    sys.exit(main())
