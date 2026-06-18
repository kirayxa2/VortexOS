TOOLCHAIN := /d/x86_64-elf-tools/bin
CC := $(TOOLCHAIN)/x86_64-elf-gcc
CFLAGS := -std=c11 -ffreestanding -fno-stack-protector -nostdlib -fno-builtin -static -Wall -Wextra -O2
LDFLAGS := -nostdlib -static -Wl,--build-id=none -Wl,-T,linker.ld

# --- libc + libvui -----------------------------------------------------------
# Приложения на libc собираются с -D__VLIBC__ (выключает inline puts/exit в
# syscalls.h) и линкуются с объектами libc (crt0 даёт _start -> main()).
LIBC_CFLAGS := $(CFLAGS) -D__VLIBC__ -Ilibc/include -I. -Ilibvui
LIBC_OBJS   := libc/crt0.o libc/string.o libc/malloc.o libc/stdio.o libc/stdlib.o
LIBC_HDRS   := $(wildcard libc/include/*.h) syscalls.h vos_abi.h

# --- утилиты /bin (libc-приложения: crt0 разбирает argv из SYS_GETARGS) ------
UTILS := ls cat rm find mkdir touch cp mv echo pwd stat head wc vctl chmod chown whoami
UTIL_BINS := $(addprefix bin/,$(UTILS))

all: vwm vterm vfiles vsettings vpanel vinit vlogin vcalc vedit vmenu $(UTIL_BINS)

libc/%.o: libc/src/%.c $(LIBC_HDRS)
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

libvui/vui.o: libvui/vui.c libvui/vui.h font8x16.h vfont.h vfont_ui.h stb_truetype.h $(LIBC_HDRS)
	$(CC) $(LIBC_CFLAGS) -c $< -o $@

# --- libc + libvui приложения ---
vsettings: vsettings.c $(LIBC_OBJS) libvui/vui.o linker.ld
	$(CC) $(LIBC_CFLAGS) $(LDFLAGS) vsettings.c $(LIBC_OBJS) libvui/vui.o -o vsettings

vcalc: vcalc.c $(LIBC_OBJS) libvui/vui.o linker.ld
	$(CC) $(LIBC_CFLAGS) $(LDFLAGS) vcalc.c $(LIBC_OBJS) libvui/vui.o -o vcalc

vedit: vedit.c $(LIBC_OBJS) libvui/vui.o linker.ld
	$(CC) $(LIBC_CFLAGS) $(LDFLAGS) vedit.c $(LIBC_OBJS) libvui/vui.o -o vedit

vmenu: vmenu.c $(LIBC_OBJS) libvui/vui.o linker.ld
	$(CC) $(LIBC_CFLAGS) $(LDFLAGS) vmenu.c $(LIBC_OBJS) libvui/vui.o -o vmenu

vinit: vinit.c $(LIBC_OBJS) linker.ld
	$(CC) $(LIBC_CFLAGS) $(LDFLAGS) vinit.c $(LIBC_OBJS) -o vinit

# --- standalone (без libc) ---
vlogin: vlogin.c vos_abi.h syscalls.h font8x16.h vfont.h vfont_ui.h stb_truetype.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vlogin.c -o vlogin

vwm: vwm.c vos_abi.h syscalls.h font8x16.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vwm.c -o vwm

vterm: vterm.c vos_abi.h syscalls.h font8x16.h vfont.h stb_truetype.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vterm.c -o vterm

vfiles: vfiles.c vos_abi.h syscalls.h font8x16.h vfont.h vfont_ui.h stb_truetype.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vfiles.c -o vfiles

vpanel: vpanel.c vos_abi.h syscalls.h font8x16.h vfont.h vfont_ui.h stb_truetype.h linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) vpanel.c -o vpanel

bin/%: bin/%.c bin/vutil.h $(LIBC_OBJS) linker.ld
	$(CC) $(LIBC_CFLAGS) $(LDFLAGS) $< $(LIBC_OBJS) -o $@

clean:
	rm -f vwm vterm vfiles vsettings vpanel vinit vlogin vcalc vedit vmenu
	rm -f $(UTIL_BINS)
	rm -f libc/*.o libvui/*.o

.PHONY: all clean
