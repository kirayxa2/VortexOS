# VortexOS — README

## Структура проекта

```
D:\VOS\
├── kernel/
│   ├── arch/x86_64/
│   │   ├── boot.asm        — точка входа, Multiboot2, long mode
│   │   ├── gdt.c/.h        — GDT + TSS
│   │   └── gdt_flush.asm   — lgdt, ltr (asm-обёртки)
│   ├── drivers/
│   │   └── vga.c/.h        — VGA text mode 80x25
│   ├── mm/
│   │   └── pmm.c/.h        — физический менеджер памяти (bitmap)
│   ├── include/
│   │   └── types.h         — uint8_t, uint64_t, bool и т.д.
│   └── kmain.c             — точка входа ядра на C
├── kernel.ld               — linker script
├── Makefile                — сборка, ISO, QEMU
└── iso/boot/grub/          — создаётся при `make iso`
```

## Сборка

### 1. Установка cross-compiler (Linux/WSL)

```bash
# Устанавливаем зависимости
sudo apt install build-essential bison flex libgmp3-dev libmpc-dev \
                 libmpfr-dev texinfo nasm xorriso grub-pc-bin qemu-system-x86

# Собираем x86_64-elf-gcc (скрипт из OSDev Wiki)
export PREFIX="$HOME/opt/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"

# binutils
wget https://ftp.gnu.org/gnu/binutils/binutils-2.41.tar.gz
tar xf binutils-2.41.tar.gz && mkdir build-binutils && cd build-binutils
../binutils-2.41/configure --target=$TARGET --prefix=$PREFIX \
    --with-sysroot --disable-nls --disable-werror
make -j$(nproc) && make install && cd ..

# gcc
wget https://ftp.gnu.org/gnu/gcc/gcc-13.2.0/gcc-13.2.0.tar.gz
tar xf gcc-13.2.0.tar.gz && mkdir build-gcc && cd build-gcc
../gcc-13.2.0/configure --target=$TARGET --prefix=$PREFIX \
    --disable-nls --enable-languages=c,c++ --without-headers
make -j$(nproc) all-gcc && make install-gcc && cd ..
```

### 2. Сборка и запуск

```bash
# В корне D:\VOS (через WSL: cd /mnt/d/VOS)
make          # собирает build/kernel.bin
make iso      # создаёт build/vortexos.iso
make run      # запускает в QEMU
```

## Этап 1 — что уже реализовано

- [x] Multiboot2 заголовок + загрузка через GRUB
- [x] Переход в 64-bit long mode (boot.asm)
- [x] Identity-mapping первых 2 ГБ (2 МБ huge pages)
- [x] GDT: null, kernel code/data, user code/data, TSS
- [x] VGA text mode драйвер (80x25, цвета, printf)
- [x] PMM: bitmap-аллокатор, разбор Multiboot2 mmap

## Следующий этап (Этап 1, часть 2)

- [ ] VMM (виртуальный менеджер памяти, 4-level paging)
- [ ] kmalloc / kfree (heap allocator в ядре)
- [ ] IDT + базовые обработчики исключений (page fault, GPF)
