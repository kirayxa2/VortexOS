/* =============================================================================
 * VortexOS — kernel/kmain.c
 * ============================================================================= */

#include "types.h"
#include "gdt.h"
#include "idt.h"
#include "vga.h"
#include "fb.h"
#include "keyboard.h"
#include "mouse.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "limine.h"
#include "pit.h"
#include "sched.h"
#include "pci.h"
#include "virtio_gpu.h"
#include "ata.h"
#include "syscall.h"
#include "vfs.h"
#include "ramfs.h"
#include "fat32.h"
#include "shell.h"
#include "elf.h"

extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

/* --- Limine requests ---------------------------------------------------- */

__attribute__((used, section(".requests_start")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request mmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

__attribute__((used, section(".requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* --- Framebuffer -------------------------------------------------------- */

uint32_t *fb_addr  = 0;  /* Глобально доступен для compositor */
uint64_t  fb_pitch = 0;  /* Глобально доступен для compositor */
uint32_t  fb_width = 0;  /* Реальная ширина framebuffer */
uint32_t  fb_height = 0; /* Реальная высота framebuffer */
static uint32_t  cur_x    = 0;
static uint32_t  cur_y    = 0;

#define FONT_W 8
#define FONT_H 16
#define FG 0xE0E0E0
#define BG 0x1A1A2E

/* Встроенный 8x16 шрифт (подмножество CP437) - глобально доступен */
const uint8_t font[128][16] = {
    [' '] ={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    ['!'] ={0,0,0x18,0x18,0x18,0x18,0x18,0x18,0,0x18,0,0,0,0,0,0},
    ['"'] ={0,0,0x6C,0x6C,0x6C,0,0,0,0,0,0,0,0,0,0,0},
    ['#'] ={0,0,0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0,0,0,0,0,0,0},
    ['%'] ={0,0,0x60,0x66,0x0C,0x18,0x30,0x66,0x06,0,0,0,0,0,0,0},
    ['&'] ={0,0,0x38,0x6C,0x68,0x76,0xDC,0xCE,0x6C,0x3A,0,0,0,0,0,0},
    ['\'']={0,0,0x18,0x18,0x18,0,0,0,0,0,0,0,0,0,0,0},
    ['('] ={0,0,0x0C,0x18,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0,0,0},
    [')'] ={0,0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0,0,0},
    ['*'] ={0,0,0,0x6C,0x38,0xFE,0x38,0x6C,0,0,0,0,0,0,0,0},
    ['+'] ={0,0,0,0x18,0x18,0x7E,0x18,0x18,0,0,0,0,0,0,0,0},
    [','] ={0,0,0,0,0,0,0,0,0x18,0x18,0x30,0,0,0,0,0},
    ['-'] ={0,0,0,0,0,0x7E,0,0,0,0,0,0,0,0,0,0},
    ['.'] ={0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0,0,0},
    ['/'] ={0,0,0x06,0x06,0x0C,0x18,0x30,0x60,0x60,0,0,0,0,0,0,0},
    ['0'] ={0,0,0x3C,0x66,0x6E,0x7E,0x76,0x66,0x66,0x3C,0,0,0,0,0,0},
    ['1'] ={0,0,0x18,0x38,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0,0},
    ['2'] ={0,0,0x3C,0x66,0x06,0x0C,0x18,0x30,0x66,0x7E,0,0,0,0,0,0},
    ['3'] ={0,0,0x3C,0x66,0x06,0x1C,0x06,0x06,0x66,0x3C,0,0,0,0,0,0},
    ['4'] ={0,0,0x0C,0x1C,0x3C,0x6C,0x6C,0x7E,0x0C,0x0C,0,0,0,0,0,0},
    ['5'] ={0,0,0x7E,0x60,0x60,0x7C,0x06,0x06,0x66,0x3C,0,0,0,0,0,0},
    ['6'] ={0,0,0x1C,0x30,0x60,0x7C,0x66,0x66,0x66,0x3C,0,0,0,0,0,0},
    ['7'] ={0,0,0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0,0,0,0,0,0},
    ['8'] ={0,0,0x3C,0x66,0x66,0x3C,0x66,0x66,0x66,0x3C,0,0,0,0,0,0},
    ['9'] ={0,0,0x3C,0x66,0x66,0x66,0x3E,0x06,0x0C,0x38,0,0,0,0,0,0},
    [':'] ={0,0,0,0x18,0x18,0,0,0x18,0x18,0,0,0,0,0,0,0},
    [';'] ={0,0,0,0x18,0x18,0,0,0x18,0x18,0x30,0,0,0,0,0,0},
    ['<'] ={0,0,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0,0,0,0,0,0,0},
    ['='] ={0,0,0,0,0x7E,0,0,0x7E,0,0,0,0,0,0,0,0},
    ['>'] ={0,0,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0,0,0,0,0,0,0},
    ['?'] ={0,0,0x3C,0x66,0x06,0x0C,0x18,0,0x18,0,0,0,0,0,0,0},
    ['@'] ={0,0,0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x66,0x3C,0,0,0,0,0,0},
    ['A'] ={0,0,0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x66,0,0,0,0,0,0},
    ['B'] ={0,0,0x7C,0x66,0x66,0x7C,0x66,0x66,0x66,0x7C,0,0,0,0,0,0},
    ['C'] ={0,0,0x3C,0x66,0x60,0x60,0x60,0x60,0x66,0x3C,0,0,0,0,0,0},
    ['D'] ={0,0,0x78,0x6C,0x66,0x66,0x66,0x66,0x6C,0x78,0,0,0,0,0,0},
    ['E'] ={0,0,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x7E,0,0,0,0,0,0},
    ['F'] ={0,0,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0,0,0,0,0,0},
    ['G'] ={0,0,0x3C,0x66,0x60,0x60,0x6E,0x66,0x66,0x3C,0,0,0,0,0,0},
    ['H'] ={0,0,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0,0,0,0,0,0},
    ['I'] ={0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},
    ['J'] ={0,0,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0,0,0,0,0,0},
    ['K'] ={0,0,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x66,0,0,0,0,0,0},
    ['L'] ={0,0,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0,0,0,0,0,0},
    ['M'] ={0,0,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x63,0,0,0,0,0,0},
    ['N'] ={0,0,0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x66,0,0,0,0,0,0},
    ['O'] ={0,0,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0,0},
    ['P'] ={0,0,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0,0,0,0,0,0},
    ['Q'] ={0,0,0x3C,0x66,0x66,0x66,0x66,0x6E,0x3C,0x06,0,0,0,0,0,0},
    ['R'] ={0,0,0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0,0,0,0,0,0},
    ['S'] ={0,0,0x3C,0x66,0x60,0x3C,0x06,0x06,0x66,0x3C,0,0,0,0,0,0},
    ['T'] ={0,0,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0,0},
    ['U'] ={0,0,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0,0},
    ['V'] ={0,0,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0,0,0,0,0,0},
    ['W'] ={0,0,0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x63,0,0,0,0,0,0},
    ['X'] ={0,0,0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x66,0,0,0,0,0,0},
    ['Y'] ={0,0,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0,0,0,0,0,0},
    ['Z'] ={0,0,0x7E,0x06,0x0C,0x18,0x30,0x60,0x60,0x7E,0,0,0,0,0,0},
    ['['] ={0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0,0,0},
    ['\\']={0,0,0x60,0x60,0x30,0x18,0x0C,0x06,0x06,0,0,0,0,0,0,0},
    [']'] ={0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0,0,0},
    ['_'] ={0,0,0,0,0,0,0,0,0,0,0xFF,0,0,0,0,0},
    ['a'] ={0,0,0,0,0x3C,0x06,0x3E,0x66,0x66,0x3E,0,0,0,0,0,0},
    ['b'] ={0,0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0,0,0,0,0,0},
    ['c'] ={0,0,0,0,0x3C,0x66,0x60,0x60,0x66,0x3C,0,0,0,0,0,0},
    ['d'] ={0,0,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x3E,0,0,0,0,0,0},
    ['e'] ={0,0,0,0,0x3C,0x66,0x7E,0x60,0x66,0x3C,0,0,0,0,0,0},
    ['f'] ={0,0,0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x30,0,0,0,0,0,0},
    ['g'] ={0,0,0,0,0x3E,0x66,0x66,0x3E,0x06,0x06,0x3C,0,0,0,0,0},
    ['h'] ={0,0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0,0,0,0,0,0},
    ['i'] ={0,0,0x18,0,0x38,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},
    ['j'] ={0,0,0x06,0,0x06,0x06,0x06,0x06,0x06,0x06,0x3C,0,0,0,0,0},
    ['k'] ={0,0,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x66,0,0,0,0,0,0},
    ['l'] ={0,0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0,0},
    ['m'] ={0,0,0,0,0x66,0x7F,0x7F,0x6B,0x63,0x63,0,0,0,0,0,0},
    ['n'] ={0,0,0,0,0x7C,0x66,0x66,0x66,0x66,0x66,0,0,0,0,0,0},
    ['o'] ={0,0,0,0,0x3C,0x66,0x66,0x66,0x66,0x3C,0,0,0,0,0,0},
    ['p'] ={0,0,0,0,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0,0,0,0,0},
    ['q'] ={0,0,0,0,0x3E,0x66,0x66,0x3E,0x06,0x06,0x06,0,0,0,0,0},
    ['r'] ={0,0,0,0,0x6C,0x76,0x60,0x60,0x60,0x60,0,0,0,0,0,0},
    ['s'] ={0,0,0,0,0x3C,0x66,0x30,0x0C,0x66,0x3C,0,0,0,0,0,0},
    ['t'] ={0,0,0x18,0x18,0x7E,0x18,0x18,0x18,0x18,0x0E,0,0,0,0,0,0},
    ['u'] ={0,0,0,0,0x66,0x66,0x66,0x66,0x66,0x3E,0,0,0,0,0,0},
    ['v'] ={0,0,0,0,0x66,0x66,0x66,0x66,0x3C,0x18,0,0,0,0,0,0},
    ['w'] ={0,0,0,0,0x63,0x63,0x6B,0x7F,0x77,0x63,0,0,0,0,0,0},
    ['x'] ={0,0,0,0,0x66,0x66,0x3C,0x3C,0x66,0x66,0,0,0,0,0,0},
    ['y'] ={0,0,0,0,0x66,0x66,0x66,0x3E,0x06,0x06,0x3C,0,0,0,0,0},
    ['z'] ={0,0,0,0,0x7E,0x0C,0x18,0x30,0x60,0x7E,0,0,0,0,0,0},
    ['{'] ={0,0,0x0E,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0,0,0,0,0,0},
    ['|'] ={0,0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0,0,0,0,0},
    ['}'] ={0,0,0x70,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0,0,0,0,0,0},
    ['~'] ={0,0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0},
};

void fb_scroll(void) {
    if (!fb_addr) return;
    uint32_t rows      = 768 / FONT_H;
    uint32_t row_px    = FONT_H * (fb_pitch / 4);
    for (uint32_t i = 0; i < (rows - 1) * row_px; i++)
        fb_addr[i] = fb_addr[i + row_px];
    for (uint32_t i = (rows - 1) * row_px; i < rows * row_px; i++)
        fb_addr[i] = BG;
}

void fb_putchar(char c) {
    if (!fb_addr) return;
    if (c == '\n') {
        cur_x = 0; cur_y++;
        if (cur_y >= 768 / FONT_H) { fb_scroll(); cur_y--; }
        return;
    }
    uint8_t idx = (uint8_t)c;
    if (idx >= 128) idx = '?';
    const uint8_t *glyph = font[idx];
    uint32_t px = cur_x * FONT_W;
    uint32_t py = cur_y * FONT_H;
    for (uint32_t row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_W; col++)
            fb_addr[(py + row) * (fb_pitch / 4) + px + col] =
                (bits & (0x80 >> col)) ? FG : BG;
    }
    cur_x++;
    if (cur_x >= 1024 / FONT_W) { cur_x = 0; cur_y++; }
    if (cur_y >= 768 / FONT_H)  { fb_scroll(); cur_y--; }
}

void fb_puts(const char *s) {
    while (*s) fb_putchar(*s++);
}

void fb_puthex(uint64_t val) {
    fb_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        int digit = (val >> shift) & 0xF;
        fb_putchar(digit < 10 ? '0' + digit : 'a' + digit - 10);
    }
}

/* --- Демо задачи -------------------------------------------------------- */

/* Простой счётчик тиков из pit.c */
extern uint64_t pit_ticks(void);

void task_a(void);
void task_b(void);
void userspace_hello_task(void);

void task_a(void) {
    uint64_t counter = 0;
    for (;;) {
        fb_puts("[TASK_A] Running #");
        fb_puthex(counter++);
        fb_putchar('\n');
        
        // Sleep через busy-wait
        uint64_t start = pit_ticks();
        while (pit_ticks() - start < 50) __asm__ volatile("pause"); // ~500ms
    }
}

void task_b(void) {
    uint64_t counter = 0;
    for (;;) {
        fb_puts("[TASK_B] Running #");
        fb_puthex(counter++);
        fb_putchar('\n');
        
        // Sleep через busy-wait
        uint64_t start = pit_ticks();
        while (pit_ticks() - start < 50) __asm__ volatile("pause"); // ~500ms
    }
}

void userspace_elf_loader_task(void) {
    extern void enter_usermode(uint64_t entry_point, uint64_t user_stack);
    extern void *kmalloc_aligned(uint64_t size, uint64_t align);
    extern uint64_t hhdm_offset;  // From vmm.c
    uint64_t hhdm_off = hhdm_offset;
    
    /* Читаем параметр — путь к ELF файлу (через userdata) */
    task_t *current = sched_current();
    if (!current || !current->userdata) {
        fb_puts("[TASK] No ELF path specified\n");
        task_exit();
        return;
    }
    
    const char *elf_path = (const char *)current->userdata;
    fb_puts("[TASK] Loading ELF: ");
    fb_puts(elf_path);
    fb_puts("...\n");
    
    vfs_node_t *elf_node = vfs_open(elf_path, 0);
    if (!elf_node) {
        fb_puts("[TASK] File not found\n");
        task_exit();
        return;
    }
    
    vfs_close(elf_node);
    
    elf_load_result_t elf_result = elf_load(elf_path);
    if (!elf_result.entry_point || !elf_result.user_pml4) {
        fb_puts("[TASK] ELF load failed\n");
        task_exit();
        return;
    }
    
    fb_puts("[TASK] ELF loaded, entry=");
    fb_puthex(elf_result.entry_point);
    fb_putchar('\n');
    
    /* Стек уже замапен в elf_load() */
    uint64_t user_stack_top = 0x800000 + 16384;

    /* Запоминаем адресное пространство процесса В ЗАДАЧЕ — теперь планировщик
     * будет грузить именно его CR3 при каждом свитче на эту задачу (раньше CR3
     * выставлялся один раз тут и не переключался → только 1 usermode-процесс). */
    task_t *self = sched_current();
    if (self) self->pml4 = (void *)elf_result.user_pml4;

    /* Kernel-стек для ловушек/syscall — СОБСТВЕННЫЙ стек этой задачи (тот же,
     * что планировщик ставит в TSS.rsp0/syscall_kernel_stack на свитче). Так у
     * каждого процесса свой kernel-стек, и они не топчут друг друга. */
    if (self && self->stack) {
        uint64_t kstk_top = (uint64_t)(self->stack + TASK_STACK_SIZE);
        gdt_set_kernel_stack(kstk_top);
        syscall_set_kernel_stack(kstk_top);
    }

    // Switch to user page table
    vmm_switch((pte_t*)elf_result.user_pml4);
    
    // DEBUG: Verify CR3 is correct
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    fb_puts("[TASK] CR3 after vmm_switch: ");
    fb_puthex(current_cr3);
    fb_puts("\n");
    fb_puts("[TASK] Expected CR3 (table_phys): ");
    uint64_t expected_cr3 = (uint64_t)elf_result.user_pml4 - hhdm_off;
    fb_puthex(expected_cr3);
    fb_puts("\n");
    
    // DEBUG: Проверяем что entry point замаплен в user PML4
    fb_puts("[TASK] Verifying entry point mapping in user PML4...\n");
    uint64_t entry_phys = vmm_virt_to_phys((pte_t*)elf_result.user_pml4, elf_result.entry_point);
    if (!entry_phys) {
        fb_puts("[TASK] ERROR: Entry point ");
        fb_puthex(elf_result.entry_point);
        fb_puts(" NOT MAPPED in user PML4!\n");
        task_exit();
        return;
    }
    fb_puts("[TASK] Entry point maps to phys ");
    fb_puthex(entry_phys);
    fb_puts("\n");
    
    fb_puts("[TASK] Entering usermode...\n");
    fb_puts("[TASK] User stack top: ");
    fb_puthex(user_stack_top);
    fb_puts("\n[TASK] Entry point: ");
    fb_puthex(elf_result.entry_point);
    fb_putchar('\n');
    
    /* Попробуем прочитать первые байты entry point через физический адрес */
    fb_puts("[TASK] Reading first bytes at entry point...\n");
    uint8_t *entry_code = (uint8_t *)(entry_phys + hhdm_off);
    fb_puts("[TASK] First 16 bytes: ");
    for (int i = 0; i < 16; i++) {
        uint8_t b = entry_code[i];
        char hex[3];
        hex[0] = (b >> 4) < 10 ? '0' + (b >> 4) : 'a' + (b >> 4) - 10;
        hex[1] = (b & 0xF) < 10 ? '0' + (b & 0xF) : 'a' + (b & 0xF) - 10;
        hex[2] = ' ';
        fb_putchar(hex[0]);
        fb_putchar(hex[1]);
        fb_putchar(hex[2]);
    }
    fb_putchar('\n');
    
    fb_puts("[TASK] About to call enter_usermode...\n");
    
    enter_usermode(elf_result.entry_point, user_stack_top);
    
    // Should not return
    fb_puts("[TASK] ERROR: Returned from usermode\n");
    task_exit();
}

void userspace_hello_task(void) {
    task_t *current = sched_current();
    if (current) current->userdata = (void *)"/hello";
    userspace_elf_loader_task();
}

/* --- Dock launcher ------------------------------------------------------
 * Фоновая kernel-задача: ждёт запрос от dock (клик по иконке терминала) и
 * запускает /vsh. Запуск делается в контексте задачи (а НЕ в IRQ мыши), где
 * kmalloc/планировщик безопасны. wm_handle_mouse_button лишь ставит флаг,
 * который мы здесь забираем через wm_dock_consume_launch(). */
void dock_launcher_task(void) {
    extern int wm_dock_consume_launch(void);
    for (;;) {
        /* Проверка запроса и засыпание — под cli, чтобы клик по доку (mouse IRQ)
         * не проскочил между проверкой и блокировкой (lost-wakeup). */
        __asm__ volatile("cli");
        if (wm_dock_consume_launch()) {
            __asm__ volatile("sti");
            vfs_node_t *vsh = vfs_open("/vsh", 0);
            if (vsh) {
                vfs_close(vsh);
                task_t *t = task_create("vsh", userspace_elf_loader_task, 10);
                if (t) t->userdata = (void *)"/vsh";
            }
            continue;
        }
        /* Запросов нет — засыпаем (0% CPU). Раньше тут был hlt-цикл, который
         * хоть и halt'ил CPU, но УДЕРЖИВАЛ свой квант планировщика (priority 3 =
         * 3 тика) ничего не делая → рендер раз в цикл простаивал ~30 мс и
         * картинка дёргалась. Теперь блокируемся; будит sched_wake из обработчика
         * клика по доку (wm_handle_mouse_button). «Block, don't poll.» */
        sched_block_current();
        __asm__ volatile("sti\n\thlt");   /* неделимо; первый IRQ переключит */
    }
}

/* --- kmain -------------------------------------------------------------- */

void kmain(void) {
    extern void enter_usermode(uint64_t entry_point, uint64_t user_stack);
    extern void *kmalloc_aligned(uint64_t size, uint64_t align);
    
    /* Включаем SSE/FPU — нужно для fxsave/fxrstor в обработчиках прерываний */
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);  /* clear EM */
    cr0 |=  (1ULL << 1);  /* set MP */
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10); /* OSFXSR | OSXMMEXCPT */
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    __asm__ volatile("fninit");
    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
        fb_addr  = (uint32_t *)fb->address;
        fb_pitch = fb->pitch;
        fb_width = fb->width;
        fb_height = fb->height;
        for (uint64_t i = 0; i < fb->height * (fb->pitch / 4); i++)
            fb_addr[i] = BG;
    }

    fb_puts("VortexOS v0.1\n\n");

    gdt_init();
    fb_puts("[OK] GDT initialized\n");

    idt_init();
    fb_puts("[OK] IDT initialized\n");

    if (mmap_request.response) {
        struct limine_memmap_response *mmap = mmap_request.response;
        pmm_init();
        for (uint64_t i = 0; i < mmap->entry_count; i++) {
            struct limine_memmap_entry *e = mmap->entries[i];
            if (e->type == LIMINE_MEMMAP_USABLE)
                pmm_init_region(e->base, e->length);
        }
    }
    fb_puts("[OK] PMM initialized\n");

    uint64_t hhdm_off = 0;
    if (hhdm_request.response)
        hhdm_off = hhdm_request.response->offset;
    vmm_init(hhdm_off);
    fb_puts("[OK] VMM initialized\n");

    heap_init(0xFFFFFFFF81000000ULL, 32 * 1024 * 1024);  // 32MB heap (нужен запас под virtio-gpu fb backing)
    fb_puts("[OK] Heap initialized (32MB)\n");

    void *p1 = kmalloc(128);
    void *p2 = kmalloc(256);
    kfree(p1);
    void *p3 = kmalloc(64);
    (void)p2; (void)p3;
    fb_puts("[OK] kmalloc/kfree OK\n");

    keyboard_init();
    fb_puts("[OK] Keyboard initialized\n");

    mouse_init();
    fb_puts("[OK] Mouse initialized\n");

    pci_init();

    /* virtio-gpu: аппаратный present без разрывов. Если устройства нет
     * (обычный -vga std) или инициализация не удалась — тихо остаёмся на
     * Limine framebuffer, загрузка не страдает. */
    virtio_gpu_init();

    ata_init();

    syscall_init();
    
    /* Initialize Window Manager */
    extern void wm_init(void);
    wm_init();
    fb_puts("[OK] Window Manager initialized\n");

    /* --- FAT32 тест ------------------------------------------------ */
    fb_puts("[TEST] Trying to mount FAT32...\n");
    vfs_init();
    if (fat32_init() == 0) {
        fb_puts("[OK] FAT32 boot sector read\n");
        fat32_mount();
    } else {
        fb_puts("[WARN] FAT32 failed, using ramfs\n");
        vfs_node_t *ramfs_root = ramfs_create_root();
        vfs_mount_root(ramfs_root);
        fb_puts("[OK] VFS + ramfs mounted\n");
        
        /* Создаём базовую структуру директорий */
        vfs_mkdir("/bin");
        vfs_mkdir("/etc");
        vfs_mkdir("/tmp");
        vfs_mkdir("/dev");
    }

    /* --- Тест VFS -------------------------------------------------- */
    fb_puts("[TEST] Checking /test.txt...\n");
    vfs_node_t *testfile = vfs_open("/test.txt", 0);
    if (testfile) {
        uint8_t rbuf[128];
        int32_t n = vfs_read(testfile, 0, 127, rbuf);
        if (n > 0) {
            rbuf[n] = 0;
            fb_puts("[VFS] /test.txt: ");
            fb_puts((const char *)rbuf);
            fb_putchar('\n');
        }
        vfs_close(testfile);
    } else {
        fb_puts("[VFS] /test.txt NOT FOUND!\n");
    }
    
    fb_puts("[TEST] Checking /hello...\n");
    vfs_node_t *hello_test = vfs_open("/hello", 0);
    if (hello_test) {
        fb_puts("[VFS] /hello FOUND!\n");
        vfs_close(hello_test);
    } else {
        fb_puts("[VFS] /hello NOT FOUND!\n");
    }

    /* --- Планировщик + таймер ------------------------------------------- */
    sched_init();
    fb_puts("[OK] Scheduler initialized\n");
    
    pit_init(100);
    fb_puts("[OK] PIT initialized (100 Hz)\n");

    /* Создаём тестовые задачи */
    /* GUI теперь полностью в userspace через window manager */
    
    /* Сначала пробуем терминал /vsh (userspace Vortex Shell), потом
     * window test / vgraph если терминала нет на диске. */
    fb_puts("[TEST] Checking for /vsh...\n");
    vfs_node_t *vsh_node = vfs_open("/vsh", 0);
    vfs_node_t *tw_node = vsh_node ? 0 : vfs_open("/testwin", 0);
    if (vsh_node) {
        fb_puts("[OK] /vsh FOUND! Launching Vortex Shell\n");
        vfs_close(vsh_node);

        /* ЗАДЕРЖКА 3 секунды (как у остальных GUI-задач) */
        uint64_t start = pit_ticks();
        while (pit_ticks() - start < 300);

        task_t *vsh_task = task_create("vsh", userspace_elf_loader_task, 10);
        if (vsh_task) vsh_task->userdata = (void *)"/vsh";
    } else if (tw_node) {
        fb_puts("[OK] /testwin FOUND!\n");
        vfs_close(tw_node);
        fb_puts("[OK] /testwin found, creating window test task\n");
        
        /* ЗАДЕРЖКА 3 секунды */
        fb_puts("[DEBUG] Starting window test in 3 seconds...\n");
        uint64_t start = pit_ticks();
        while (pit_ticks() - start < 300);
        
        task_t *tw_task = task_create("testwin", userspace_elf_loader_task, 10);
        if (tw_task) tw_task->userdata = (void *)"/testwin";
    } else {
        fb_puts("[TEST] /testwin not found, checking for /vgraph...\n");
        vfs_node_t *vg_node = vfs_open("/vgraph", 0);
        if (vg_node) {
            fb_puts("[OK] /vgraph FOUND!\n");
            vfs_close(vg_node);
            fb_puts("[OK] /vgraph found, creating compositor task\n");
            
            /* ЗАДЕРЖКА 3 секунды */
            fb_puts("[DEBUG] Starting compositor in 3 seconds...\n");
            uint64_t start = pit_ticks();
            while (pit_ticks() - start < 300);
            
            task_t *vg_task = task_create("vgraph", userspace_elf_loader_task, 10);
            if (vg_task) vg_task->userdata = (void *)"/vgraph";
        } else {
            fb_puts("[WARN] No GUI programs found\n");
        }
    }
    
    /* ВРЕМЕННО ОТКЛЮЧАЕМ hello пока тестируем compositor */
    /*
    vfs_node_t *hello_node = vfs_open("/hello", 0);
    if (hello_node) {
        vfs_close(hello_node);
        fb_puts("[OK] /hello found, creating userspace task\n");
        task_t *hello_task = task_create("hello", userspace_elf_loader_task, 5);
        if (hello_task) hello_task->userdata = (void *)"/hello";
    } else {
        fb_puts("[WARN] /hello not found (run 'make disk-with-apps')\n");
    }
    */
    
    /* Shell как фоновая задача */
    // task_create("vos-sh", shell_run, 2);

    /* Задача рендера (fix #1) — рендер вынесен из PIT IRQ0 в отдельную задачу.
     * PIT лишь ставит флаг, а эта задача рисует кадр с включёнными прерываниями,
     * не стопоря планировщик и остальные IRQ. Высокий приоритет, чтобы кадры
     * шли ровно ~50 FPS. */
    extern void wm_render_task(void);
    task_create("render", wm_render_task, 8);
    fb_puts("[OK] Render task started\n");

    /* Dock launcher — запускает терминал по клику на иконку в доке. */
    {
        task_t *dock_task = task_create("dock", dock_launcher_task, 3);
        extern void wm_set_dock_task(task_t *);
        wm_set_dock_task(dock_task);   /* чтобы клик по доку мог разбудить задачу */
    }
    fb_puts("[OK] Dock launcher started\n");

    fb_puts("[OK] All tasks created\n");
    fb_puts("[SCHEDULER] Starting multitasking...\n\n");

    /* kmain теперь idle task - планировщик возьмёт управление при первом IRQ0 */
    for (;;) __asm__ volatile("hlt");
}
