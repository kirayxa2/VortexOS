/* =============================================================================
 * VortexOS — kernel/drivers/vga.c
 * ============================================================================= */

#include "vga.h"
#include "types.h"

#define VGA_BASE        ((volatile uint16_t *)0xB8000)
#define VGA_COLS        80
#define VGA_ROWS        25
#define VGA_COLOR(fg, bg) ((uint8_t)((bg << 4) | (fg & 0x0F)))
#define VGA_CHAR(c, attr) ((uint16_t)(c | ((uint16_t)attr << 8)))
#define VGA_CRTC_ADDR   0x3D4
#define VGA_CRTC_DATA   0x3D5
#define VGA_REG_CURSOR_HI 14
#define VGA_REG_CURSOR_LO 15

static int cursor_row = 0;
static int cursor_col = 0;
static uint8_t current_color;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void vga_update_cursor(void)
{
    uint16_t pos = (uint16_t)(cursor_row * VGA_COLS + cursor_col);
    outb(VGA_CRTC_ADDR, VGA_REG_CURSOR_HI);
    outb(VGA_CRTC_DATA, (uint8_t)(pos >> 8));
    outb(VGA_CRTC_ADDR, VGA_REG_CURSOR_LO);
    outb(VGA_CRTC_DATA, (uint8_t)(pos & 0xFF));
}

static void vga_scroll(void)
{
    for (int row = 0; row < VGA_ROWS - 1; row++)
        for (int col = 0; col < VGA_COLS; col++)
            VGA_BASE[row * VGA_COLS + col] = VGA_BASE[(row + 1) * VGA_COLS + col];

    for (int col = 0; col < VGA_COLS; col++)
        VGA_BASE[(VGA_ROWS - 1) * VGA_COLS + col] = VGA_CHAR(' ', current_color);

    cursor_row = VGA_ROWS - 1;
}

void vga_init(void)
{
    current_color = VGA_COLOR(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    cursor_row = 0;
    cursor_col = 0;
    vga_clear();
}

void vga_clear(void)
{
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_BASE[i] = VGA_CHAR(' ', current_color);
    cursor_row = 0;
    cursor_col = 0;
    vga_update_cursor();
}

void vga_set_color(vga_color_t fg, vga_color_t bg)
{
    current_color = VGA_COLOR(fg, bg);
}

void vga_putchar(char c)
{
    switch (c) {
    case '\n': cursor_col = 0; cursor_row++; break;
    case '\r': cursor_col = 0; break;
    case '\t': cursor_col = (cursor_col + 8) & ~7; break;
    case '\b':
        if (cursor_col > 0) cursor_col--;
        VGA_BASE[cursor_row * VGA_COLS + cursor_col] = VGA_CHAR(' ', current_color);
        break;
    default:
        VGA_BASE[cursor_row * VGA_COLS + cursor_col] = VGA_CHAR(c, current_color);
        cursor_col++;
        break;
    }
    if (cursor_col >= VGA_COLS) { cursor_col = 0; cursor_row++; }
    if (cursor_row >= VGA_ROWS) vga_scroll();
    vga_update_cursor();
}

void vga_puts(const char *str)
{
    while (*str) vga_putchar(*str++);
}

void vga_printf(const char *fmt, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, fmt);

    while (*fmt) {
        if (*fmt != '%') { vga_putchar(*fmt++); continue; }
        fmt++;
        switch (*fmt++) {
        case 's': {
            const char *s = __builtin_va_arg(args, const char *);
            if (!s) s = "(null)";
            vga_puts(s);
            break;
        }
        case 'c':
            vga_putchar((char)__builtin_va_arg(args, int));
            break;
        case 'd': {
            int val = __builtin_va_arg(args, int);
            if (val < 0) { vga_putchar('-'); val = -val; }
            char buf[20]; int i = 0;
            if (val == 0) { vga_putchar('0'); break; }
            while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
            while (i--) vga_putchar(buf[i]);
            break;
        }
        case 'u': {
            unsigned int val = __builtin_va_arg(args, unsigned int);
            char buf[20]; int i = 0;
            if (val == 0) { vga_putchar('0'); break; }
            while (val > 0) { buf[i++] = '0' + (val % 10); val /= 10; }
            while (i--) vga_putchar(buf[i]);
            break;
        }
        case 'x':
        case 'p': {
            uint64_t val = __builtin_va_arg(args, uint64_t);
            if (fmt[-1] == 'p') vga_puts("0x");
            char buf[16]; int i = 0;
            if (val == 0) { vga_putchar('0'); break; }
            while (val > 0) {
                int d = val & 0xF;
                buf[i++] = (d < 10) ? ('0' + d) : ('a' + d - 10);
                val >>= 4;
            }
            while (i--) vga_putchar(buf[i]);
            break;
        }
        case '%': vga_putchar('%'); break;
        default:  vga_putchar('?'); break;
        }
    }
    __builtin_va_end(args);
}
