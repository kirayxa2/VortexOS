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
    kernel/lib/string.c                \
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
    kernel/drivers/serial.c            \
    kernel/drivers/ata.c               \
    kernel/drivers/compositor.c        \
    kernel/drivers/simple_wm.c         \
    kernel/mm/pmm.c                    \
    kernel/mm/vmm.c                    \
    kernel/mm/heap.c                   \
    kernel/fs/vfs.c                    \
    kernel/fs/ramfs.c                  \
    kernel/fs/fat32.c                  \
    kernel/fs/vortexfs.c               \
    kernel/fs/elf.c                    \
    kernel/fs/shell.c                  \
    kernel/ipc/ipc.c                   \
    kernel/sched/sched.c

ASM_OBJS := $(patsubst %.asm, build/%.o, $(ASM_SRCS))
C_OBJS   := $(patsubst %.c,   build/%.o, $(C_SRCS))
ALL_OBJS := $(ASM_OBJS) $(C_OBJS)

# --- Цели --------------------------------------------------------------------
.PHONY: all clean run run-std run-gpu run-gpu-whpx iso disk disk-clean vortexfs-disk vortexfs-disk-clean userspace disk-with-apps vortexfs-with-apps

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

# --- VortexFS disk image (second ATA drive) --------------------------------
vortexfs-disk: build/vortexfs.img

vortexfs-disk-clean:
	rm -f build/vortexfs.img
	@echo "=== VortexFS image removed ==="

build/vortexfs.img:
	@mkdir -p build
	@echo "=== Creating 16MB VortexFS disk image ==="
	python3 tools/mkvortexfs.py build/vortexfs.img 16
	@echo "=== VortexFS image ready: build/vortexfs.img ==="

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

## VortexFS — основной диск (drive 0), FAT32 — вторичный (drive 1, опционально).
## Ядро пробует VortexFS первым → FAT32 → ramfs.
## ДЕФОЛТ = virtio-gpu. Причина: с -vga std QEMU сам сканирует framebuffer на
## изменения по GUI-таймеру (~30 мс) => на хосте максимум ~33 FPS, перетаскивание
## дёргается, сколько бы FPS ни выдавал vwm. С virtio-gpu каждый present
## (TRANSFER+FLUSH) показывается хостом немедленно => плавность = FPS гостя.
run: run-gpu

## Старый путь со стандартной VGA (фоллбэк, если virtio вдруг сломается).
run-std: iso vortexfs-disk
	$(QEMU)                          \
	    -cdrom build/vortex.iso      \
	    -m 256M                      \
	    -serial stdio                \
	    -display sdl                 \
	    -machine pc                  \
	    -boot order=d                \
	    -drive file=build/vortexfs.img,format=raw,if=ide,index=0 \
	    -no-reboot                   \
	    -no-shutdown

## Запуск с virtio-gpu (аппаратный present без разрывов).
## ВНИМАНИЕ: НЕ добавлять сюда -accel whpx! Проверено 2026-06-11: под WHPX
## (kernel-irqchip=off) у гостя умирает доставка legacy-IRQ (PIT/PS2/ATA через
## PIC) => мышь/часы/vpanel мертвы, хотя первый кадр vwm рисуется. Для
## экспериментов есть отдельный target run-gpu-whpx.
run-gpu: iso vortexfs-disk
	$(QEMU)                          \
	    -cdrom build/vortex.iso      \
	    -m 256M                      \
	    -serial stdio                \
	    -display sdl                 \
	    -machine pc                  \
	    -vga virtio                  \
	    -boot order=d                \
	    -drive file=build/vortexfs.img,format=raw,if=ide,index=0 \
	    -no-reboot                   \
	    -no-shutdown

## ЭКСПЕРИМЕНТ: virtio-gpu + аппаратная виртуализация WHPX. Быстрее в разы,
## но 2026-06-11 на машине Джона под WHPX зависала доставка legacy-IRQ
## (мышь/PIT/ATA). Не использовать как дефолт, пока не разберёмся.
run-gpu-whpx: iso vortexfs-disk
	$(QEMU)                          \
	    -cdrom build/vortex.iso      \
	    -m 256M                      \
	    -serial stdio                \
	    -display sdl                 \
	    -machine pc                  \
	    -accel whpx,kernel-irqchip=off \
	    -accel tcg                   \
	    -vga virtio                  \
	    -boot order=d                \
	    -drive file=build/vortexfs.img,format=raw,if=ide,index=0 \
	    -no-reboot                   \
	    -no-shutdown

## Legacy: запуск с FAT32 как root (старое поведение).
## Если VortexFS не найден на drive 0, ядро откатится на FAT32.
run-fat32: iso disk
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
	@echo "=== Adding userspace programs to FAT32 /bin ==="
	python3 tools/add_file.py build/disk.img userspace/hello bin/hello
	python3 tools/add_file.py build/disk.img userspace/vortexgraph bin/vgraph
	python3 tools/add_file.py build/disk.img userspace/test_window bin/testwin
	python3 tools/add_file.py build/disk.img userspace/vsh bin/vsh
	python3 tools/add_file.py build/disk.img userspace/vwm bin/vwm
	python3 tools/add_file.py build/disk.img userspace/vterm bin/vterm
	python3 tools/add_file.py build/disk.img userspace/vdemo bin/vdemo
	python3 tools/add_file.py build/disk.img userspace/vfiles bin/vfiles
	python3 tools/add_file.py build/disk.img userspace/vuidemo bin/vuidemo
	python3 tools/add_file.py build/disk.img userspace/vsettings bin/vsettings
	python3 tools/add_file.py build/disk.img userspace/vpanel bin/vpanel
	python3 tools/add_file.py build/disk.img userspace/vinit bin/vinit
	@echo "=== Adding /bin utilities (ls, cat, rm, find, ...) ==="
	python3 tools/add_file.py build/disk.img userspace/bin/vctl bin/vctl
	python3 tools/add_file.py build/disk.img userspace/bin/ls bin/ls
	python3 tools/add_file.py build/disk.img userspace/bin/cat bin/cat
	python3 tools/add_file.py build/disk.img userspace/bin/rm bin/rm
	python3 tools/add_file.py build/disk.img userspace/bin/find bin/find
	python3 tools/add_file.py build/disk.img userspace/bin/mkdir bin/mkdir
	python3 tools/add_file.py build/disk.img userspace/bin/touch bin/touch
	python3 tools/add_file.py build/disk.img userspace/bin/cp bin/cp
	python3 tools/add_file.py build/disk.img userspace/bin/mv bin/mv
	python3 tools/add_file.py build/disk.img userspace/bin/echo bin/echo
	python3 tools/add_file.py build/disk.img userspace/bin/pwd bin/pwd
	python3 tools/add_file.py build/disk.img userspace/bin/stat bin/stat
	python3 tools/add_file.py build/disk.img userspace/bin/head bin/head
	python3 tools/add_file.py build/disk.img userspace/bin/wc bin/wc
	python3 tools/add_file.py build/disk.img userspace/bin/chmod bin/chmod
	python3 tools/add_file.py build/disk.img userspace/bin/chown bin/chown
	python3 tools/add_file.py build/disk.img userspace/bin/whoami bin/whoami
	@echo "=== Creating FS hierarchy (/etc, /home, /tmp) ==="
	python3 tools/add_file.py build/disk.img "Welcome to VortexOS!" etc/motd
	python3 tools/add_file.py build/disk.img --mkdir home
	python3 tools/add_file.py build/disk.img --mkdir tmp
	@echo "=== Adding vinit service configs (/etc/vinit) ==="
	python3 tools/add_file.py build/disk.img userspace/etc/vinit/10-vwm.svc etc/vinit/10-vwm.svc
	python3 tools/add_file.py build/disk.img userspace/etc/vinit/20-panel.svc etc/vinit/20-panel.svc
	@echo "=== FAT32 disk with apps ready ==="

# --- VortexFS disk с приложениями (заменяет FAT32 как корневую FS) ----------
# Теперь VortexFS — основной образ диска (drive 0). FAT32 остаётся как
# fallback, но `make vortexfs-with-apps` + `make run` = VortexFS root.
vortexfs-with-apps: vortexfs-disk userspace
	@echo "=== Adding userspace programs to VortexFS /bin ==="
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/hello bin/hello
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vortexgraph bin/vgraph
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/test_window bin/testwin
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vsh bin/vsh
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vwm bin/vwm
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vterm bin/vterm
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vdemo bin/vdemo
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vfiles bin/vfiles
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vuidemo bin/vuidemo
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vsettings bin/vsettings
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vpanel bin/vpanel
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/vinit bin/vinit
	@echo "=== Adding /bin utilities (ls, cat, rm, find, ...) ==="
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/vctl bin/vctl
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/ls bin/ls
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/cat bin/cat
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/rm bin/rm
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/find bin/find
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/mkdir bin/mkdir
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/touch bin/touch
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/cp bin/cp
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/mv bin/mv
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/echo bin/echo
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/pwd bin/pwd
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/stat bin/stat
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/head bin/head
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/wc bin/wc
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/chmod bin/chmod
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/chown bin/chown
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/bin/whoami bin/whoami
	@echo "=== Creating FS hierarchy (/etc, /home, /tmp) ==="
	python3 tools/add_vortexfs_file.py build/vortexfs.img "Welcome to VortexOS!" etc/motd
	python3 tools/add_vortexfs_file.py build/vortexfs.img --mkdir home
	python3 tools/add_vortexfs_file.py build/vortexfs.img --mkdir tmp
	@echo "=== Adding vinit service configs (/etc/vinit) ==="
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/etc/vinit/10-vwm.svc etc/vinit/10-vwm.svc
	python3 tools/add_vortexfs_file.py build/vortexfs.img userspace/etc/vinit/20-panel.svc etc/vinit/20-panel.svc
	@echo "=== VortexFS disk with apps ready ==="
