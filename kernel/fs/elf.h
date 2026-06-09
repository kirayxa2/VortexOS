#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word    p_type;
    Elf64_Word    p_flags;
    Elf64_Off     p_offset;
    Elf64_Addr    p_vaddr;
    Elf64_Addr    p_paddr;
    Elf64_Xword   p_filesz;
    Elf64_Xword   p_memsz;
    Elf64_Xword   p_align;
} Elf64_Phdr;

/* e_ident constants */
#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define EV_CURRENT 1

/* e_type constants */
#define ET_EXEC 2
#define ET_DYN  3

/* e_machine constants */
#define EM_X86_64 62

/* p_type constants */
#define PT_LOAD 1

/* p_flags constants */
#define PF_X 1
#define PF_W 2
#define PF_R 4

// ELF loading result
typedef struct {
    uint64_t entry_point;
    void *user_pml4;  // User page table
} elf_load_result_t;

// Load ELF executable from VFS path into a new user page table
// Returns {entry_point, user_pml4} on success, {0, NULL} on failure
elf_load_result_t elf_load(const char *path);

#endif
