TOOLCHAIN := /d/x86_64-elf-tools/bin
CC := $(TOOLCHAIN)/x86_64-elf-gcc
CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -nostdlib -fno-builtin -static -Wall -Wextra -O2
LDFLAGS := -nostdlib -static -Wl,--build-id=none -Wl,-T,linker.ld

all: hello vortexgraph test_window

hello: hello.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) hello.c -o hello

vortexgraph: vortexgraph.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vortexgraph.c -o vortexgraph

test_window: test_window.c syscalls.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) test_window.c -o test_window

clean:
	rm -f hello vortexgraph test_window

.PHONY: all clean
