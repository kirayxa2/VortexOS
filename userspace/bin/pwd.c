/* /bin/pwd — текущий каталог (наследуется от шелла при spawn). */
#include "vutil.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("%s\n", vu_cwd());
    return 0;
}
