TOOLCHAIN := /d/x86_64-elf-tools/bin
CC := $(TOOLCHAIN)/x86_64-elf-gcc
CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -nostdlib -fno-builtin -static -Wall -Wextra -O2
LDFLAGS := -nostdlib -static -Wl,--build-id=none -Wl,-T,linker.ld

all: hello vortexgraph test_window vsh vwm vterm vdemo vfiles

hello: hello.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) hello.c -o hello

vortexgraph: vortexgraph.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vortexgraph.c -o vortexgraph

test_window: test_window.c syscalls.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) test_window.c -o test_window

vsh: vsh.c syscalls.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vsh.c -o vsh

vwm: vwm.c vos_abi.h syscalls.h font8x16.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vwm.c -o vwm

vterm: vterm.c vos_abi.h syscalls.h font8x16.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vterm.c -o vterm

vdemo: vdemo.c vos_abi.h syscalls.h font8x16.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vdemo.c -o vdemo

vfiles: vfiles.c vos_abi.h syscalls.h font8x16.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vfiles.c -o vfiles

clean:
	rm -f hello vortexgraph test_window vsh vwm vterm vdemo vfiles

.PHONY: all clean
