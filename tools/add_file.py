#!/usr/bin/env python3
"""
Добавляет файл (или директорию) в FAT32 образ диска.

Поддерживает подкаталоги: `add_file.py disk.img userspace/vwm /bin/vwm`
создаст каталог /bin (если его нет) и положит файл внутрь. Недостающие
каталоги по пути создаются автоматически (с записями `.` и `..`).
Существующий файл с тем же именем перезаписывается (старая цепочка
кластеров освобождается) — повторный `make disk-with-apps` не плодит
дубликатов.

Режимы:
    add_file.py <disk.img> <source_file|text> <dest_path>   — записать файл
    add_file.py <disk.img> --mkdir <dir_path>               — создать каталог

Имена — классические 8.3 (как понимает ядро VortexOS), регистр не важен.
"""
import struct
import sys
import os

ATTR_ARCHIVE = 0x20
ATTR_DIR     = 0x10
ATTR_LFN     = 0x0F
ATTR_VOLUME  = 0x08
FAT_EOC      = 0x0FFFFFF8
FAT_FREE     = 0x00000000


class Fat32Image:
    def __init__(self, path):
        self.f = open(path, 'r+b')
        self.f.seek(0)
        boot = self.f.read(512)
        self.bps  = struct.unpack_from('<H', boot, 11)[0]   # bytes/sector
        self.spc  = struct.unpack_from('<B', boot, 13)[0]   # sectors/cluster
        reserved  = struct.unpack_from('<H', boot, 14)[0]
        self.nfats = struct.unpack_from('<B', boot, 16)[0]
        self.spf  = struct.unpack_from('<I', boot, 36)[0]   # sectors/FAT
        self.root_cluster = struct.unpack_from('<I', boot, 44)[0]

        self.fat_lba = reserved
        self.data_lba = reserved + self.nfats * self.spf
        self.cluster_bytes = self.spc * self.bps

        # FAT целиком в память; пишем обе копии при close()
        self.f.seek(self.fat_lba * self.bps)
        self.fat = bytearray(self.f.read(self.spf * self.bps))
        self.nclusters = len(self.fat) // 4

        # FAT[0]/FAT[1] — служебные; корневой кластер должен быть занят.
        if self.fat_get(0) == 0:
            self.fat_set(0, 0x0FFFFFF8)
            self.fat_set(1, 0x0FFFFFFF)
        if self.fat_get(self.root_cluster) == FAT_FREE:
            self.fat_set(self.root_cluster, FAT_EOC)

    # --- FAT --------------------------------------------------------------
    def fat_get(self, c):
        return struct.unpack_from('<I', self.fat, c * 4)[0] & 0x0FFFFFFF

    def fat_set(self, c, v):
        struct.pack_into('<I', self.fat, c * 4, v & 0x0FFFFFFF)

    def alloc_cluster(self):
        for c in range(2, self.nclusters):
            if self.fat_get(c) == FAT_FREE:
                self.fat_set(c, FAT_EOC)
                self.write_cluster(c, b'\x00' * self.cluster_bytes)
                return c
        raise RuntimeError('FAT32: нет свободных кластеров')

    def free_chain(self, c):
        while 2 <= c < FAT_EOC:
            nxt = self.fat_get(c)
            self.fat_set(c, FAT_FREE)
            c = nxt

    def chain(self, c):
        out = []
        while 2 <= c < FAT_EOC:
            out.append(c)
            c = self.fat_get(c)
        return out

    # --- кластеры ----------------------------------------------------------
    def cluster_lba(self, c):
        return self.data_lba + (c - 2) * self.spc

    def read_cluster(self, c):
        self.f.seek(self.cluster_lba(c) * self.bps)
        return bytearray(self.f.read(self.cluster_bytes))

    def write_cluster(self, c, data):
        assert len(data) == self.cluster_bytes
        self.f.seek(self.cluster_lba(c) * self.bps)
        self.f.write(data)

    # --- 8.3 имена ----------------------------------------------------------
    @staticmethod
    def name_83(name):
        name = name.upper()
        if '.' in name:
            base, ext = name.rsplit('.', 1)
        else:
            base, ext = name, ''
        base, ext = base[:8], ext[:3]
        if not base:
            raise ValueError(f'плохое имя FAT: {name!r}')
        return base.ljust(8).encode('ascii') + ext.ljust(3).encode('ascii')

    # --- директории ----------------------------------------------------------
    def dir_entries(self, dir_cluster):
        """Итератор (cluster, offset_in_cluster, raw32) по записям каталога."""
        for c in self.chain(dir_cluster):
            data = self.read_cluster(c)
            for off in range(0, self.cluster_bytes, 32):
                yield c, off, data[off:off + 32]

    def find_entry(self, dir_cluster, name):
        want = self.name_83(name)
        for c, off, raw in self.dir_entries(dir_cluster):
            if raw[0] in (0x00, 0xE5):
                if raw[0] == 0x00:
                    return None
                continue
            if raw[11] == ATTR_LFN or (raw[11] & ATTR_VOLUME):
                continue
            if raw[0:11] == want:
                return c, off, raw
        return None

    def find_free_slot(self, dir_cluster):
        for c, off, raw in self.dir_entries(dir_cluster):
            if raw[0] in (0x00, 0xE5):
                return c, off
        # каталог полон — расширяем цепочку
        last = self.chain(dir_cluster)[-1]
        newc = self.alloc_cluster()
        self.fat_set(last, newc)
        return newc, 0

    def write_entry(self, c, off, raw):
        data = self.read_cluster(c)
        data[off:off + 32] = raw
        self.write_cluster(c, data)

    @staticmethod
    def make_dirent(name83, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        struct.pack_into('<H', e, 20, (cluster >> 16) & 0xFFFF)
        struct.pack_into('<H', e, 26, cluster & 0xFFFF)
        struct.pack_into('<I', e, 28, size)
        return bytes(e)

    @staticmethod
    def entry_cluster(raw):
        hi = struct.unpack_from('<H', raw, 20)[0]
        lo = struct.unpack_from('<H', raw, 26)[0]
        return (hi << 16) | lo

    def make_dir(self, parent_cluster, name):
        """Создаёт подкаталог, возвращает его кластер."""
        newc = self.alloc_cluster()
        data = bytearray(self.cluster_bytes)
        data[0:32]  = self.make_dirent(b'.       ' + b'   ', ATTR_DIR, newc, 0)
        # у `..` корня cluster = 0 по соглашению FAT
        pc = 0 if parent_cluster == self.root_cluster else parent_cluster
        data[32:64] = self.make_dirent(b'..      ' + b'   ', ATTR_DIR, pc, 0)
        self.write_cluster(newc, data)

        c, off = self.find_free_slot(parent_cluster)
        self.write_entry(c, off, self.make_dirent(self.name_83(name), ATTR_DIR, newc, 0))
        return newc

    def walk_dirs(self, components, create=True):
        """Спускается по списку каталогов от корня, создавая недостающие."""
        cur = self.root_cluster
        for comp in components:
            found = self.find_entry(cur, comp)
            if found:
                _, _, raw = found
                if not (raw[11] & ATTR_DIR):
                    raise RuntimeError(f'/{comp}: уже существует и это не каталог')
                cur = self.entry_cluster(raw)
            elif create:
                cur = self.make_dir(cur, comp)
            else:
                return None
        return cur

    # --- файлы ----------------------------------------------------------------
    def add_file(self, content, dest_path):
        parts = [p for p in dest_path.strip('/').split('/') if p]
        if not parts:
            raise ValueError('пустой путь назначения')
        filename, dirs = parts[-1], parts[:-1]
        dir_cluster = self.walk_dirs(dirs)

        # перезапись: убираем старую запись и освобождаем её кластеры
        old = self.find_entry(dir_cluster, filename)
        if old:
            c, off, raw = old
            if raw[11] & ATTR_DIR:
                raise RuntimeError(f'{dest_path}: уже существует как каталог')
            oldc = self.entry_cluster(raw)
            if oldc >= 2:
                self.free_chain(oldc)
            self.write_entry(c, off, b'\xE5' + raw[1:])

        # данные
        first = 0
        if content:
            clusters = []
            for i in range(0, len(content), self.cluster_bytes):
                clusters.append(self.alloc_cluster())
            for i, c in enumerate(clusters):
                chunk = content[i * self.cluster_bytes:(i + 1) * self.cluster_bytes]
                self.write_cluster(c, chunk.ljust(self.cluster_bytes, b'\x00'))
                self.fat_set(c, clusters[i + 1] if i + 1 < len(clusters) else FAT_EOC)
            first = clusters[0]

        c, off = self.find_free_slot(dir_cluster)
        self.write_entry(c, off, self.make_dirent(
            self.name_83(filename), ATTR_ARCHIVE, first, len(content)))

    def close(self):
        for n in range(self.nfats):
            self.f.seek((self.fat_lba + n * self.spf) * self.bps)
            self.f.write(self.fat)
        self.f.close()


def main():
    if len(sys.argv) != 4:
        print('Usage: add_file.py <disk.img> <source_file|text> <dest_path>')
        print('       add_file.py <disk.img> --mkdir <dir_path>')
        sys.exit(1)

    disk_path, source, dest = sys.argv[1], sys.argv[2], sys.argv[3]
    img = Fat32Image(disk_path)
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
