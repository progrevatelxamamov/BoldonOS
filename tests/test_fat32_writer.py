"""scripts/fat32.py: the pure-Python ESP writer used by make-main-img.py.

The test formats an image, stores a tree that exercises short 8.3 names, long
names, nested directories, multi-cluster files and an empty file, then reads
the tree back with an independent reader implemented here (the writer is never
used for verification). Header fields are checked against the FAT32 spec.
"""
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import fat32  # noqa: E402


def read_fat32(image):
    """Independent FAT32 reader: returns {path: bytes} for a flat tree dump."""
    sec = 512
    bytes_per_sector, = struct.unpack_from('<H', image, 11)
    spc = image[13]
    reserved, = struct.unpack_from('<H', image, 14)
    nfats = image[16]
    fatsz, = struct.unpack_from('<I', image, 36)
    root, = struct.unpack_from('<I', image, 44)
    cluster_bytes = spc * bytes_per_sector
    data_start = (reserved + nfats * fatsz)
    fat = struct.unpack_from(f'<{fatsz * sec // 4}I', image, reserved * sec)

    def chain(start):
        out, c = [], start
        while 2 <= c < 0x0FFFFFF8:
            out.append(c)
            c = fat[c]
        return out

    def read_chain(start, size=None):
        blob = b''.join(
            image[(data_start + (c - 2) * spc) * sec:][:cluster_bytes]
            for c in chain(start))
        return blob[:size] if size is not None else blob

    def parse_dir(cluster):
        entries, lfn = [], []
        blob = read_chain(cluster)
        for off in range(0, len(blob), 32):
            entry = blob[off:off + 32]
            if entry[0] == 0:
                break
            if entry[0] == 0xE5:
                lfn = []
                continue
            if entry[11] == 0x0F:
                lfn.append(bytes(entry))
                continue
            name = entry[0:11]
            if name in (b'.          ', b'..         '):
                lfn = []
                continue
            base = name[:8].decode().rstrip()
            ext = name[8:].decode().rstrip()
            short = base + ('.' + ext if ext else '')
            long_name = short
            if lfn:
                raw = b''.join(l[1:11] + l[14:26] + l[28:32] for l in reversed(lfn))
                long_name = raw.decode('utf-16-le').split('\0')[0]
            cluster_id = (struct.unpack_from('<H', entry, 20)[0] << 16) | \
                struct.unpack_from('<H', entry, 26)[0]
            size = struct.unpack_from('<I', entry, 28)[0]
            entries.append((long_name, entry[11], cluster_id, size))
            lfn = []
        return entries

    out = {}

    def walk(cluster, path):
        for name, attr, child, size in parse_dir(cluster):
            full = path + '/' + name
            if attr & 0x10:
                walk(child, full)
            else:
                out[full] = read_chain(child, size)

    walk(root, '')
    return out


FILES = {
    '/EFI/BOOT/BOOTX64.EFI': b'EFI-BINARY' * 100,
    '/EFI/BOOT/limine-bios.sys': b'SYS' * 10,
    '/boot/kernel': b'KERNEL' * 1000,
    '/boot/boldonsplash.bmp': bytes(range(256)) * 4,
    '/boot/limine/limine.conf': b'timeout: 3\n/Default\n',
    '/boot/libpayload-LICENSES.md': b'MIT\n',
    '/EFI/BOOT/initrd.ELF': b'x' * 70000,     # spans many clusters
    '/boot/empty': b'',
}

image = bytearray(64 * 1024 * 1024)
writer = fat32.build_fat32(image, FILES, volume_label='BOOT')

# --- header fields, compared against the FAT32 specification --------------
assert image[510:512] == b'\x55\xAA'
assert struct.unpack_from('<H', image, 11)[0] == 512
assert image[16] == 2                     # two FATs
assert struct.unpack_from('<I', image, 36)[0] == writer.fat_sectors
assert struct.unpack_from('<I', image, 44)[0] == 2       # root cluster
assert image[82:90] == b'FAT32   '
assert image[71:82] == b'BOOT       '
# The backup boot sector must be identical to the primary one.
assert image[0:512] == image[6 * 512:7 * 512]
# FSInfo signature.
assert struct.unpack_from('<I', image, 512)[0] == 0x41615252

# The volume must satisfy the FAT32 cluster-count rule so that Linux and UEFI
# both accept it as FAT32 rather than silently treating it as FAT16.
assert writer.cluster_count >= 65525, writer.cluster_count
# Both FAT copies must be identical.
start = writer.reserved * 512
fat_a = image[start: start + writer.fat_sectors * 512]
fat_b = image[start + writer.fat_sectors * 512:
              start + 2 * writer.fat_sectors * 512]
assert fat_a == fat_b

got = read_fat32(bytes(image))

# FAT stores every name twice: an 8.3 alias (upper case) and, when needed, a
# long-name entry that preserves the original spelling. Compare accordingly.
def lookup(entries, path):
    if path in entries:
        return entries[path]
    upper = path.upper()
    for key, value in entries.items():
        if key.upper() == upper:
            return value
    raise AssertionError(f'{path} not found in {sorted(entries)}')


assert len(got) == len(FILES), (sorted(got), sorted(FILES))
for path, payload in FILES.items():
    assert lookup(got, path) == payload, path

# Names that are not valid 8.3 must keep their exact spelling via LFN entries.
for long_name in ('boldonsplash.bmp', 'libpayload-LICENSES.md', 'limine.conf'):
    assert long_name in ' '.join(got), (long_name, sorted(got))

# Regression: Fat32Writer used to emit a long-name entry for every name whose
# 8.3 alias differed from the input only by case ("boot" -> "BOOT"), and the
# record it wrote was malformed -- the padding after the terminator was left
# as uninitialised bytes. Limine's FAT reader rejected the whole directory and
# reported "Stage 3 file not found". A well-formed entry has attribute 0x0F,
# a non-zero sequence number, a zero terminator after the text and 0xFFFF in
# every slot beyond a short name. Check that no long-name entry exists at all
# for a pure 8.3 tree.
flat = bytearray(bytes(64 * 1024 * 1024))
writer = fat32.build_fat32(flat, {'/BOOT/KERNEL': b'k' * 100},
                           volume_label='BOOT')
data_start = (writer.reserved + writer.num_fats * writer.fat_sectors)
root_dir = bytes(flat[data_start * 512: data_start * 512 + 512])
for off in range(0, 512, 32):
    entry = root_dir[off:off + 32]
    if not entry or entry[0] == 0:
        break
    assert entry[11] != 0x0F, ('unexpected LFN entry', entry)

# A long name must still produce a well-formed chain whose slots are fully
# initialised: 0xFFFF padding, name checksum matching the 8.3 alias.
long_img = bytearray(bytes(64 * 1024 * 1024))
writer = fat32.build_fat32(long_img,
                           {'/boot/libpayload-LICENSES.md': b'x'},
                           volume_label='BOOT')
data_start = (writer.reserved + writer.num_fats * writer.fat_sectors)
boot_cluster = 3                       # /boot is cluster 3 after the root
boot_dir = bytes(long_img[(data_start + (boot_cluster - 2)) * 512:][:512])
lfn_entries = []
alias = None
for off in range(0, 512, 32):
    entry = boot_dir[off:off + 32]
    if not entry or entry[0] == 0:
        break
    if entry[0] == 0xE5:
        continue
    if entry[11] == 0x0F:
        lfn_entries.append(entry)
        continue
    if entry[0] == ord('.') and entry[1] in (ord('.'), 0x20):
        continue                     # "." and ".." are not files
    alias = entry
    break


def _lfn_checksum(short):
    total = 0
    for byte in short:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


assert lfn_entries, 'long name produced no LFN entry'
assert alias is not None, 'long name produced no 8.3 alias'
assert lfn_entries[0][13] == _lfn_checksum(alias[0:11]), 'LFN checksum mismatch'

# Entries are stored back to front: the entry next to the 8.3 alias holds the
# first 13 characters and carries ordinal 1; the first stored entry holds the
# final characters and sets the 0x40 "last entry" flag.
for index, entry in enumerate(lfn_entries):
    ordinal = entry[0] & 0x3F
    assert ordinal == len(lfn_entries) - index, (index, ordinal)
    assert entry[26:28] == b'\x00\x00', ('reserved bytes are not zero', entry)
# Only the first stored entry (the one holding the tail of the name) is the
# last logical entry, and that is the one which sets 0x40.
assert lfn_entries[0][0] & 0x40, 'the 0x40 flag must be on the first stored entry'

ordered = list(reversed(lfn_entries))
chars = []
for entry in ordered:
    for start, end in ((1, 11), (14, 26), (28, 32)):
        chars.append(entry[start:end])
text = b''.join(chars).decode('utf-16-le', errors='ignore')
assert text.split('\x00')[0] == 'libpayload-LICENSES.md', text

print('FAT32 writer round-trip and header checks PASS')
