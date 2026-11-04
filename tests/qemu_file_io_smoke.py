#!/usr/bin/env python3
"""Boot a disposable UEFI+USB ext4 guest twice and verify real syscall I/O.

Requires QEMU, OVMF, mke2fs, and a C compiler. Filesystem and EFI boot assets
are created in a temporary directory OUTSIDE the repository. /bin/sh is a
standalone syscall probe, not a real userland or KDE Plasma installation.

Example:
  python3 tests/qemu_file_io_smoke.py --qemu /path/to/qemu-system-x86_64 \
    --firmware-code /path/to/edk2-x86_64-code.fd \
    --firmware-vars /path/to/edk2-i386-vars.fd
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import uuid
import zlib

REPO = Path(__file__).resolve().parents[1]


def make_disk(path, tree):
    sectors = 80 * 1024 * 1024 // 512
    start, end = 2048, sectors - 2048
    with path.open('wb') as out:
        out.truncate(sectors * 512)
    parts = bytearray(128 * 128)
    parts[0:16] = uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4').bytes_le
    parts[16:32] = uuid.uuid4().bytes_le
    struct.pack_into('<QQ', parts, 32, start, end)
    label = 'BOLDON_ROOT'.encode('utf-16le')
    parts[56:56 + len(label)] = label
    hdr = bytearray(512)
    hdr[:8] = b'EFI PART'
    struct.pack_into('<IIIIQQQQ16sQIII', hdr, 8, 0x10000, 92, 0, 0,
                     1, sectors - 1, 34, sectors - 34, uuid.uuid4().bytes_le,
                     2, 128, 128, zlib.crc32(parts))
    struct.pack_into('<I', hdr, 16, zlib.crc32(hdr[:92]))
    mbr = bytearray(512)
    mbr[446 + 4] = 0xee
    struct.pack_into('<II', mbr, 446 + 8, 1, sectors - 1)
    mbr[510:] = b'\x55\xaa'
    with path.open('r+b') as out:
        out.write(mbr)
        out.write(hdr)
        out.write(parts)
    subprocess.run(['/usr/sbin/mke2fs', '-q', '-F', '-t', 'ext4', '-b', '4096',
                    '-L', 'BOLDON_ROOT', '-O', '^64bit,^metadata_csum',
                    '-E', f'offset={start * 512}', '-d', str(tree), str(path),
                    str((end - start + 1) * 512 // 4096)], check=True)


def boot(args, work, turn):
    serial = work / f'boot-{turn}.log'
    env = os.environ.copy()
    if args.library_path:
        env['LD_LIBRARY_PATH'] = args.library_path + (':' + env['LD_LIBRARY_PATH']
                                                     if env.get('LD_LIBRARY_PATH') else '')
    qemu = [args.qemu]
    if args.loader:
        qemu.insert(0, args.loader)
    cmd = qemu + ['-L', args.qemu_data, '-M', 'q35', '-m', '1024', '-smp', '2',
                  '-display', 'none', '-serial', f'file:{serial}', '-no-reboot',
                  '-drive', f'if=pflash,unit=0,format=raw,readonly=on,file={args.firmware_code}',
                  '-drive', f'if=pflash,unit=1,format=raw,file={work / "vars.fd"}',
                  '-drive', f'file=fat:rw:{work / "esp"},if=none,id=esp,format=raw',
                  '-device', 'ide-hd,drive=esp,bootindex=1',
                  '-device', 'qemu-xhci,id=xhci',
                  '-drive', f'file={work / "root.img"},if=none,id=stick,format=raw',
                  '-device', 'usb-storage,drive=stick,bus=xhci.0']
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    try:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            text = serial.read_text(errors='replace') if serial.exists() else ''
            if 'PROBE FAIL' in text or 'Halted.' in text or 'User panic!' in text:
                raise AssertionError('Guest failed:\n' + text[-3500:])
            if 'PROBE ALL PASS' in text:
                for marker in ('PROBE PASS write-close-reopen-read bytes',
                               'PROBE PASS dup preserves stdin',
                               'PROBE PASS redirected fd1 write/read',
                               'PROBE PASS 32 repeated truncate/write/read cycles',
                               'PROBE PASS 8 fork/parent-close/child-write cycles'):
                    assert marker in text, f'Missing marker: {marker}\n{text[-3500:]}'
                expected = 'PROBE FIRST BOOT' if turn == 1 else 'PROBE REBOOT PASS persisted bytes'
                assert expected in text, f'Missing {expected}\n{text[-3500:]}'
                print(f'Boot {turn}: {expected}; all write/read checks PASS')
                return
            if proc.poll() is not None:
                raise RuntimeError(f'QEMU exited {proc.returncode}: {proc.stderr.read().decode(errors="replace")}')
            time.sleep(0.2)
        raise TimeoutError(f'Guest did not finish boot {turn}:\n{text[-3500:]}')
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--qemu', default='qemu-system-x86_64')
    ap.add_argument('--qemu-data', default='/usr/share/qemu')
    ap.add_argument('--firmware-code', required=True)
    ap.add_argument('--firmware-vars', required=True)
    ap.add_argument('--loader', help='Optional musl loader for a portable QEMU')
    ap.add_argument('--library-path', help='Optional shared libraries for portable QEMU')
    ap.add_argument('--timeout', type=int, default=75)
    ap.add_argument('--keep-image', type=Path, help='Keep image/logs outside the repository')
    args = ap.parse_args()
    if args.keep_image:
        dst = args.keep_image.resolve()
        if dst == REPO or REPO in dst.parents:
            ap.error('--keep-image must be outside the repository')
        dst.mkdir(parents=True, exist_ok=True)
        run(dst, args)
    else:
        with tempfile.TemporaryDirectory(prefix='boldon-guest-io-') as tmp:
            run(Path(tmp), args)


def run(work, args):
    root = work / 'rootfs'
    (root / 'bin').mkdir(parents=True, exist_ok=True)
    (root / 'etc').mkdir(exist_ok=True)
    probe = work / 'probe'
    subprocess.run(['cc', '-nostdlib', '-static', '-no-pie', '-ffreestanding',
                    '-fno-builtin', '-fno-stack-protector', '-mno-sse', '-mno-mmx',
                    '-O2', '-Wl,-e,_start', str(REPO / 'tests/guest_file_io_probe.c'),
                    '-o', str(probe)], check=True)
    shutil.copy2(probe, root / 'bin/sh')
    (root / 'etc/init.sh').write_text('# QEMU syscall probe; /bin/sh is not a real shell\n')
    make_disk(work / 'root.img', root)
    esp = work / 'esp'
    (esp / 'EFI/BOOT').mkdir(parents=True, exist_ok=True)
    (esp / 'boot/limine').mkdir(parents=True, exist_ok=True)
    shutil.copy2(REPO / 'limine-binary/BOOTX64.EFI', esp / 'EFI/BOOT/BOOTX64.EFI')
    shutil.copy2(REPO / 'kernel/bin/kernel', esp / 'boot/kernel')
    shutil.copy2(REPO / 'boldonsplash.bmp', esp / 'boot/boldonsplash.bmp')
    shutil.copy2(REPO / 'limine.conf', esp / 'boot/limine/limine.conf')
    shutil.copy2(args.firmware_vars, work / 'vars.fd')
    boot(args, work, 1)
    boot(args, work, 2)
    print('QEMU ext4 guest: persistence across cold boot PASS')


if __name__ == '__main__':
    main()
