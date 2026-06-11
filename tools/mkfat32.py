#!/usr/bin/env python3
"""
Создаёт минимальный FAT32 boot sector для тестирования
"""
import struct
import sys

def create_fat32_bootsector(size_mb=16):
    sector_size = 512
    cluster_size = 4096  # 8 sectors per cluster
    sectors_per_cluster = cluster_size // sector_size
    total_sectors = (size_mb * 1024 * 1024) // sector_size
    reserved_sectors = 32
    num_fats = 2
    sectors_per_fat = 128  # Для 16MB достаточно
    
    root_cluster = 2
    
    # Boot sector structure
    boot = bytearray(512)
    
    # Jump instruction
    boot[0:3] = b'\xEB\x58\x90'
    
    # OEM
    boot[3:11] = b'VORTEXOS'
    
    # BPB
    struct.pack_into('<H', boot, 11, sector_size)           # Bytes per sector
    struct.pack_into('<B', boot, 13, sectors_per_cluster)   # Sectors per cluster
    struct.pack_into('<H', boot, 14, reserved_sectors)      # Reserved sectors
    struct.pack_into('<B', boot, 16, num_fats)              # Number of FATs
    struct.pack_into('<H', boot, 17, 0)                     # Root entries (0 for FAT32)
    struct.pack_into('<H', boot, 19, 0)                     # Total sectors 16 (0 for FAT32)
    struct.pack_into('<B', boot, 21, 0xF8)                  # Media descriptor
    struct.pack_into('<H', boot, 22, 0)                     # Sectors per FAT 16 (0 for FAT32)
    struct.pack_into('<H', boot, 24, 32)                    # Sectors per track
    struct.pack_into('<H', boot, 26, 64)                    # Number of heads
    struct.pack_into('<I', boot, 28, 0)                     # Hidden sectors
    struct.pack_into('<I', boot, 32, total_sectors)         # Total sectors 32
    
    # FAT32 Extended BPB
    struct.pack_into('<I', boot, 36, sectors_per_fat)       # Sectors per FAT 32
    struct.pack_into('<H', boot, 40, 0)                     # Flags
    struct.pack_into('<H', boot, 42, 0)                     # Version
    struct.pack_into('<I', boot, 44, root_cluster)          # Root cluster
    struct.pack_into('<H', boot, 48, 1)                     # FSInfo sector
    struct.pack_into('<H', boot, 50, 6)                     # Backup boot sector
    # Reserved 12 bytes (52-63)
    struct.pack_into('<B', boot, 64, 0x80)                  # Drive number
    struct.pack_into('<B', boot, 66, 0x29)                  # Boot signature
    struct.pack_into('<I', boot, 67, 0x12345678)            # Serial number
    boot[71:82] = b'VortexOS   '                            # Volume label (11 bytes)
    boot[82:90] = b'FAT32   '                               # Filesystem type (8 bytes)
    
    # Boot signature
    struct.pack_into('<H', boot, 510, 0xAA55)
    
    return boot

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print("Usage: mkfat32.py <disk.img>")
        sys.exit(1)
    
    disk_path = sys.argv[1]
    boot_sector = create_fat32_bootsector(16)
    
    # Write boot sector to existing disk image
    with open(disk_path, 'r+b') as f:
        f.write(boot_sector)

        # Инициализируем обе копии FAT: FAT[0]/FAT[1] служебные,
        # FAT[2] = EOC (корневой каталог занимает кластер 2).
        # Раньше FAT оставалась нулевой и первый же add_file.py мог
        # выделить кластер 2 под данные — прямо поверх корня.
        reserved_sectors = 32
        sectors_per_fat = 128
        fat_init = struct.pack('<III', 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFF8)
        for n in range(2):
            f.seek((reserved_sectors + n * sectors_per_fat) * 512)
            f.write(fat_init)
    
    print(f"FAT32 boot sector written to {disk_path}")
