#!/usr/bin/env python3
"""
mkvortexfs.py — создаёт образ диска, отформатированный как VortexFS.

Использование:
    python3 tools/mkvortexfs.py build/vortexfs.img 16
        → создаст 16 МБ образ с VortexFS

On-disk layout (512-byte sectors):
    Sector 0:           Superblock
    Sectors 1..1:       Inode bitmap     (4096 inodes, 1 sector)
    Sectors 2..9:       Block bitmap     (up to 32768 blocks, 8 sectors)
    Sectors 10..521:    Inode table      (4096 × 64 bytes = 512 sectors)
    Sectors 522+:       Data blocks

Block 0 is reserved as sentinel. Root dir uses block 1.
"""

import struct
import sys
import os

SECTOR = 512
VTXFS_MAGIC   = 0x56545846   # "VTXF"
VTXFS_VERSION = 1
MAX_INODES    = 4096
INODE_SIZE    = 64

# VFS types (must match vfs.h)
VFS_DIR = 0x02


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <image_path> <size_mb>")
        sys.exit(1)

    img_path = sys.argv[1]
    size_mb  = int(sys.argv[2])
    total_sectors = size_mb * 1024 * 1024 // SECTOR

    # --- Calculate layout ---
    ibm_sec = (MAX_INODES // 8 + SECTOR - 1) // SECTOR       # 1
    bbm_sec = (total_sectors // 8 + SECTOR - 1) // SECTOR     # ~8
    itb_sec = (MAX_INODES * INODE_SIZE + SECTOR - 1) // SECTOR # 512

    ibm_start = 1
    bbm_start = ibm_start + ibm_sec
    itb_start = bbm_start + bbm_sec
    dat_start = itb_start + itb_sec
    data_blocks = total_sectors - dat_start

    print(f"VortexFS layout:")
    print(f"  Total sectors:     {total_sectors}")
    print(f"  Inode bitmap:      sector {ibm_start} ({ibm_sec} sectors)")
    print(f"  Block bitmap:      sector {bbm_start} ({bbm_sec} sectors)")
    print(f"  Inode table:       sector {itb_start} ({itb_sec} sectors)")
    print(f"  Data blocks:       sector {dat_start}+ ({data_blocks} blocks)")

    # --- Create image ---
    img = bytearray(total_sectors * SECTOR)

    # --- Superblock (sector 0) ---
    sb = struct.pack('<IIIIIIIIIII',
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

    # --- Root inode (inode 0) ---
    root_inode = struct.pack('<III',
        VFS_DIR,  # type
        0,        # size (empty directory)
        1,        # blocks = 1 (one block allocated)
    )
    # direct[0] = 1 (data block 1), direct[1..9] = 0
    direct = struct.pack('<' + 'I' * 10, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    # indirect = 0, dbl = 0, mode = 0755 (root:root), uid = gid = 0
    tail = struct.pack('<II', 0, 0) + struct.pack('<HBB', 0o755, 0, 0)
    inode_data = root_inode + direct + tail
    assert len(inode_data) == INODE_SIZE

    inode_off = itb_start * SECTOR  # inode 0 at start of table
    img[inode_off:inode_off + INODE_SIZE] = inode_data

    # Data block 1 (root dir) is already zeroed (empty dir)

    # --- Write image ---
    with open(img_path, 'wb') as f:
        f.write(img)

    print(f"VortexFS image written: {img_path} ({size_mb} MB)")


if __name__ == '__main__':
    main()
