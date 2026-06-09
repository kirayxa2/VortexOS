#ifndef VOS_KEYBOARD_H
#define VOS_KEYBOARD_H

#include "types.h"

void keyboard_init(void);
char keyboard_getchar(void);  /* блокирующий — ждёт нажатия */

#endif /* VOS_KEYBOARD_H */
