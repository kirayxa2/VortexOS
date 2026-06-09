/* =============================================================================
 * VortexOS — userspace/vortexgraph.c
 * Compositor в userspace — display server
 * ============================================================================= */

typedef unsigned long uint64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef short int16_t;

/* Syscall numbers */
#define SYS_WRITE      0
#define SYS_EXIT       1
#define SYS_FB_INFO    4
#define SYS_FB_MAP     5
#define SYS_INPUT_POLL 6

/* Syscall wrapper */
static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    __asm__ volatile(
        "mov %1, %%rax\n"
        "mov %2, %%rdi\n"
        "mov %3, %%rsi\n"
        "mov %4, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static inline uint64_t syscall1(uint64_t num, uint64_t a1) {
    return syscall3(num, a1, 0, 0);
}

static inline uint64_t syscall0(uint64_t num) {
    return syscall3(num, 0, 0, 0);
}

/* Framebuffer info */
typedef struct {
    uint64_t phys_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} fb_info_t;

/* Input event */
typedef struct {
    uint8_t type;
    int16_t mouse_dx;
    int16_t mouse_dy;
    uint8_t mouse_buttons;
    uint16_t key_code;
    uint8_t key_pressed;
} input_event_t;

/* Простая функция вывода текста */
void puts(const char *s) {
    int len = 0;
    while (s[len]) len++;
    syscall3(SYS_WRITE, 1, (uint64_t)s, len);
}

/* -------------------------------------------------------------------------
 * Главная функция compositor
 * ------------------------------------------------------------------------- */

void _start(void) {
    puts("VortexGraph: Display server starting...\n");
    
    /* Получаем информацию о framebuffer */
    fb_info_t fb_info;
    syscall1(SYS_FB_INFO, (uint64_t)&fb_info);
    
    puts("VortexGraph: Framebuffer info:\n");
    puts("  Width: ");
    /* TODO: вывести число */
    puts("\n");
    
    /* Мапим framebuffer в наше адресное пространство */
    uint64_t fb_addr = syscall0(SYS_FB_MAP);
    
    if (fb_addr == 0) {
        puts("VortexGraph: Failed to map framebuffer!\n");
        syscall1(SYS_EXIT, 1);
    }
    
    puts("VortexGraph: Framebuffer mapped at 0x80000000\n");
    
    /* Получаем указатель на framebuffer */
    uint32_t *fb = (uint32_t *)fb_addr;
    
    /* Рисуем тестовую картинку — синий градиент */
    for (uint32_t y = 0; y < fb_info.height; y++) {
        for (uint32_t x = 0; x < fb_info.width; x++) {
            uint8_t blue = (y * 255) / fb_info.height;
            uint32_t color = 0xFF000000 | blue;  /* ARGB: синий градиент */
            fb[y * (fb_info.pitch / 4) + x] = color;
        }
    }
    
    puts("VortexGraph: Test pattern drawn!\n");
    puts("VortexGraph: Entering main loop...\n");
    
    /* Главный цикл — обрабатываем события */
    for (;;) {
        input_event_t event;
        syscall1(SYS_INPUT_POLL, (uint64_t)&event);
        
        /* TODO: обработать события и перерисовать */
        
        /* Простая задержка */
        __asm__ volatile("pause");
    }
    
    syscall1(SYS_EXIT, 0);
}
