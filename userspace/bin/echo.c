/* /bin/echo [-n] args... — печать аргументов. */
#include "vutil.h"

int main(int argc, char **argv) {
    int start = 1, newline = 1;
    if (argc > 1 && !strcmp(argv[1], "-n")) { newline = 0; start = 2; }
    for (int i = start; i < argc; i++) {
        if (i > start) putchar(' ');
        printf("%s", argv[i]);
    }
    if (newline) putchar('\n');
    return 0;
}
