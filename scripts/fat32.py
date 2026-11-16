"""Minimal FAT32 writer: format an image and populate it from a file map.

Only what a UEFI firmware needs is implemented: one FAT32 volume, 512-byte
sectors, configurable cluster size, the standard boot sector + FSInfo pair and
long file names written the way Windows and mkfs.vfat write them. There is no
FAT12/16 support and no journal.

The module is dependency free so ``make-main-img.py`` can build a bootable disk
on a machine without mtools, dosfstools or sgdisk.
"""
import struct

SECTOR = 512
DIRENT = 32
EOC = 0x0FFFFFFF
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LFN = 0x0F

# Characters Windows refuses in short names.
_SHORT_BAD = set('"*+,/:;<=>?[\\]| ')
_VOLUME_SERIAL = 0x42424242


class Fat32Error(RuntimeError):
    pass


def _short_name_for(name, taken):
    """Return (short11, needs_lfn) for `name`, registering the alias in `taken`.

    `needs_lfn` is true when the 8.3 alias is not a case-insensitive match of
    the requested name, which is exactly when Windows stores a long name.
    """
    upper = name.upper()
    dot = upper.rfind('.')
    if dot <= 0:
        base, ext = upper, ''
    else:
        base, ext = upper[:dot], upper[dot + 1:]

    def clean(text):
        return ''.join(c for c in text if c not in _SHORT_BAD)

    base_clean = clean(base)
    ext_clean = clean(ext)
    exact = (base_clean == base and ext_clean == ext and
             1 <= len(base) <= 8 and len(ext) <= 3)
    if exact:
        pad = (base_clean.ljust(8) + ext_clean.ljust(3)).encode('ascii')
        if pad not in taken:
            taken.add(pad)
            return pad, False

    stem = (base_clean or 'FILE').ljust(8)[:8]
    ext_part = ext_clean[:3]
    for i in range(1, 1_000_000):
        tail = f'~{i}'
        pad = ((stem[:8 - len(tail)] + tail).ljust(8) +
               ext_part.ljust(3)).encode('ascii')
        if pad not in taken:
            taken.add(pad)
            return pad, True
    raise Fat32Error(f'no free short name for {name}')


def _lfn_checksum(short11):
    total = 0
    for byte in short11:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def _lfn_entries(long_name, short11):
    """Long-file-name entries for `long_name`, in on-disk (descending) order."""
    chars = long_name
    if len(chars) % 13:
        chars += '\0' * (13 - len(chars) % 13)
    units = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    checksum = _lfn_checksum(short11)
    out = []
    for idx in range(len(units) - 1, -1, -1):
        seq = idx + 1
        if idx == len(units) - 1:
            seq |= 0x40
        entry = bytearray(DIRENT)
        entry[0] = seq
        entry[11] = ATTR_LFN
        entry[13] = checksum
        enc = units[idx].ljust(13, '\uffff').encode('utf-16-le')
        entry[1:11] = enc[0:10]
        entry[14:26] = enc[10:22]
        entry[28:32] = enc[22:26]
        out.append(bytes(entry))
    return out


def _dirent(short11, attr, cluster, size):
    entry = bytearray(DIRENT)
    entry[0:11] = short11
    entry[11] = attr
    entry[20:22] = struct.pack('<H', (cluster >> 16) & 0xFFFF)
    entry[26:28] = struct.pack('<H', cluster & 0xFFFF)
    entry[28:32] = struct.pack('<I', size)
    return bytes(entry)


class Fat32Writer:
    """Builds a FAT32 filesystem inside a caller-owned bytearray."""

    def __init__(self, data, volume_label='BOOT', reserved_sectors=8,
                 num_fats=2, sectors_per_cluster=0):
        if len(data) % SECTOR:
            raise Fat32Error('image size must be a multiple of 512 bytes')
        self.data = data
        self.label = volume_label
        self.reserved = reserved_sectors
        self.num_fats = num_fats
        if sectors_per_cluster:
            self.spc = sectors_per_cluster
        else:
            # Pick the smallest power-of-two cluster size that keeps the FAT32
            # minimum of 65525 clusters; small ESPs need 4 KiB clusters.
            self.spc = 0
            for candidate in (1, 2, 4, 8, 16, 32, 64, 128):
                clusters = (len(data) // SECTOR - reserved_sectors -
                            num_fats * 8) // candidate
                if clusters >= 65525:
                    self.spc = candidate
                    break
            if not self.spc:
                raise Fat32Error('image too small for any FAT32 cluster size')
        self.cluster_bytes = self.spc * SECTOR
        self.total_sectors = len(data) // SECTOR
        self._compute_layout()
        self.fat = [0] * (self.cluster_count + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = 0x0FFFFFFF
        self.next_free = 2
        self.root_cluster = 2

    def _compute_layout(self):
        """Microsoft's FAT32 size formula (fatgen103), then verify by count.

        Iterating ceil() on the cluster count oscillates, so the size is
        computed in closed form and only then sanity checked.
        """
        disk = self.total_sectors
        tmp1 = disk - (self.reserved + 0)      # FAT32 has no fixed root dir
        tmp2 = (256 * self.spc) + self.num_fats
        fat_sectors = (tmp1 + (tmp2 // 2) - 1) // (tmp2 // 2)
        clusters = (disk - self.reserved - self.num_fats * fat_sectors) // self.spc
        # A slightly bigger FAT can never be needed, but one sector more keeps
        # us above the boundary when the division truncates.
        while True:
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fat_sectors:
                break
            fat_sectors = need
            clusters = (disk - self.reserved - self.num_fats * fat_sectors) // self.spc
        if clusters < 65525:
            raise Fat32Error(f'only {clusters} clusters: FAT32 needs 65525+')
        self.fat_sectors = fat_sectors
        self.cluster_count = clusters
        self.data_start = self.reserved + self.num_fats * fat_sectors

    # -- raw access ------------------------------------------------------
    def _put(self, offset, payload):
        self.data[offset:offset + len(payload)] = payload

    def _cluster_offset(self, cluster):
        if not 2 <= cluster < self.cluster_count + 2:
            raise Fat32Error(f'cluster {cluster} out of range')
        return (self.data_start + (cluster - 2) * self.spc) * SECTOR

    def _alloc(self):
        while self.next_free < self.cluster_count + 2 and self.fat[self.next_free]:
            self.next_free += 1
        if self.next_free >= self.cluster_count + 2:
            raise Fat32Error('filesystem full')
        cluster = self.next_free
        self.next_free += 1
        self.fat[cluster] = EOC
        return cluster

    def _chain(self, size):
        count = max(1, (size + self.cluster_bytes - 1) // self.cluster_bytes)
        chain = [self._alloc() for _ in range(count)]
        for first, second in zip(chain, chain[1:]):
            self.fat[first] = second
        self.fat[chain[-1]] = EOC
        return chain

    def _write_chain(self, chain, payload):
        for index, cluster in enumerate(chain):
            chunk = payload[index * self.cluster_bytes:
                            (index + 1) * self.cluster_bytes]
            self._put(self._cluster_offset(cluster),
                      bytes(chunk).ljust(self.cluster_bytes, b'\0'))

    # -- formatting ------------------------------------------------------
    def format(self):
        self.data[:] = b'\0' * len(self.data)
        boot = bytearray(SECTOR)
        boot[0:3] = b'\xEB\x58\x90'
        boot[3:11] = b'MSWIN4.1'
        struct.pack_into('<H', boot, 11, SECTOR)
        boot[13] = self.spc
        struct.pack_into('<H', boot, 14, self.reserved)
        boot[16] = self.num_fats
        struct.pack_into('<H', boot, 22, 0)         # reserved, FAT32 = 0
        boot[21] = 0xF8
        struct.pack_into('<H', boot, 24, 63)
        struct.pack_into('<H', boot, 26, 255)
        struct.pack_into('<I', boot, 28, 0)
        struct.pack_into('<I', boot, 32, self.total_sectors)
        struct.pack_into('<I', boot, 36, self.fat_sectors)
        struct.pack_into('<H', boot, 40, 0)         # no FAT mirroring
        struct.pack_into('<H', boot, 42, 0)         # version 0.0
        struct.pack_into('<I', boot, 44, self.root_cluster)
        struct.pack_into('<H', boot, 48, 1)         # FSInfo at sector 1
        struct.pack_into('<H', boot, 50, 6)         # backup boot sector
        # (the backup lives at sector 6, the FSInfo backup at sector 7)
        boot[64] = 0x80                             # drive number
        boot[66] = 0x29
        struct.pack_into('<I', boot, 67, _VOLUME_SERIAL)
        boot[71:82] = self.label.upper().ljust(11)[:11].encode('ascii', 'replace')
        boot[82:90] = b'FAT32   '
        boot[510:512] = b'\x55\xAA'
        self._put(0, bytes(boot))
        self._put(6 * SECTOR, bytes(boot))

        fsinfo = bytearray(SECTOR)
        struct.pack_into('<I', fsinfo, 0, 0x41615252)
        struct.pack_into('<I', fsinfo, 484, 0x61417272)
        struct.pack_into('<I', fsinfo, 488, 0xFFFFFFFF)
        struct.pack_into('<I', fsinfo, 492, 0xFFFFFFFF)
        struct.pack_into('<I', fsinfo, 508, 0xAA550000)
        self._put(SECTOR, bytes(fsinfo))
        self._put(7 * SECTOR, bytes(fsinfo))

    def _flush_fat(self):
        entries = b''.join(struct.pack('<I', v & 0x0FFFFFFF) for v in self.fat)
        pad = self.fat_sectors * SECTOR - len(entries)
        entries += b'\0' * max(0, pad)
        for copy in range(self.num_fats):
            self._put((self.reserved + copy * self.fat_sectors) * SECTOR, entries)

    # -- tree writing ----------------------------------------------------
    def build(self, files):
        """Format the volume and place `files` (path -> bytes) on it."""
        self.format()
        tree = {'children': {}}
        for path, payload in sorted(files.items()):
            parts = [p for p in path.split('/') if p]
            if not parts:
                raise Fat32Error(f'empty path in file map: {path!r}')
            node = tree
            for part in parts[:-1]:
                child = node['children'].setdefault(part, {'children': {}})
                if 'data' in child:
                    raise Fat32Error(f'{path}: {part} is also a file')
                node = child
            leaf = node['children'].setdefault(parts[-1], {})
            if leaf.get('children'):
                raise Fat32Error(f'{path}: name already used by a directory')
            leaf['data'] = bytes(payload)

        # Every directory needs its first cluster before its parent's entry is
        # written, and a ".." entry pointing at its parent.
        self.fat[self.root_cluster] = EOC
        self.next_free = self.root_cluster + 1
        self._reserve_dirs(tree)

        self._write_dir(self.root_cluster, tree['children'], parent=None)
        self._flush_fat()
        return self

    def _reserve_dirs(self, node):
        for child in node['children'].values():
            if 'data' not in child:
                child['cluster'] = self._alloc()
                self._reserve_dirs(child)

    def _write_dir(self, cluster, children, parent):
        skipped = {'.', '..'}
        taken = set()
        dirents = []
        if parent is not None:
            dirents.append(_dirent(b'.          ', ATTR_DIRECTORY, cluster, 0))
            dirents.append(_dirent(b'..         ', ATTR_DIRECTORY, parent, 0))
        for name in sorted(children):
            if name in skipped:
                continue
            child = children[name]
            short11, needs_lfn = _short_name_for(name, taken)
            if needs_lfn:
                dirents.extend(_lfn_entries(name, short11))
            if 'data' not in child:
                dirents.append(_dirent(short11, ATTR_DIRECTORY,
                                       child['cluster'], 0))
            else:
                payload = child['data']
                chain = self._chain(len(payload)) if payload else []
                if chain:
                    self._write_chain(chain, payload)
                dirents.append(_dirent(short11, ATTR_ARCHIVE,
                                       chain[0] if chain else 0, len(payload)))

        payload = b''.join(dirents)
        chain = [cluster]
        needed = max(1, (len(payload) + self.cluster_bytes - 1) //
                     self.cluster_bytes)
        while len(chain) < needed:
            chain.append(self._alloc())
        for first, second in zip(chain, chain[1:]):
            self.fat[first] = second
        self.fat[chain[-1]] = EOC
        self._write_chain(chain, payload)

        for name in sorted(children):
            if name in skipped:
                continue
            child = children[name]
            if 'data' not in child:
                self._write_dir(child['cluster'], child['children'], cluster)


def build_fat32(image_bytes, files, volume_label='BOOT',
                sectors_per_cluster=0):
    """Format `image_bytes` (bytearray) as FAT32 and store `files` on it."""
    return Fat32Writer(image_bytes, volume_label=volume_label,
                      sectors_per_cluster=sectors_per_cluster).build(files)
