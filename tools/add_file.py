#!/usr/bin/env python3
"""
Добавляет файл в FAT32 образ диска
"""
import struct
import sys
import os

def add_file_to_fat32(disk_path, source_file, dest_path):
    # Читаем исходный файл
    if os.path.isfile(source_file):
        with open(source_file, 'rb') as sf:
            content = sf.read()
    else:
        # Если не файл, то это текстовая строка
        content = source_file.encode('utf-8')
    
    # Парсим путь назначения
    parts = dest_path.strip('/').split('/')
    if len(parts) > 1:
        # Есть директория - пока не поддерживается полностью
        # Создадим файл в root с полным именем
        filename = '_'.join(parts)  # Временное решение
    else:
        filename = parts[0] if parts else 'file.txt'
    
    with open(disk_path, 'r+b') as f:
        # Читаем boot sector
        f.seek(0)
        boot = f.read(512)
        
        bytes_per_sector = struct.unpack_from('<H', boot, 11)[0]
        sectors_per_cluster = struct.unpack_from('<B', boot, 13)[0]
        reserved_sectors = struct.unpack_from('<H', boot, 14)[0]
        num_fats = struct.unpack_from('<B', boot, 16)[0]
        sectors_per_fat = struct.unpack_from('<I', boot, 36)[0]
        root_cluster = struct.unpack_from('<I', boot, 44)[0]
        
        # Вычисляем позиции
        fat_begin_lba = reserved_sectors
        cluster_begin_lba = reserved_sectors + (num_fats * sectors_per_fat)
        
        # Ищем свободный slot в root directory
        root_lba = cluster_begin_lba + (root_cluster - 2) * sectors_per_cluster
        f.seek(root_lba * bytes_per_sector)
        root_data = f.read(sectors_per_cluster * bytes_per_sector)
        
        free_slot = -1
        for i in range(0, len(root_data), 32):
            first_byte = root_data[i]
            if first_byte == 0x00 or first_byte == 0xE5:  # Free slot
                free_slot = i
                break
        
        if free_slot == -1:
            print("Error: No free directory entries")
            sys.exit(1)
        
        # Ищем свободный cluster для данных
        # Читаем FAT
        f.seek(fat_begin_lba * bytes_per_sector)
        fat = f.read(sectors_per_fat * bytes_per_sector)
        
        free_cluster = -1
        for i in range(2, sectors_per_fat * bytes_per_sector // 4):
            entry = struct.unpack_from('<I', fat, i * 4)[0]
            if entry == 0:  # Free cluster
                free_cluster = i
                break
        
        if free_cluster == -1:
            print("Error: No free clusters")
            sys.exit(1)
        
        # Создаём 8.3 имя
        name_base = filename.upper()[:8].ljust(8)
        name_ext = '   '
        if '.' in filename:
            parts = filename.upper().split('.')
            name_base = parts[0][:8].ljust(8)
            name_ext = parts[1][:3].ljust(3)
        
        # Создаём directory entry
        dirent = bytearray(32)
        dirent[0:8] = name_base.encode('ascii')
        dirent[8:11] = name_ext.encode('ascii')
        dirent[11] = 0x20  # Attribute: Archive
        dirent[20:22] = struct.pack('<H', (free_cluster >> 16) & 0xFFFF)  # Cluster high
        dirent[26:28] = struct.pack('<H', free_cluster & 0xFFFF)  # Cluster low
        dirent[28:32] = struct.pack('<I', len(content))  # File size
        
        # Пишем directory entry
        f.seek(root_lba * bytes_per_sector + free_slot)
        f.write(dirent)
        
        # Пишем данные в clusters
        clusters_needed = (len(content) + (sectors_per_cluster * bytes_per_sector) - 1) // (sectors_per_cluster * bytes_per_sector)
        current_cluster = free_cluster
        offset = 0
        
        for i in range(clusters_needed):
            # Пишем данные
            data_lba = cluster_begin_lba + (current_cluster - 2) * sectors_per_cluster
            chunk_size = min(sectors_per_cluster * bytes_per_sector, len(content) - offset)
            f.seek(data_lba * bytes_per_sector)
            f.write(content[offset:offset + chunk_size])
            offset += chunk_size
            
            # Обновляем FAT
            if i < clusters_needed - 1:
                # Ищем следующий свободный cluster
                next_cluster = -1
                for j in range(current_cluster + 1, sectors_per_fat * bytes_per_sector // 4):
                    entry = struct.unpack_from('<I', fat, j * 4)[0]
                    if entry == 0:
                        next_cluster = j
                        break
                
                if next_cluster == -1:
                    print("Error: Out of clusters")
                    sys.exit(1)
                
                # Обновляем FAT: current -> next
                for fat_num in range(num_fats):
                    fat_offset = (fat_begin_lba + fat_num * sectors_per_fat) * bytes_per_sector + current_cluster * 4
                    f.seek(fat_offset)
                    f.write(struct.pack('<I', next_cluster))
                
                current_cluster = next_cluster
            else:
                # Последний cluster - ставим EOF
                for fat_num in range(num_fats):
                    fat_offset = (fat_begin_lba + fat_num * sectors_per_fat) * bytes_per_sector + current_cluster * 4
                    f.seek(fat_offset)
                    f.write(struct.pack('<I', 0x0FFFFFF8))
        
        print(f"Added '{filename}' ({len(content)} bytes) to FAT32 disk in cluster {free_cluster}")

if __name__ == '__main__':
    if len(sys.argv) != 4:
        print("Usage: add_file.py <disk.img> <source_file> <dest_path>")
        print("  source_file: file path or text string")
        print("  dest_path: /path/on/disk")
        sys.exit(1)
    
    add_file_to_fat32(sys.argv[1], sys.argv[2], sys.argv[3])

