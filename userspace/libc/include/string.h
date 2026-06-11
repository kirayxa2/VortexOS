/* =============================================================================
 * VortexOS libc — string.h
 * ============================================================================= */
#ifndef _VLIBC_STRING_H
#define _VLIBC_STRING_H

#include <stddef.h>

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);

/* Не-стандарт, но удобно: всегда NUL-терминирует, возвращает strlen(src). */
size_t strlcpy(char *dst, const char *src, size_t cap);

#endif /* _VLIBC_STRING_H */
