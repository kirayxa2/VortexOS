/* /bin/whoami — текущий uid/gid процесса (SYS_GETUID). */
#include "vutil.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t uid = vos_getuid();
    uint32_t gid = vos_getgid();
    if (uid == 0)
        printf("root (uid=0 gid=%u)\n", gid);
    else
        printf("uid=%u gid=%u\n", uid, gid);
    return 0;
}
