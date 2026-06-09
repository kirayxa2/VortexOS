/* =============================================================================
 * VortexOS — kernel/drivers/keyboard.c
 * Драйвер PS/2 клавиатуры. Обрабатывает IRQ1, переводит scancode -> ASCII.
 * ============================================================================= */

#include "keyboard.h"
#include "idt.h"
#include "sched.h"
#include "fb.h"

#define KB_DATA 0x60   /* порт данных PS/2 */
#define KB_STATUS 0x64 /* порт статуса */

/* Кольцевой буфер для символов */
#define KB_BUF_SIZE 64
static char     kb_buf[KB_BUF_SIZE];
static uint32_t kb_head = 0;
static uint32_t kb_tail = 0;

static uint8_t shift = 0;
static uint8_t caps  = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

static inline void ps2_wait_write(void) {
    uint32_t timeout = 100000;
    while (timeout-- && (inb(0x64) & 0x02));
}

/* Scancode set 1 -> ASCII (без shift) */
static const char sc_normal[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};

/* Scancode set 1 -> ASCII (с shift) */
static const char sc_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

static void kb_handler(interrupt_frame_t *frame) {
    (void)frame;
    uint8_t sc = inb(KB_DATA);

    /* Отпускание клавиши (бит 7 = 1) */
    if (sc & 0x80) {
        sc &= 0x7F;
        if (sc == 0x2A || sc == 0x36) shift = 0;
        return;
    }

    /* Shift */
    if (sc == 0x2A || sc == 0x36) { shift = 1; return; }
    /* Caps Lock */
    if (sc == 0x3A) { caps ^= 1; return; }

    if (sc >= 128) return;

    char c = shift ? sc_shift[sc] : sc_normal[sc];
    if (!c) return;

    /* Caps Lock влияет только на буквы */
    if (caps && c >= 'a' && c <= 'z') c -= 32;
    if (caps && c >= 'A' && c <= 'Z') c += 32;

    /* Кладём в буфер */
    uint32_t next = (kb_head + 1) % KB_BUF_SIZE;
    if (next != kb_tail) {
        kb_buf[kb_head] = c;
        kb_head = next;
    }
}

void keyboard_init(void) {
    /* Flush PS/2 output buffer — точная копия VortexOS */
    int limit = 128;
    while (limit-- > 0 && (inb(0x64) & 0x01))
        (void)inb(0x60);
    
    irq_register(1, kb_handler);
}

char keyboard_getchar(void) {
    while (kb_head == kb_tail) {
        /* Без hlt — простой busy-wait.
         * hlt может ломать стак если rsp не выровнян. */
        __asm__ volatile("pause");
    }
    char c = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    return c;
}
