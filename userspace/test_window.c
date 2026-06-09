#include "syscalls.h"

void _start(void) {
    puts("Test Window: Starting...\n");
    puts("About to create window...\n");
    
    // Create window
    uint64_t win = wm_create_window("Hello VOS!", 100, 100, 400, 300);
    
    puts("Window created, id=");
    // Simple hex print
    char hexbuf[20];
    for (int i = 0; i < 16; i++) {
        int digit = (win >> (60 - i*4)) & 0xF;
        hexbuf[i] = digit < 10 ? '0' + digit : 'a' + digit - 10;
    }
    hexbuf[16] = '\n';
    hexbuf[17] = 0;
    puts(hexbuf);
    
    if (!win) {
        puts("Failed to create window!\n");
        exit(1);
    }
    
    puts("Drawing to window...\n");
    
    // Draw background
    wm_draw_rect(win, 0, 0, 400, 300, 0xFF2A2A3E);
    
    puts("Flushing...\n");
    
    // Flush to screen
    wm_flush(win);
    
    puts("Window rendered! Going to sleep...\n");
    
    // Sleep forever
    for (;;) {
        __asm__ volatile("pause");
    }
    
    exit(0);
}
