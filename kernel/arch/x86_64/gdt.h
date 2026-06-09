#ifndef VOS_GDT_H
#define VOS_GDT_H

#include "types.h"

void gdt_init(void);
void gdt_set_kernel_stack(uint64_t rsp0);
void gdt_install_tss(uint64_t tss_addr);

#define SEG_KERNEL_CODE 0x08
#define SEG_KERNEL_DATA 0x10
/* udata=index3(0x18)|RPL3=0x1B, ucode=index4(0x20)|RPL3=0x23
 * Этот порядок нужен для sysretq: STAR[63:48]=0x10 → SS=+8=0x18|3, CS=+16=0x20|3 */
#define SEG_USER_DATA   0x1B   /* udata: GDT index 3, RPL=3 */
#define SEG_USER_CODE   0x23   /* ucode: GDT index 4, RPL=3 */
#define SEG_TSS         0x28

#endif /* VOS_GDT_H */
