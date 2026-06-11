/* =============================================================================
 * VortexOS libc — ctype.h (ASCII)
 * ============================================================================= */
#ifndef _VLIBC_CTYPE_H
#define _VLIBC_CTYPE_H

static inline int isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
static inline int isupper(int c) { return c >= 'A' && c <= 'Z'; }
static inline int islower(int c) { return c >= 'a' && c <= 'z'; }
static inline int isalpha(int c) { return isupper(c) || islower(c); }
static inline int isalnum(int c) { return isalpha(c) || isdigit(c); }
static inline int isprint(int c) { return c >= 0x20 && c < 0x7F; }
static inline int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
static inline int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

#endif /* _VLIBC_CTYPE_H */
