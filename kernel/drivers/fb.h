#ifndef VOS_FB_H
#define VOS_FB_H

#include "types.h"

void fb_init(uint32_t *addr, uint64_t pitch, uint64_t width, uint64_t height);
void fb_putchar(char c);
void fb_puts(const char *s);
void fb_puthex(uint64_t val);

#endif /* VOS_FB_H */
