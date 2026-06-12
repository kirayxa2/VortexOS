#!/usr/bin/env python3
"""
add_vortexfs_file.py — добавляет файл (или директорию) в VortexFS образ диска.

Понимает v1/v2 (блок 512, инод 64 байта) и v3 (блок 4096, инод 128 байт,
triple indirect) — параметры берутся из суперблока образа.

Поддерживает подкаталоги:
    python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vwm bin/vwm
        → создаст /bin (если нет) и положит файл vwm

Режимы:
    add_vortexfs_file.py <img> <source_file|text> <dest_path>   — записать файл
    add_vortexfs_file.py <img> --mkdir <dir_path>               — создать каталог

Block 0 is reserved as sentinel. Valid blocks start at 1.
Должно 1:1 совпадать с kernel/fs/vortexfs.{h,c}.
"""

import struct
import sys
import os

SECTOR       = 512
VTXFS_MAGIC  = 0x56545846   # "VTXF"
NAME_MAX     = 59
DIRENT_SIZE  = 64
MAX_DIRECT   = 10

# VFS types (must match vfs.h)
VFS_FILE = 0x01
VFS_DIR  = 0x02


class VortexFSImage:
    """Read/write access to a VortexFS disk image (v1/v2/v3)."""

    def __init__(self, path):
        self.f = open(path, 'r+b')
        self.f.seek(0)
        raw_sb = self.f.read(SECTOR)
        self._parse_superblock(raw_sb)

    def _parse_superblock(self, raw):
        (self.magic, self.version, self.total_blocks, self.total_inodes,
         self.free_blocks, self.free_inodes,
         self.ibm_start, self.bbm_start, self.itb_start, self.dat_start,
         self.root_inode, self.jrn_start, self.jrn_sectors,
         self.block_size) = struct.unpack_from('<' + 'I' * 14, raw, 0)
        if self.magic != VTXFS_MAGIC or self.version not in (1, 2, 3):
            raise ValueError(f"Not a valid VortexFS image (magic=0x{self.magic:08X})")
        if self.version < 3:
            self.block_size = SECTOR
            self.inode_size = 64
        else:
            if self.block_size != 4096:
                raise ValueError(f"v3 image with bad block_size={self.block_size}")
            self.inode_size = 128
        # runtime params (как g_bs/g_spb/g_ppb/g_dpb в ядре)
        self.BS  = self.block_size
        self.SPB = self.BS // SECTOR             # секторов в блоке
        self.PPB = self.BS // 4                  # указателей в блоке
        self.DPB = self.BS // DIRENT_SIZE        # dirent'ов в блоке
        self.IPS = SECTOR // self.inode_size     # инодов в секторе

    # --- Superblock I/O ---------------------------------------------------
    def _write_superblock(self):
        sb = struct.pack('<' + 'I' * 14,
            self.magic, self.version, self.total_blocks, self.total_inodes,
            self.free_blocks, self.free_inodes,
            self.ibm_start, self.bbm_start, self.itb_start, self.dat_start,
            self.root_inode, self.jrn_start, self.jrn_sectors,
            self.block_size if self.version >= 3 else 0)
        sb = sb.ljust(SECTOR, b'\x00')
        self.f.seek(0)
        self.f.write(sb)

    # --- Sector I/O -------------------------------------------------------
    def _read_sector(self, rel):
        self.f.seek(rel * SECTOR)
        return bytearray(self.f.read(SECTOR))

    def _write_sector(self, rel, data):
        assert len(data) == SECTOR
        self.f.seek(rel * SECTOR)
        self.f.write(data)

    # --- Bitmap operations ------------------------------------------------
    def _bm_set(self, bm_start, idx, val):
        sec = bm_start + (idx // 8) // SECTOR
        off = (idx // 8) % SECTOR
        bit = idx % 8
        data = self._read_sector(sec)
        if val:
            data[off] |= (1 << bit)
        else:
            data[off] &= ~(1 << bit)
        self._write_sector(sec, data)

    def _bm_alloc(self, bm_start, maximum):
        """Find first free bit, set it, return index or -1."""
        bm_sectors = (maximum // 8 + SECTOR - 1) // SECTOR
        global_bit = 0
        for s in range(bm_sectors):
            if global_bit >= maximum:
                break
            data = self._read_sector(bm_start + s)
            for b in range(SECTOR):
                if global_bit >= maximum:
                    break
                if data[b] == 0xFF:
                    global_bit += 8
                    continue
                for bit in range(8):
                    if global_bit >= maximum:
                        break
                    if not (data[b] & (1 << bit)):
                        data[b] |= (1 << bit)
                        self._write_sector(bm_start + s, data)
                        return global_bit
                    global_bit += 1
        return -1

    # --- Inode I/O --------------------------------------------------------
    def _ino_read(self, ino):
        sec = self.itb_start + ino // self.IPS
        off = (ino % self.IPS) * self.inode_size
        data = self._read_sector(sec)
        return data[off:off + self.inode_size]

    def _ino_write(self, ino, inode_data):
        assert len(inode_data) == self.inode_size
        sec = self.itb_start + ino // self.IPS
        off = (ino % self.IPS) * self.inode_size
        data = self._read_sector(sec)
        data[off:off + self.inode_size] = inode_data
        self._write_sector(sec, data)

    def _ino_alloc(self):
        n = self._bm_alloc(self.ibm_start, self.total_inodes)
        if n >= 0:
            self.free_inodes -= 1
            self._write_superblock()
        return n

    # --- Inode parsing helpers --------------------------------------------
    def _parse_inode(self, raw):
        if self.version >= 3:
            typ, blocks = struct.unpack_from('<II', raw, 0)
            size = struct.unpack_from('<Q', raw, 8)[0]
            direct = list(struct.unpack_from('<' + 'I' * MAX_DIRECT, raw, 16))
            indirect, dbl, trpl = struct.unpack_from('<III', raw, 16 + MAX_DIRECT * 4)
            mode, uid, gid = struct.unpack_from('<HBB', raw, 28 + MAX_DIRECT * 4)
            nlinks = struct.unpack_from('<I', raw, 32 + MAX_DIRECT * 4)[0]
        else:
            typ, size, blocks = struct.unpack_from('<III', raw, 0)
            direct = list(struct.unpack_from('<' + 'I' * MAX_DIRECT, raw, 12))
            indirect, dbl = struct.unpack_from('<II', raw, 12 + MAX_DIRECT * 4)
            mode, uid, gid = struct.unpack_from('<HBB', raw, 20 + MAX_DIRECT * 4)
            trpl, nlinks = 0, 1
        return {
            'type': typ, 'size': size, 'blocks': blocks, 'direct': direct,
            'indirect': indirect, 'dbl_indirect': dbl, 'trpl_indirect': trpl,
            'mode': mode, 'uid': uid, 'gid': gid, 'nlinks': nlinks,
        }

    def _pack_inode(self, d):
        if self.version >= 3:
            raw  = struct.pack('<II', d['type'], d['blocks'])
            raw += struct.pack('<Q', d['size'])
            raw += struct.pack('<' + 'I' * MAX_DIRECT, *d['direct'])
            raw += struct.pack('<III', d['indirect'],
                               d.get('dbl_indirect', 0), d.get('trpl_indirect', 0))
            raw += struct.pack('<HBB', d.get('mode', 0), d.get('uid', 0), d.get('gid', 0))
            raw += struct.pack('<I', d.get('nlinks', 1))
            raw += b'\x00' * 52
        else:
            raw  = struct.pack('<III', d['type'], d['size'], d['blocks'])
            raw += struct.pack('<' + 'I' * MAX_DIRECT, *d['direct'])
            raw += struct.pack('<II', d['indirect'], d.get('dbl_indirect', 0))
            raw += struct.pack('<HBB', d.get('mode', 0), d.get('uid', 0), d.get('gid', 0))
        assert len(raw) == self.inode_size
        return raw

    # --- Data block I/O (логический блок = SPB секторов) --------------------
    def _blk_read(self, blk):
        out = bytearray()
        for s in range(self.SPB):
            out += self._read_sector(self.dat_start + blk * self.SPB + s)
        return out

    def _blk_write(self, blk, data):
        assert len(data) == self.BS
        for s in range(self.SPB):
            self._write_sector(self.dat_start + blk * self.SPB + s,
                               data[s * SECTOR:(s + 1) * SECTOR])

    def _blk_alloc(self):
        n = self._bm_alloc(self.bbm_start, self.total_blocks)
        if n >= 0:
            self.free_blocks -= 1
            self._write_superblock()
            self._blk_write(n, bytearray(self.BS))  # zero out
        return n

    # --- Block mapping (direct + indirect + double + triple[v3]) ------------
    def _read_ptr(self, blk, slot):
        buf = self._blk_read(blk)
        return struct.unpack_from('<I', buf, slot * 4)[0]

    def _inode_get_blk(self, d, idx):
        P = self.PPB
        if idx < MAX_DIRECT:
            return d['direct'][idx]
        off = idx - MAX_DIRECT

        if off < P:                                       # single indirect
            if d['indirect'] == 0:
                return 0
            return self._read_ptr(d['indirect'], off)
        off -= P

        if off < P * P:                                   # double indirect
            if d.get('dbl_indirect', 0) == 0:
                return 0
            ptr = self._read_ptr(d['dbl_indirect'], off // P)
            if ptr == 0:
                return 0
            return self._read_ptr(ptr, off % P)
        off -= P * P

        if self.version < 3 or d.get('trpl_indirect', 0) == 0:  # triple (v3)
            return 0
        t2 = self._read_ptr(d['trpl_indirect'], off // (P * P))
        if t2 == 0:
            return 0
        rem = off % (P * P)
        t3 = self._read_ptr(t2, rem // P)
        if t3 == 0:
            return 0
        return self._read_ptr(t3, rem % P)

    def _set_ptr(self, blk, slot, val):
        buf = self._blk_read(blk)
        struct.pack_into('<I', buf, slot * 4, val)
        self._blk_write(blk, buf)

    def _ensure_ptr(self, blk, slot):
        """Вернуть указатель из таблицы, выделив новый блок при нуле."""
        ptr = self._read_ptr(blk, slot)
        if ptr == 0:
            nb = self._blk_alloc()
            if nb < 0:
                return -1
            self._set_ptr(blk, slot, nb)
            ptr = nb
        return ptr

    def _inode_set_blk(self, d, idx, blk):
        P = self.PPB
        if idx < MAX_DIRECT:
            d['direct'][idx] = blk
            return 0
        off = idx - MAX_DIRECT

        if off < P:                                       # single indirect
            if d['indirect'] == 0:
                ib = self._blk_alloc()
                if ib < 0:
                    return -1
                d['indirect'] = ib
            self._set_ptr(d['indirect'], off, blk)
            return 0
        off -= P

        if off < P * P:                                   # double indirect
            if d.get('dbl_indirect', 0) == 0:
                db = self._blk_alloc()
                if db < 0:
                    return -1
                d['dbl_indirect'] = db
            ptr = self._ensure_ptr(d['dbl_indirect'], off // P)
            if ptr < 0:
                return -1
            self._set_ptr(ptr, off % P, blk)
            return 0
        off -= P * P

        if self.version < 3 or off >= P * P * P:          # triple (v3)
            return -1
        if d.get('trpl_indirect', 0) == 0:
            tb = self._blk_alloc()
            if tb < 0:
                return -1
            d['trpl_indirect'] = tb
        t2 = self._ensure_ptr(d['trpl_indirect'], off // (P * P))
        if t2 < 0:
            return -1
        rem = off % (P * P)
        t3 = self._ensure_ptr(t2, rem // P)
        if t3 < 0:
            return -1
        self._set_ptr(t3, rem % P, blk)
        return 0

    # --- Directory operations ---------------------------------------------
    def _dir_find(self, dir_ino, name):
        """Find entry by name in directory. Returns (inode_num, type) or None."""
        di = self._parse_inode(self._ino_read(dir_ino))
        name_bytes = name.encode('utf-8')[:NAME_MAX]

        for bi in range(di['blocks']):
            b = self._inode_get_blk(di, bi)
            if b == 0:
                continue
            blk_data = self._blk_read(b)
            for e in range(self.DPB):
                off = e * DIRENT_SIZE
                ent_name = blk_data[off:off + NAME_MAX + 1]
                null_pos = ent_name.find(b'\x00')
                if null_pos >= 0:
                    ent_name = ent_name[:null_pos]
                if len(ent_name) == 0:
                    continue
                ent_ino = struct.unpack_from('<I', blk_data, off + NAME_MAX + 1)[0]
                if ent_name == name_bytes:
                    child = self._parse_inode(self._ino_read(ent_ino))
                    return ent_ino, child['type']
        return None

    def _dir_add(self, dir_ino, name, child_ino):
        """Add a directory entry (name → child_ino) to directory dir_ino."""
        di = self._parse_inode(self._ino_read(dir_ino))
        name_bytes = name.encode('utf-8')[:NAME_MAX]

        # Look for a free slot in existing blocks
        for bi in range(di['blocks']):
            b = self._inode_get_blk(di, bi)
            if b == 0:
                continue
            blk_data = self._blk_read(b)
            for e in range(self.DPB):
                off = e * DIRENT_SIZE
                if blk_data[off] == 0:  # empty slot
                    blk_data[off:off + NAME_MAX + 1] = name_bytes.ljust(NAME_MAX + 1, b'\x00')
                    struct.pack_into('<I', blk_data, off + NAME_MAX + 1, child_ino)
                    self._blk_write(b, blk_data)
                    di['size'] += DIRENT_SIZE
                    self._ino_write(dir_ino, self._pack_inode(di))
                    return 0

        # No free slot — allocate new block
        nb = self._blk_alloc()
        if nb < 0:
            return -1
        if self._inode_set_blk(di, di['blocks'], nb) != 0:
            return -1
        di['blocks'] += 1

        blk_data = bytearray(self.BS)
        blk_data[0:NAME_MAX + 1] = name_bytes.ljust(NAME_MAX + 1, b'\x00')
        struct.pack_into('<I', blk_data, NAME_MAX + 1, child_ino)
        self._blk_write(nb, blk_data)

        di['size'] += DIRENT_SIZE
        self._ino_write(dir_ino, self._pack_inode(di))
        return 0

    def _new_inode(self, typ, size=0, mode=0):
        return {
            'type': typ, 'size': size, 'blocks': 0,
            'direct': [0] * MAX_DIRECT,
            'indirect': 0, 'dbl_indirect': 0, 'trpl_indirect': 0,
            'mode': mode, 'uid': 0, 'gid': 0, 'nlinks': 1,
        }

    def mkdir(self, dir_ino, name):
        """Create a subdirectory under dir_ino. Returns new inode number."""
        found = self._dir_find(dir_ino, name)
        if found:
            ino, typ = found
            if typ == VFS_DIR:
                return ino  # already exists as dir, return it
            raise RuntimeError(f"'{name}' already exists and is not a directory")

        ino = self._ino_alloc()
        if ino < 0:
            raise RuntimeError("No free inodes")

        self._ino_write(ino, self._pack_inode(self._new_inode(VFS_DIR, mode=0o755)))

        if self._dir_add(dir_ino, name, ino) != 0:
            raise RuntimeError("Failed to add directory entry")
        return ino

    def walk_dirs(self, components):
        """Walk/create directory path from root. Returns final dir inode."""
        cur = self.root_inode
        for comp in components:
            cur = self.mkdir(cur, comp)
        return cur

    def add_file(self, content, dest_path, mode=None):
        """Add a file with given content to dest_path in the image."""
        parts = [p for p in dest_path.strip('/').split('/') if p]
        if not parts:
            raise ValueError("empty destination path")
        filename, dirs = parts[-1], parts[:-1]

        dir_ino = self.walk_dirs(dirs) if dirs else self.root_inode

        found = self._dir_find(dir_ino, filename)
        if found:
            ino, typ = found
            if typ == VFS_DIR:
                raise RuntimeError(f"'{dest_path}' already exists as a directory")
            # For build tooling we don't free old blocks — образ собирается
            # с нуля при каждом make, утечка не накапливается.

        ino = self._ino_alloc()
        if ino < 0:
            raise RuntimeError("No free inodes")

        # /bin/* — исполняемые (0755), остальное — данные (0644)
        new_inode = self._new_inode(
            VFS_FILE, size=len(content),
            mode=mode if mode is not None
                 else (0o755 if dest_path.strip('/').startswith('bin/') else 0o644))

        # Write data blocks
        offset = 0
        while offset < len(content):
            chunk = content[offset:offset + self.BS]
            blk = self._blk_alloc()
            if blk < 0:
                raise RuntimeError("No free data blocks")
            bi = new_inode['blocks']
            if self._inode_set_blk(new_inode, bi, blk) != 0:
                raise RuntimeError("Failed to set block pointer")
            new_inode['blocks'] += 1
            padded = bytearray(self.BS)
            padded[:len(chunk)] = chunk
            self._blk_write(blk, padded)
            offset += self.BS

        # Handle empty file (0 bytes, 0 blocks)
        self._ino_write(ino, self._pack_inode(new_inode))

        if self._dir_add(dir_ino, filename, ino) != 0:
            raise RuntimeError("Failed to add directory entry")

    def close(self):
        self.f.close()


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <image> <source_file|text> <dest_path>")
        print(f"       {sys.argv[0]} <image> --mkdir <dir_path>")
        sys.exit(1)

    img_path, source, dest = sys.argv[1], sys.argv[2], sys.argv[3]

    # Fix MSYS2 path mangling (same as add_file.py)
    dest = dest.replace('\\', '/')
    if len(dest) >= 2 and dest[1] == ':':
        for marker in ('/usr/bin/', '/bin/', '/etc/', '/home/', '/tmp/'):
            idx = dest.lower().find(marker)
            if idx != -1:
                fixed = dest[idx:]
                if marker == '/usr/bin/':
                    fixed = '/bin/' + dest[idx + len(marker):]
                print(f"warning: MSYS2 path mangling detected ({dest!r}), "
                      f"using {fixed!r}.")
                dest = fixed
                break
        else:
            print(f"error: dest path {dest!r} looks like a mangled Windows path.")
            sys.exit(1)

    img = VortexFSImage(img_path)
    try:
        if source == '--mkdir':
            img.walk_dirs([p for p in dest.strip('/').split('/') if p])
            print(f"mkdir '{dest}' OK")
        else:
            if os.path.isfile(source):
                with open(source, 'rb') as sf:
                    content = sf.read()
            else:
                content = source.encode('utf-8')
            img.add_file(content, dest)
            print(f"Added '{dest}' ({len(content)} bytes)")
    finally:
        img.close()


if __name__ == '__main__':
    main()
