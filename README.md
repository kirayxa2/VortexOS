# VortexOS

A hobby operating system for x86_64, written in C and Assembly.

VortexOS has its own kernel, filesystem, userspace programs and graphical desktop. It boots with Limine and is developed on Windows using MSYS2 and QEMU.

Work in progress. Expect bugs, unfinished features and things that worked yesterday.

## Current state

- 64-bit kernel with physical and virtual memory management.
- Kernel heap, interrupts, exceptions and system calls.
- Scheduling, ring 3 userspace, IPC and shared memory.
- VFS with VortexFS, FAT32 and ramfs.
- ELF program loading.
- Keyboard, mouse, timer, serial, ATA and PCI drivers.
- VirtIO GPU support and an experimental VirGL rendering path.
- Userspace window manager and compositor.
- Login screen, panel, application menu and desktop applications.

The graphical desktop runs in userspace. Applications share their window surfaces with `vwm` through shared memory and communicate through IPC.

## Applications

| Program | Purpose |
| --- | --- |
| `vinit` | Starts userspace services |
| `vlogin` | Login screen |
| `vwm` | Window manager and compositor |
| `vpanel` | Desktop panel |
| `vmenu` | Application menu |
| `vterm` | Terminal |
| `vfiles` | File manager |
| `vsettings` | Settings |
| `vedit` | Text editor |
| `vcalc` | Calculator |

Userspace also includes basic command-line tools such as `ls`, `cat`, `cp`, `mv`, `rm`, `find`, `mkdir`, `head` and `wc`.

## Build environment

The current Makefile targets **Windows + MSYS2 UCRT64**.

You need:

- MSYS2
- An `x86_64-elf` GCC/binutils cross-toolchain
- NASM
- GNU Make
- Python 3
- xorriso
- QEMU with SDL display support
- Limine binaries in `limine/`

### Install MSYS2 packages

Open the **MSYS2 UCRT64** terminal, not PowerShell or Command Prompt.

Update MSYS2:

```bash
pacman -Syu
```

If asked to close the terminal, reopen UCRT64 and run the update again.

Install the build tools:

```bash
pacman -S --needed \
    make \
    nasm \
    python \
    xorriso \
    git
```

Check that they are available:

```bash
make --version
nasm -v
python3 --version
xorriso -version
```

These packages do not install the `x86_64-elf` cross-toolchain or the external QEMU build used by this project.

### Toolchain and QEMU paths

The current Makefile uses:

```make
TOOLCHAIN := /d/x86_64-elf-tools/bin
QEMU      := /d/qemu/qemu-system-x86_64.exe
```

That corresponds to these Windows paths:

```text
D:\x86_64-elf-tools\bin\
D:\qemu\qemu-system-x86_64.exe
```

Check the tools:

```bash
/d/x86_64-elf-tools/bin/x86_64-elf-gcc --version
/d/x86_64-elf-tools/bin/x86_64-elf-ld --version
/d/qemu/qemu-system-x86_64.exe --version
```

If your installation uses different paths, update the Makefile. Check `userspace/Makefile` for toolchain paths as well.

The regular Windows compiler is not a replacement for `x86_64-elf-gcc`.

### Limine

The ISO target expects these files:

```text
limine/
├── limine.exe
├── limine-bios.sys
├── limine-bios-cd.bin
├── limine-uefi-cd.bin
├── BOOTX64.EFI
└── BOOTIA32.EFI
```

Boot configuration is stored in `limine.conf`.

## Build and run

From the project directory in UCRT64:

```bash
cd /d/VOS

# Build the kernel.
make

# Build userspace and add applications, service configs
# and fonts to the VortexFS image.
make vortexfs-with-apps

# Build the ISO and start QEMU.
make run
```

Adjust `/d/VOS` if the project is stored elsewhere.

### Build targets

| Command | Action |
| --- | --- |
| `make` | Build the kernel |
| `make userspace` | Build userspace programs |
| `make iso` | Create the bootable ISO |
| `make vortexfs-with-apps` | Build and copy userspace into VortexFS |
| `make run` | Start QEMU using the default VirtIO GPU path |
| `make run-std` | Start QEMU with standard VGA |
| `make run-gl-desktop` | Start the experimental VirGL desktop |
| `make disk-with-apps` | Populate the alternative FAT32 image |

After changing a userspace application, run `make vortexfs-with-apps` before using the regular `make run` path.

### Experimental VirGL desktop

Requires a QEMU build with `virtio-vga-gl` and SDL/OpenGL support, plus an already populated VortexFS image:

```bash
make vortexfs-with-apps
make run-gl-desktop
```

This target rebuilds `vwm` and `vlogin`, places them in a separate desktop test image and runs QEMU with a temporary snapshot overlay. Guest writes are not saved back to the image.

The GL test image is reused between runs. Rebuilding the main VortexFS image does not automatically refresh every file in that test copy.

### Build output

```text
build/
├── kernel.bin              # Kernel ELF binary
├── kernel.map              # Linker map
├── vortex.iso              # Bootable ISO
├── vortexfs.img            # Main VortexFS image
├── vortexfs-gl-test.img     # VirGL test copy, created when needed
├── vortexfs-desktop.img     # VirGL desktop test image
└── disk.img                # Optional FAT32 image
```

**Warning:** `make clean` deletes the entire `build/` directory, including guest disk images and anything saved inside them. Back up images before cleaning.

## Source layout

```text
VortexOS/
├── kernel/
│   ├── arch/x86_64/        # CPU setup, interrupts, syscalls and ring 3 entry
│   ├── drivers/            # Hardware drivers and graphics support
│   ├── fs/                 # VFS, VortexFS, FAT32, ramfs and ELF loading
│   ├── include/            # Shared kernel types and Limine definitions
│   ├── ipc/                # Inter-process communication
│   ├── lib/                # Kernel utility functions
│   ├── mm/                 # Physical memory, virtual memory and heap
│   ├── sched/              # Scheduler and context switching
│   ├── boot_log.h          # Boot status display
│   └── kmain.c             # Kernel startup
├── userspace/
│   ├── bin/                # Command-line utilities
│   ├── etc/
│   │   ├── fonts/          # Desktop fonts
│   │   └── vinit/          # Service definitions
│   ├── libc/               # Userspace C runtime and library
│   ├── libvui/             # UI library
│   ├── vos_abi.h           # Kernel/userspace interface
│   ├── syscalls.h          # Userspace syscall wrappers
│   ├── vwm.c               # Window manager and compositor
│   ├── vwm_gpu.h           # GPU rendering support
│   ├── vwm_boot.h          # Desktop startup visuals
│   ├── vwm_perf.h          # Performance instrumentation
│   ├── vinit.c
│   ├── vlogin.c
│   ├── vpanel.c
│   ├── vmenu.c
│   ├── vterm.c
│   ├── vfiles.c
│   ├── vsettings.c
│   ├── vedit.c
│   ├── vcalc.c
│   └── Makefile
├── tools/                  # Image tools, verification and QEMU experiments
├── docs/                   # Project documentation
├── limine/                 # Bootloader files
├── limine.conf             # Boot configuration
├── kernel.ld               # Kernel linker script
└── Makefile
```

The tree above focuses on the OS sources. Local checkpoints, downloaded reference projects and experimental QEMU build directories are omitted.

## Notes

- QEMU is the main development target.
- The default run target uses 256 MiB of guest RAM.
- WHPX is experimental and is not the default because of observed legacy interrupt delivery problems.
- VirtIO GPU and VirGL are separate rendering paths with different QEMU requirements.
- This is not a daily-use operating system.
