OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(_start)

SECTIONS {
    . = 0xffffffff80000000;

    _kernel_start = .;

    .text : {
        *(.text .text.*)
    }

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    }

    /* Limine requests — KEEP чтобы линкер не выбросил */
    .requests : ALIGN(8) {
        KEEP(*(.requests_start))
        KEEP(*(.requests))
        KEEP(*(.requests_end))
    }

    .data : ALIGN(4096) {
        *(.data .data.*)
    }

    .bss : ALIGN(4096) {
        *(COMMON)
        *(.bss .bss.*)
    }

    _kernel_end = ALIGN(4096);

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
