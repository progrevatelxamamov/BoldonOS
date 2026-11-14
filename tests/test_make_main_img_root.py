"""scripts/make-main-img.py: the ext4 root it builds must be a valid filesystem.

Two bugs this guards against, both of which produced an image that booted
cleanly through the bootloader and the kernel and then simply had no shell:

  * `debugfs set_inode_field <path> mode <value>` parses its value as decimal
    (or 0x-prefixed hex), never as bare octal. Writing "40755" left every inode
    with mode 0o117463 and an unknown file type, so e2fsck reported the whole
    root as corrupt.
  * The builder never checked the filesystem it produced, so that corruption
    only surfaced as a mysterious ENOENT from execve on real hardware.

The test builds a real ext4 image the way main() does, copies a small tree into
it, and then asserts, through an independent reader (debugfs + e2fsck), that the
modes are the ones the source files have.
"""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

# The script name contains a hyphen, so it cannot be imported by name.
import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    'make_main_img', ROOT / 'scripts' / 'make-main-img.py')
make_main_img = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(make_main_img)

MKE2FS = shutil.which('mke2fs') or '/sbin/mke2fs'
DEBUGFS = shutil.which('debugfs') or '/sbin/debugfs'
E2FSCK = shutil.which('e2fsck') or '/sbin/e2fsck'

work = Path(tempfile.mkdtemp(prefix='boldon-root-test-'))
try:
    tree = work / 'osdata'
    (tree / 'bin').mkdir(parents=True)
    (tree / 'etc').mkdir()
    busybox = tree / 'bin' / 'busybox'
    busybox.write_bytes(b'\x7fELF' + b'\x00' * 64)
    busybox.chmod(0o755)
    (tree / 'bin' / 'sh').symlink_to('busybox')
    (tree / 'etc' / 'init.sh').write_text('#!/bin/sh\necho hello\n')

    image = work / 'root.img'
    subprocess.run([MKE2FS, '-q', '-F', '-t', 'ext4', '-L', 'BOLDON_ROOT',
                    str(image), '16384'], check=True)

    make_main_img.copy_tree(DEBUGFS, str(image), str(tree))
    make_main_img.verify_root_image(E2FSCK, str(image))

    def stat(path):
        out = subprocess.run([DEBUGFS, '-R', f'stat {path}', str(image)],
                             capture_output=True, text=True, check=True).stdout
        return out

    busybox = stat('/bin/busybox')
    assert 'Type: regular' in busybox, busybox
    assert 'Mode:  0755' in busybox, busybox

    bin_dir = stat('/bin')
    assert 'Type: directory' in bin_dir, bin_dir
    assert 'Mode:  0755' in bin_dir, bin_dir

    # The symlink must be a symlink, not a copy of the target.
    link = stat('/bin/sh')
    assert 'Type: symlink' in link or 'Fast link' in link, link

    script = stat('/etc/init.sh')
    assert 'Type: regular' in script, script
    assert 'Mode:  0644' in script, script

    # A deliberately corrupt image must be rejected, so the check cannot pass
    # vacuously. Reproduce the exact bug: debugfs read the bare octal string
    # "040755" as decimal 40755 == 0o117463, which is the unknown file type
    # that made e2fsck complain. Writing 40755 here restores that state.
    subprocess.run([DEBUGFS, '-w', '-R', 'set_inode_field /bin mode 40755',
                    str(image)], capture_output=True, check=True)
    try:
        make_main_img.verify_root_image(E2FSCK, str(image))
    except SystemExit:
        pass
    else:
        raise AssertionError('verify_root_image accepted a corrupt filesystem')

    print('make-main-img root filesystem checks PASS')
finally:
    shutil.rmtree(work, ignore_errors=True)
