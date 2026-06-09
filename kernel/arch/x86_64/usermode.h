#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

// Enter ring 3 and execute code at entry_point with user_stack
void enter_usermode(uint64_t entry_point, uint64_t user_stack);

#endif
