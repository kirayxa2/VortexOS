// Simple hello world for userspace

// Syscall numbers (matching kernel)
#define SYS_WRITE 0
#define SYS_EXIT  1

void _start() {
    const char *msg = "Hello from userspace!\n";
    
    // Calculate string length
    int len = 0;
    while (msg[len]) len++;
    
    // syscall: write(1, msg, len)
    __asm__ volatile (
        "mov $0, %%rax\n"      // SYS_WRITE = 0
        "mov $1, %%rdi\n"      // fd = 1 (stdout)
        "mov %0, %%rsi\n"      // buf = msg
        "mov %1, %%rdx\n"      // count = len
        "syscall\n"
        :
        : "r"(msg), "r"((long)len)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11"
    );
    
    // syscall: exit(0)
    __asm__ volatile (
        "mov $1, %%rax\n"      // SYS_EXIT = 1
        "mov $0, %%rdi\n"      // exit_code = 0
        "syscall\n"
        :
        :
        : "rax", "rdi", "rcx", "r11"
    );
    
    // Should never reach here
    for (;;) {
        __asm__ volatile("hlt");
    }
}
