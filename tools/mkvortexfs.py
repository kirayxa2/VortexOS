#!/usr/bin/env python3
"""
mkvortexfs.py — создаёт образ диска, отформатированный как VortexFS v3.

Использование:
    python3 tools/mkvortexfs.py build/vortexfs.img 16
        → создаст 16 МБ образ с VortexFS

v3 on-disk layout (512-byte sectors; данные адресуются 4К-БЛОКАМИ):
    Sector 0:           Superblock
    Sectors 1..:        Inode bitmap     (4096 inodes, 1 sector)
    дальше:             Block bitmap
    дальше:             Inode table      (4096 × 128 bytes = 1024 sectors)
    дальше:             Journal          (64 sectors, metadata WAL)
    дальше:             Data blocks      (4096 байт = 8 секторов каждый)

Block 0 is reserved as sentinel. Root dir uses block 1.
Должно 1:1 совпадать с kernel/fs/vortexfs.{h,c} (vortexfs_mkfs).
"""

import struct
import sys

SECTOR = 512
VTXFS_MAGIC   = 0x56545846   # "VTXF"
VTXFS_VERSION = 3            # v3: 4К блоки, инод 128 байт, triple indirect
BLOCK_SIZE    = 4096
SPB           = BLOCK_SIZE // SECTOR   # секторов в блоке = 8
JOURNAL_SECTORS = 64
MAX_INODES    = 4096
INODE_SIZE    = 128

# VFS types (must match vfs.h)
VFS_DIR = 0x02


def pack_inode_v3(typ, blocks, size, direct, indirect=0, dbl=0, trpl=0,
                  mode=0, uid=0, gid=0, nlinks=1):
    raw  = struct.pack('<II', typ, blocks)
    raw += struct.pack('<Q', size)
    raw += struct.pack('<' + 'I' * 10, *direct)
    raw += struct.pack('<III', indirect, dbl, trpl)
    raw += struct.pack('<HBB', mode, uid, gid)
    raw += struct.pack('<I', nlinks)
    raw += b'\x00' * 52
    assert len(raw) == INODE_SIZE
    return raw


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <image_path> <size_mb>")
        sys.exit(1)

    img_path = sys.argv[1]
    size_mb  = int(sys.argv[2])
    total_sectors = size_mb * 1024 * 1024 // SECTOR

    # --- Calculate layout (как vortexfs_mkfs в ядре) ---
    max_blocks = total_sectors // SPB                       # верхняя оценка
    ibm_sec = (MAX_INODES // 8 + SECTOR - 1) // SECTOR       # 1
    bbm_sec = (max_blocks // 8 + SECTOR - 1) // SECTOR
    itb_sec = (MAX_INODES * INODE_SIZE + SECTOR - 1) // SECTOR  # 1024

    ibm_start = 1
    bbm_start = ibm_start + ibm_sec
    itb_start = bbm_start + bbm_sec
    jrn_start = itb_start + itb_sec
    dat_start = jrn_start + JOURNAL_SECTORS
    data_blocks = (total_sectors - dat_start) // SPB

    print(f"VortexFS v3 layout:")
    print(f"  Total sectors:     {total_sectors}")
    print(f"  Block size:        {BLOCK_SIZE} bytes ({SPB} sectors)")
    print(f"  Inode bitmap:      sector {ibm_start} ({ibm_sec} sectors)")
    print(f"  Block bitmap:      sector {bbm_start} ({bbm_sec} sectors)")
    print(f"  Inode table:       sector {itb_start} ({itb_sec} sectors)")
    print(f"  Journal:           sector {jrn_start} ({JOURNAL_SECTORS} sectors)")
    print(f"  Data blocks:       sector {dat_start}+ ({data_blocks} x 4K blocks)")

    # --- Create image ---
    img = bytearray(total_sectors * SECTOR)

    # --- Superblock (sector 0) ---
    sb = struct.pack('<IIIIIIIIIIIIII',
        VTXFS_MAGIC,
        VTXFS_VERSION,
        data_blocks,        # total_blocks
        MAX_INODES,         # total_inodes
        data_blocks - 2,    # free_blocks (block 0 reserved + block 1 for root)
        MAX_INODES - 1,     # free_inodes (inode 0 for root)
        ibm_start,
        bbm_start,
        itb_start,
        dat_start,
        0,                  # root_inode = 0
        jrn_start,
        JOURNAL_SECTORS,
        BLOCK_SIZE,         # v3: block_size
    )
    sb = sb.ljust(SECTOR, b'\x00')
    img[0:SECTOR] = sb

    # --- Helper: set bit in bitmap ---
    def bm_set(bm_start_sector, index):
        byte_off = index // 8
        bit      = index % 8
        abs_off  = bm_start_sector * SECTOR + byte_off
        img[abs_off] |= (1 << bit)

    # --- Mark inode 0 as used ---
    bm_set(ibm_start, 0)

    # --- Mark data block 0 (sentinel) and block 1 (root dir) as used ---
    bm_set(bbm_start, 0)
    bm_set(bbm_start, 1)

    # --- Root inode (inode 0): dir, 1 block, direct[0]=1, mode 0755 ---
    inode_data = pack_inode_v3(VFS_DIR, 1, 0, [1] + [0] * 9, mode=0o755)
    inode_off = itb_start * SECTOR  # inode 0 at start of table
    img[inode_off:inode_off + INODE_SIZE] = inode_data

    # Data block 1 (root dir) is already zeroed (empty dir)

    # --- Write image ---
    with open(img_path, 'wb') as f:
        f.write(img)

    print(f"Created '{img_path}' ({size_mb} MB, VortexFS v3)")


if __name__ == '__main__':
    main()
