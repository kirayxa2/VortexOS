#!/usr/bin/env python3
"""
add_vortexfs_file.py — добавляет файл (или директорию) в VortexFS образ диска.

Поддерживает подкаталоги:
    python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vwm bin/vwm
        → создаст /bin (если нет) и положит файл vwm

Режимы:
    add_vortexfs_file.py <img> <source_file|text> <dest_path>   — записать файл
    add_vortexfs_file.py <img> --mkdir <dir_path>               — создать каталог

On-disk layout (must match kernel/fs/vortexfs.h):
    Sector 0:           Superblock
    Sectors 1..1:       Inode bitmap     (4096 inodes max)
    Sectors 2..9:       Block bitmap     (up to 32768 blocks)
    Sectors 10..521:    Inode table      (4096 × 64 bytes)
    Sectors 522+:       Data blocks

Block 0 is reserved as sentinel. Valid blocks start at 1.
"""

import struct
import sys
import os

SECTOR       = 512
VTXFS_MAGIC  = 0x56545846   # "VTXF"
VTXFS_VERSION = 1
MAX_INODES   = 4096
INODE_SIZE   = 64
NAME_MAX     = 59
DIRENT_SIZE  = 64
DIRENTS_PER_BLOCK = SECTOR // DIRENT_SIZE  # 8
MAX_DIRECT   = 10

# VFS types (must match vfs.h)
VFS_FILE = 0x01
VFS_DIR  = 0x02


class VortexFSImage:
    """Read/write access to a VortexFS disk image."""

    def __init__(self, path):
        self.f = open(path, 'r+b')
        self.f.seek(0)
        raw_sb = self.f.read(SECTOR)
        self._parse_superblock(raw_sb)

    def _parse_superblock(self, raw):
        (self.magic, self.version, self.total_blocks, self.total_inodes,
         self.free_blocks, self.free_inodes,
         self.ibm_start, self.bbm_start, self.itb_start, self.dat_start,
         self.root_inode) = struct.unpack_from('<IIIIIIIIIII', raw, 0)
        if self.magic != VTXFS_MAGIC or self.version != VTXFS_VERSION:
            raise ValueError(f"Not a valid VortexFS image (magic=0x{self.magic:08X})")

    # --- Superblock I/O ---------------------------------------------------
    def _write_superblock(self):
        sb = struct.pack('<IIIIIIIIIII',
            self.magic, self.version, self.total_blocks, self.total_inodes,
            self.free_blocks, self.free_inodes,
            self.ibm_start, self.bbm_start, self.itb_start, self.dat_start,
            self.root_inode)
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
    def _bm_get(self, bm_start, idx):
        sec = bm_start + (idx // 8) // SECTOR
        off = (idx // 8) % SECTOR
        bit = idx % 8
        data = self._read_sector(sec)
        return (data[off] >> bit) & 1

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
        sec = self.itb_start + ino // 8
        off = (ino % 8) * INODE_SIZE
        data = self._read_sector(sec)
        return data[off:off + INODE_SIZE]

    def _ino_write(self, ino, inode_data):
        assert len(inode_data) == INODE_SIZE
        sec = self.itb_start + ino // 8
        off = (ino % 8) * INODE_SIZE
        data = self._read_sector(sec)
        data[off:off + INODE_SIZE] = inode_data
        self._write_sector(sec, data)

    def _ino_alloc(self):
        n = self._bm_alloc(self.ibm_start, self.total_inodes)
        if n >= 0:
            self.free_inodes -= 1
            self._write_superblock()
        return n

    # --- Inode parsing helpers --------------------------------------------
    @staticmethod
    def _parse_inode(raw):
        """Parse 64-byte inode into dict."""
        typ, size, blocks = struct.unpack_from('<III', raw, 0)
        direct = list(struct.unpack_from('<' + 'I' * MAX_DIRECT, raw, 12))
        indirect = struct.unpack_from('<I', raw, 12 + MAX_DIRECT * 4)[0]
        dbl_indirect = struct.unpack_from('<I', raw, 12 + MAX_DIRECT * 4 + 4)[0]
        mode, uid, gid = struct.unpack_from('<HBB', raw, 12 + MAX_DIRECT * 4 + 8)
        return {
            'type': typ,
            'size': size,
            'blocks': blocks,
            'direct': direct,
            'indirect': indirect,
            'dbl_indirect': dbl_indirect,
            'mode': mode,
            'uid': uid,
            'gid': gid,
        }

    @staticmethod
    def _pack_inode(ino_dict):
        """Pack inode dict back to 64 bytes."""
        raw = struct.pack('<III', ino_dict['type'], ino_dict['size'], ino_dict['blocks'])
        raw += struct.pack('<' + 'I' * MAX_DIRECT, *ino_dict['direct'])
        raw += struct.pack('<I', ino_dict['indirect'])
        raw += struct.pack('<I', ino_dict.get('dbl_indirect', 0))
        raw += struct.pack('<HBB', ino_dict.get('mode', 0),
                           ino_dict.get('uid', 0), ino_dict.get('gid', 0))
        assert len(raw) == INODE_SIZE
        return raw

    # --- Data block I/O ---------------------------------------------------
    def _blk_read(self, blk):
        return self._read_sector(self.dat_start + blk)

    def _blk_write(self, blk, data):
        self._write_sector(self.dat_start + blk, data)

    def _blk_alloc(self):
        n = self._bm_alloc(self.bbm_start, self.total_blocks)
        if n >= 0:
            self.free_blocks -= 1
            self._write_superblock()
            # Zero out the new block
            self._blk_write(n, bytearray(SECTOR))
        return n

    # --- Block mapping (direct + indirect) --------------------------------
    def _inode_get_blk(self, ino_dict, idx):
        if idx < MAX_DIRECT:
            return ino_dict['direct'][idx]
        off = idx - MAX_DIRECT

        # Single indirect (128 pointers)
        if off < 128:
            if ino_dict['indirect'] == 0:
                return 0
            ibuf = self._blk_read(ino_dict['indirect'])
            return struct.unpack_from('<I', ibuf, off * 4)[0]

        # Double indirect (128 × 128 pointers)
        off -= 128
        dbl = ino_dict.get('dbl_indirect', 0)
        if dbl == 0 or off >= 128 * 128:
            return 0
        d1 = self._blk_read(dbl)
        slot = off // 128
        ptr = struct.unpack_from('<I', d1, slot * 4)[0]
        if ptr == 0:
            return 0
        d2 = self._blk_read(ptr)
        return struct.unpack_from('<I', d2, (off % 128) * 4)[0]

    def _inode_set_blk(self, ino_dict, idx, blk):
        if idx < MAX_DIRECT:
            ino_dict['direct'][idx] = blk
            return 0
        off = idx - MAX_DIRECT

        # Single indirect
        if off < 128:
            if ino_dict['indirect'] == 0:
                ib = self._blk_alloc()
                if ib < 0:
                    return -1
                ino_dict['indirect'] = ib
            ibuf = self._blk_read(ino_dict['indirect'])
            struct.pack_into('<I', ibuf, off * 4, blk)
            self._blk_write(ino_dict['indirect'], ibuf)
            return 0

        # Double indirect
        off -= 128
        if off >= 128 * 128:
            return -1

        if ino_dict.get('dbl_indirect', 0) == 0:
            db = self._blk_alloc()
            if db < 0:
                return -1
            ino_dict['dbl_indirect'] = db

        d1 = self._blk_read(ino_dict['dbl_indirect'])
        slot = off // 128
        ptr = struct.unpack_from('<I', d1, slot * 4)[0]
        if ptr == 0:
            sb = self._blk_alloc()
            if sb < 0:
                return -1
            struct.pack_into('<I', d1, slot * 4, sb)
            self._blk_write(ino_dict['dbl_indirect'], d1)
            ptr = sb

        d2 = self._blk_read(ptr)
        struct.pack_into('<I', d2, (off % 128) * 4, blk)
        self._blk_write(ptr, d2)
        return 0

    # --- Directory operations ---------------------------------------------
    def _dir_find(self, dir_ino, name):
        """Find entry by name in directory. Returns (inode_num, dirent_type) or None."""
        raw = self._ino_read(dir_ino)
        di = self._parse_inode(raw)
        name_bytes = name.encode('utf-8')[:NAME_MAX]

        for bi in range(di['blocks']):
            b = self._inode_get_blk(di, bi)
            if b == 0:
                continue
            blk_data = self._blk_read(b)
            for e in range(DIRENTS_PER_BLOCK):
                off = e * DIRENT_SIZE
                ent_name = blk_data[off:off + NAME_MAX + 1]
                # Null-terminated name
                null_pos = ent_name.find(b'\x00')
                if null_pos >= 0:
                    ent_name = ent_name[:null_pos]
                if len(ent_name) == 0:
                    continue
                ent_ino = struct.unpack_from('<I', blk_data, off + NAME_MAX + 1)[0]
                if ent_name == name_bytes:
                    # Read the inode to get its type
                    child_raw = self._ino_read(ent_ino)
                    child = self._parse_inode(child_raw)
                    return ent_ino, child['type']
        return None

    def _dir_add(self, dir_ino, name, child_ino):
        """Add a directory entry (name → child_ino) to directory dir_ino."""
        raw = self._ino_read(dir_ino)
        di = self._parse_inode(raw)
        name_bytes = name.encode('utf-8')[:NAME_MAX]

        # Look for a free slot in existing blocks
        for bi in range(di['blocks']):
            b = self._inode_get_blk(di, bi)
            if b == 0:
                continue
            blk_data = self._blk_read(b)
            for e in range(DIRENTS_PER_BLOCK):
                off = e * DIRENT_SIZE
                if blk_data[off] == 0:  # empty slot
                    # Write entry
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

        blk_data = bytearray(SECTOR)
        blk_data[0:NAME_MAX + 1] = name_bytes.ljust(NAME_MAX + 1, b'\x00')
        struct.pack_into('<I', blk_data, NAME_MAX + 1, child_ino)
        self._blk_write(nb, blk_data)

        di['size'] += DIRENT_SIZE
        self._ino_write(dir_ino, self._pack_inode(di))
        return 0

    def mkdir(self, dir_ino, name):
        """Create a subdirectory under dir_ino. Returns new inode number."""
        # Check if already exists
        found = self._dir_find(dir_ino, name)
        if found:
            ino, typ = found
            if typ == VFS_DIR:
                return ino  # already exists as dir, return it
            raise RuntimeError(f"'{name}' already exists and is not a directory")

        ino = self._ino_alloc()
        if ino < 0:
            raise RuntimeError("No free inodes")

        new_inode = {
            'type': VFS_DIR,
            'size': 0,
            'blocks': 0,
            'direct': [0] * MAX_DIRECT,
            'indirect': 0,
            'dbl_indirect': 0,
            'mode': 0o755,   # root:root rwxr-xr-x
        }
        self._ino_write(ino, self._pack_inode(new_inode))

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

        # Navigate/create parent directories
        dir_ino = self.walk_dirs(dirs) if dirs else self.root_inode

        # Check for existing file — remove it (simple overwrite)
        found = self._dir_find(dir_ino, filename)
        if found:
            ino, typ = found
            if typ == VFS_DIR:
                raise RuntimeError(f"'{dest_path}' already exists as a directory")
            # For simplicity, we don't free old blocks/inode — just overwrite
            # In a production FS we'd need proper unlink+free
            # For build tooling this is fine since we format fresh each time

        # Allocate inode
        ino = self._ino_alloc()
        if ino < 0:
            raise RuntimeError("No free inodes")

        new_inode = {
            'type': VFS_FILE,
            'size': len(content),
            'blocks': 0,
            'direct': [0] * MAX_DIRECT,
            'indirect': 0,
            'dbl_indirect': 0,
            # /bin/* — исполняемые (0755), остальное — данные (0644)
            'mode': mode if mode is not None
                    else (0o755 if dest_path.strip('/').startswith('bin/')
                          else 0o644),
        }

        # Write data blocks
        offset = 0
        while offset < len(content):
            chunk = content[offset:offset + SECTOR]
            blk = self._blk_alloc()
            if blk < 0:
                raise RuntimeError("No free data blocks")
            bi = new_inode['blocks']
            if self._inode_set_blk(new_inode, bi, blk) != 0:
                raise RuntimeError("Failed to set block pointer")
            new_inode['blocks'] += 1
            # Pad chunk to sector size
            padded = bytearray(SECTOR)
            padded[:len(chunk)] = chunk
            self._blk_write(blk, padded)
            offset += SECTOR

        # Handle empty file (0 bytes, 0 blocks)
        self._ino_write(ino, self._pack_inode(new_inode))

        # Add directory entry
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
