# =============================================================================
# VortexOS — Makefile (Windows + MSYS2 UCRT + Limine)
# Запуск: cd /d/VOS && make run
# =============================================================================

TOOLCHAIN := /d/x86_64-elf-tools/bin
QEMU      := /d/qemu/qemu-system-x86_64.exe
LIMINE    := limine
XORRISO   := xorriso

CC  := $(TOOLCHAIN)/x86_64-elf-gcc
LD  := $(TOOLCHAIN)/x86_64-elf-ld
AS  := nasm

CFLAGS := \
    -std=c11              \
    -ffreestanding        \
    -fno-stack-protector  \
    -fno-builtin          \
    -fno-pie              \
    -fno-pic              \
    -mno-red-zone         \
    -mcmodel=kernel       \
    -mno-sse              \
    -mno-sse2             \
    -mno-mmx              \
    -mno-80387            \
    -mgeneral-regs-only   \
    -Wall                 \
    -Wextra               \
    -O2                   \
    -Ikernel/include      \
    -Ikernel/arch/x86_64  \
    -Ikernel/drivers      \
    -Ikernel/mm           \
    -Ikernel/sched        \
    -Ikernel/fs           \
    -Ikernel/ipc

ASFLAGS := -f elf64

LDFLAGS := \
    -T kernel.ld        \
    -nostdlib           \
    --no-dynamic-linker \
    -z max-page-size=0x1000 \
    -Map=build/kernel.map

# --- Исходники ----------------------------------------------------------------
ASM_SRCS := \
    kernel/arch/x86_64/boot.asm        \
    kernel/arch/x86_64/gdt_flush.asm   \
    kernel/arch/x86_64/idt_flush.asm   \
    kernel/arch/x86_64/syscall_entry.asm \
    kernel/arch/x86_64/usermode.asm    \
    kernel/sched/context_switch.asm

C_SRCS := \
    kernel/kmain.c                     \
    kernel/arch/x86_64/gdt.c           \
    kernel/arch/x86_64/idt.c           \
    kernel/arch/x86_64/syscall.c       \
    kernel/arch/x86_64/tss.c           \
    kernel/drivers/vga.c               \
    kernel/drivers/keyboard.c          \
    kernel/drivers/mouse.c             \
    kernel/drivers/pit.c               \
    kernel/drivers/pci.c               \
    kernel/drivers/virtio_gpu.c        \
    kernel/drivers/ata.c               \
    kernel/drivers/compositor.c        \
    kernel/drivers/simple_wm.c         \
    kernel/mm/pmm.c                    \
    kernel/mm/vmm.c                    \
    kernel/mm/heap.c                   \
    kernel/fs/vfs.c                    \
    kernel/fs/ramfs.c                  \
    kernel/fs/fat32.c                  \
    kernel/fs/elf.c                    \
    kernel/fs/shell.c                  \
    kernel/ipc/ipc.c                   \
    kernel/sched/sched.c

ASM_OBJS := $(patsubst %.asm, build/%.o, $(ASM_SRCS))
C_OBJS   := $(patsubst %.c,   build/%.o, $(C_SRCS))
ALL_OBJS := $(ASM_OBJS) $(C_OBJS)

# --- Цели --------------------------------------------------------------------
.PHONY: all clean run iso disk disk-clean userspace disk-with-apps

all: build/kernel.bin
	@echo "=== Build OK: build/kernel.bin ==="

# --- Создаём образ диска с FAT32 -------------------------------------------
disk: build/disk.img

disk-clean:
	rm -f build/disk.img
	@echo "=== Disk image removed ==="

build/disk.img:
	@mkdir -p build
	@echo "=== Creating 16MB FAT32 disk image ==="
	dd if=/dev/zero of=build/disk.img bs=1M count=16
	python3 tools/mkfat32.py build/disk.img
	@echo "=== Adding test file to disk ==="
	python3 tools/add_file.py build/disk.img "Hello from FAT32!" test.txt
	@echo "=== Disk image ready: build/disk.img ==="

build/kernel.bin: $(ALL_OBJS)
	@echo "[LD]  $@"
	$(LD) $(LDFLAGS) -o $@ $^

build/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "[CC]  $<"
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.asm
	@mkdir -p $(dir $@)
	@echo "[AS]  $<"
	$(AS) $(ASFLAGS) $< -o $@

# --- ISO через Limine + xorriso ----------------------------------------------
iso: build/kernel.bin
	@mkdir -p build/iso/boot/limine
	@mkdir -p build/iso/EFI/BOOT
	cp build/kernel.bin        build/iso/boot/kernel.bin
	cp limine.conf             build/iso/boot/limine/limine.conf
	cp limine/limine-bios.sys  build/iso/boot/limine/
	cp limine/limine-bios-cd.bin build/iso/boot/limine/
	cp limine/limine-uefi-cd.bin build/iso/boot/limine/
	cp limine/BOOTX64.EFI      build/iso/EFI/BOOT/
	cp limine/BOOTIA32.EFI     build/iso/EFI/BOOT/
	$(XORRISO) -as mkisofs              \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot                   \
	    -boot-load-size 4               \
	    -boot-info-table                \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part                  \
	    --efi-boot-image                \
	    --protective-msdos-label        \
	    build/iso -o build/vortex.iso
	$(LIMINE)/limine.exe bios-install build/vortex.iso
	@echo "=== ISO ready: build/vortex.iso ==="

run: iso disk
	$(QEMU)                          \
	    -cdrom build/vortex.iso      \
	    -m 256M                      \
	    -serial stdio                \
	    -display sdl                 \
	    -machine pc                  \
	    -boot order=d                \
	    -drive file=build/disk.img,format=raw,if=ide,index=0 \
	    -no-reboot                   \
	    -no-shutdown

## Запуск с virtio-gpu (аппаратный present без разрывов).
## `-vga virtio` = virtio-vga: Limine всё ещё получает framebuffer для загрузки,
## а наш virtio_gpu-драйвер перехватывает scanout. Если что-то не так — просто
## запусти обычный `make run` (там virtio-gpu нет и драйвер сам отключается).
run-gpu: iso disk
	$(QEMU)                          \
	    -cdrom build/vortex.iso      \
	    -m 256M                      \
	    -serial stdio                \
	    -display sdl                 \
	    -machine pc                  \
	    -vga virtio                  \
	    -boot order=d                \
	    -drive file=build/disk.img,format=raw,if=ide,index=0 \
	    -no-reboot                   \
	    -no-shutdown

clean:
	rm -rf build/

userspace:
	@echo "=== Building userspace programs ==="
	$(MAKE) -C userspace

# ВАЖНО: пути назначения БЕЗ ведущего слэша. MSYS2 на Windows молча
# конвертирует аргументы вида "/bin/vwm" в Windows-пути ("C:/msys64/bin/vwm"),
# и файлы уезжали в мусорные каталоги внутри образа. add_file.py сам считает
# путь от корня FAT32.
disk-with-apps: disk userspace
	@echo "=== Adding userspace programs to /bin ==="
	python3 tools/add_file.py build/disk.img userspace/hello bin/hello
	python3 tools/add_file.py build/disk.img userspace/vortexgraph bin/vgraph
	python3 tools/add_file.py build/disk.img userspace/test_window bin/testwin
	python3 tools/add_file.py build/disk.img userspace/vsh bin/vsh
	python3 tools/add_file.py build/disk.img userspace/vwm bin/vwm
	python3 tools/add_file.py build/disk.img userspace/vterm bin/vterm
	python3 tools/add_file.py build/disk.img userspace/vdemo bin/vdemo
	python3 tools/add_file.py build/disk.img userspace/vfiles bin/vfiles
	@echo "=== Creating FS hierarchy (/etc, /home, /tmp) ==="
	python3 tools/add_file.py build/disk.img "Welcome to VortexOS!" etc/motd
	python3 tools/add_file.py build/disk.img --mkdir home
	python3 tools/add_file.py build/disk.img --mkdir tmp
	@echo "=== Disk with apps ready ==="
