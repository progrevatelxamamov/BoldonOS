"""scripts/make-osdata.py: build the root filesystem directory main.img needs.

osdata/ is git-ignored because it holds a binary, so a fresh clone has nothing
to put in the ext4 root. This script produces the minimum that starts a shell:
a static busybox, /bin/sh as a symlink to it, and /etc/init.sh.

The static-binary check matters: a dynamically linked busybox from the host
would be copied into the image and then fail at execve with ENOENT, because the
image has no libc or dynamic loader in it.
"""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location(
    'make_osdata', ROOT / 'scripts' / 'make-osdata.py')
make_osdata = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(make_osdata)

# A tiny well-formed ELF64 header is enough for the copy path; the scripts do
# not execute what they stage.
STATIC_ELF = (
    b'\x7fELF\x02\x01\x01\x00' + b'\x00' * 8 +
    b'\x02\x00\x3e\x00' + b'\x01\x00\x00\x00' + b'\x00' * 40 +
    b'\x00' * 64
)
DYNAMIC_ELF = STATIC_ELF + b'\x03\x00\x00\x00/lib64/ld-linux-x86-64.so.2\x00'

work = Path(tempfile.mkdtemp(prefix='boldon-osdata-test-'))
try:
    fake = work / 'busybox'
    fake.write_bytes(STATIC_ELF)
    fake.chmod(0o755)

    assert make_osdata.is_static_elf(str(fake))

    dynamic = work / 'busybox-dynamic'
    dynamic.write_bytes(DYNAMIC_ELF)
    dynamic.chmod(0o755)
    assert not make_osdata.is_static_elf(str(dynamic)), \
        'a dynamically linked binary must not be staged as /bin/busybox'

    not_elf = work / 'notelf'
    not_elf.write_bytes(b'#!/bin/sh\necho hi\n')
    assert not make_osdata.is_static_elf(str(not_elf))

    out = work / 'osdata'
    result = subprocess.run(
        [sys.executable, str(ROOT / 'scripts' / 'make-osdata.py'),
         '--out', str(out), '--busybox', str(fake)],
        capture_output=True, text=True)
    assert result.returncode == 0, (result.stdout, result.stderr)

    staged_busybox = out / 'bin' / 'busybox'
    assert staged_busybox.is_file(), sorted(os.listdir(out / 'bin'))
    assert staged_busybox.read_bytes() == STATIC_ELF
    assert staged_busybox.stat().st_mode & 0o111, 'busybox is not executable'

    link = out / 'bin' / 'sh'
    assert link.is_symlink() and os.readlink(link) == 'busybox', link

    init = out / 'etc' / 'init.sh'
    body = init.read_text()
    assert body.startswith('#!/bin/sh'), body
    assert 'exec /bin/sh' in body, body
    assert init.stat().st_mode & 0o111, 'init.sh is not executable'

    # The staged tree must satisfy the check the image builder applies.
    sys.path.insert(0, str(ROOT / 'scripts'))
    _img_spec = importlib.util.spec_from_file_location(
        'make_main_img', ROOT / 'scripts' / 'make-main-img.py')
    make_main_img = importlib.util.module_from_spec(_img_spec)
    _img_spec.loader.exec_module(make_main_img)
    make_main_img.verify_root_payload(str(out))

    # A missing busybox must be reported, not ignored.
    result = subprocess.run(
        [sys.executable, str(ROOT / 'scripts' / 'make-osdata.py'),
         '--out', str(work / 'osdata2'), '--busybox', str(work / 'nope')],
        capture_output=True, text=True)
    assert result.returncode != 0, 'a missing --busybox path was accepted'
    assert 'not a file' in (result.stdout + result.stderr)

    print('make-osdata staging checks PASS')
finally:
    shutil.rmtree(work, ignore_errors=True)
