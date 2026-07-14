#pragma once

#include <stdint.h>

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

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
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

typedef struct {
    Elf64_Word  sh_name;
    Elf64_Word  sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr  sh_addr;
    Elf64_Off   sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word  sh_link;
    Elf64_Word  sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

typedef struct {
    Elf64_Word    st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
} Elf64_Sym;

#define PT_LOAD 1

#define PF_X          (1 << 0)    /* Executable */
#define PF_W          (1 << 1)    /* Writable */
#define PF_R          (1 << 2)    /* Readable */

#define SHF_WRITE     (1 << 0)  /* Writable */
#define SHF_ALLOC     (1 << 1)  /* Occupies memory during execution */
#define SHF_EXECINSTR (1 << 2)  /* Executable */

#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6

#define EI_MAG0    0          /* File identification byte 0 index */
#define ELFMAG0    0x7f       /* Magic number byte 0 */
#define EI_MAG1    1          /* File identification byte 1 index */
#define ELFMAG1    'E'        /* Magic number byte 1 */
#define EI_MAG2    2          /* File identification byte 2 index */
#define ELFMAG2    'L'        /* Magic number byte 2 */
#define EI_MAG3    3          /* File identification byte 3 index */
#define ELFMAG3    'F'        /* Magic number byte 3 */

#define ELFMAG     "\177ELF"
#define SELFMAG    4

#define EI_CLASS   4          /* File class byte index */
#define ELFCLASS64 2          /* 64-bit objects */

#define EI_DATA     5          /* Data encoding byte index */
#define ELFDATA2LSB 1

#define EI_NIDENT   16

#define ELF64_ST_BIND(i)   ((i) >> 4)
#define ELF64_ST_TYPE(i)   ((i) & 0xf)

#define PT_GNU_STACK  0x6474e551
#define PT_GNU_RELRO  0x6474e552

#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8

#define ET_NONE      0        /* No file type */
#define ET_REL       1        /* Relocatable file */
#define ET_EXEC      2        /* Executable file */
#define ET_DYN       3        /* Shared object file */
#define ET_CORE      4        /* Core file */

#define EM_NONE      0
#define EM_X86_64    62       /* AMD x86-64 architecture */

#define STT_NOTYPE  0      /* Symbol type is unspecified */
#define STT_OBJECT  1      /* Symbol is a data object */
#define STT_FUNC    2      /* Symbol is a code object (function) */
#define STT_SECTION 3      /* Symbol associated with a section */
#define STT_FILE    4      /* Symbol gives the name of the source file */

#define STB_LOCAL   0      /* Local symbol */
#define STB_GLOBAL  1      /* Global symbol */
#define STB_WEAK    2      /* Weak symbol */

#define SHN_UNDEF     0          /* Undefined section */
#define SHN_LORESERVE 0xff00     /* Start of reserved indices */
#define SHN_ABS       0xfff1     /* Associated symbol has absolute value */
#define SHN_COMMON    0xfff2     /* Associated symbol is common */

#define R_X86_64_NONE    0
#define R_X86_64_64      1
#define R_X86_64_PC32    2
#define R_X86_64_GOT32   3
#define R_X86_64_PLT32   4
#define R_X86_64_COPY    5
#define R_X86_64_RELATIVE 8
#define R_X86_64_32S     11
#define R_X86_64_32      10

typedef struct {
    Elf64_Addr    r_offset;
    Elf64_Xword   r_info;
    Elf64_Sxword  r_addend;
} Elf64_Rela;

#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xFFFFFFFFL)
#define ELF64_R_INFO(s,t) (((Elf64_Xword)(s) << 32) + ((t) & 0xFFFFFFFFL))