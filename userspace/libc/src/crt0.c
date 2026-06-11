/* =============================================================================
 * VortexOS libc — crt0.c
 * Точка входа libc-приложений: _start → разбор argv → main(argc, argv) → exit().
 *
 * argv приходит из ядра одной строкой (SYS_GETARGS — это cmdline, переданный
 * шеллом в SYS_SPAWN_EX, например "ls -l /bin"). Разбираем по пробелам,
 * кавычки v1 не поддерживаем. Приложения со старой сигнатурой `int main(void)`
 * совместимы по ABI: лишние аргументы в rdi/rsi просто игнорируются.
 *
 * Ядро гарантирует rsp % 16 == 8 на входе в _start (как требует SysV ABI
 * перед call), так что обычный вызов main() даёт корректно выровненный стек —
 * SSE-кодген (movaps) работает без #GP.
 * ============================================================================= */
#include <stdlib.h>
#include <vos.h>

int main(int argc, char **argv);

#define ARGV_MAX 16

static char  arg_buf[256];
static char *arg_vec[ARGV_MAX + 1];

void _start(void) {
    int argc = 0;

    int64_t n = vos_getargs(arg_buf, sizeof(arg_buf));
    if (n < 0) n = 0;
    arg_buf[n] = 0;

    char *p = arg_buf;
    while (*p && argc < ARGV_MAX) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        arg_vec[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    arg_vec[argc] = 0;

    exit(main(argc, arg_vec));
}
