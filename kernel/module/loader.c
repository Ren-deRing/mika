#include <kernel/module.h>
#include <kernel/kmem.h>
#include <kernel/printf.h>
#include <kernel/fs/vfs.h>
#include <kernel/init.h>
#include <uapi/elf.h>
#include <uapi/fcntl.h>
#include <string.h>

struct module_section {
    uint32_t sh_type;
    uint64_t sh_flags;
    uint32_t sh_index;
    uintptr_t loaded_addr;
    uint64_t size;
};

struct module *module_load_from_fd(int fd) {
    off_t file_size = vfs_lseek(fd, 0, SEEK_END);
    if (file_size < (off_t)sizeof(Elf64_Ehdr)) return NULL;
    vfs_lseek(fd, 0, SEEK_SET);

    void *file_buf = kmalloc(file_size);
    if (!file_buf) return NULL;

    ssize_t rd = vfs_read(fd, file_buf, file_size);
    if (rd != file_size) { kfree(file_buf); return NULL; }

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)file_buf;

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        kfree(file_buf);
        return NULL;
    }
    if (ehdr->e_type != ET_REL || ehdr->e_machine != EM_X86_64 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        kfree(file_buf);
        return NULL;
    }

    Elf64_Shdr *shdr = (Elf64_Shdr *)((uintptr_t)file_buf + ehdr->e_shoff);
    uint16_t shnum = ehdr->e_shnum;
    uint16_t shstrndx = ehdr->e_shstrndx;
    const char *shstrtab = (const char *)file_buf + shdr[shstrndx].sh_offset;

    const char *modinfo = NULL;
    Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    uint32_t strtab_size = 0;
    uint16_t symtab_idx = 0;

    uint32_t alloc_count = 0;
    struct module_section sections[64];

    for (uint16_t i = 0; i < shnum; i++) {
        if (shdr[i].sh_type == SHT_SYMTAB) {
            symtab = (Elf64_Sym *)((uintptr_t)file_buf + shdr[i].sh_offset);
            symtab_idx = i;
            strtab = (const char *)file_buf + shdr[shdr[i].sh_link].sh_offset;
            strtab_size = shdr[shdr[i].sh_link].sh_size;
        }
    }

    for (uint16_t i = 0; i < shnum; i++) {
        const char *name = shstrtab + shdr[i].sh_name;
        if (__builtin_strcmp(name, ".modinfo") == 0 && shdr[i].sh_type == SHT_PROGBITS) {
            modinfo = (const char *)file_buf + shdr[i].sh_offset;
        }
    }

    for (uint16_t i = 0; i < shnum; i++) {
        if (shdr[i].sh_type == SHT_PROGBITS || shdr[i].sh_type == SHT_NOBITS) {
            if (shdr[i].sh_flags & SHF_ALLOC) {
                sections[alloc_count].sh_type = shdr[i].sh_type;
                sections[alloc_count].sh_flags = shdr[i].sh_flags;
                sections[alloc_count].sh_index = i;
                sections[alloc_count].size = shdr[i].sh_size;
                alloc_count++;
                if (alloc_count >= 64) break;
            }
        }
    }

    uint64_t total_size = 0;
    uint64_t alignments[64];
    for (uint32_t i = 0; i < alloc_count; i++) {
        uint64_t align = shdr[sections[i].sh_index].sh_addralign;
        if (align < 16) align = 16;
        alignments[i] = align;
        total_size = ALIGN_UP(total_size, align);
        total_size += sections[i].size;
    }

    void *module_base = module_alloc(total_size);
    if (!module_base) {
        kfree(file_buf);
        return NULL;
    }

    uintptr_t cursor = (uintptr_t)module_base;
    for (uint32_t i = 0; i < alloc_count; i++) {
        cursor = ALIGN_UP(cursor, alignments[i]);
        sections[i].loaded_addr = cursor;

        Elf64_Shdr *s = &shdr[sections[i].sh_index];
        if (s->sh_type == SHT_PROGBITS) {
            __builtin_memcpy((void *)cursor, (const void *)((uintptr_t)file_buf + s->sh_offset), s->sh_size);
        } else {
            __builtin_memset((void *)cursor, 0, s->sh_size);
        }
        cursor += sections[i].size;
    }

    for (uint16_t i = 0; i < shnum; i++) {
        if (shdr[i].sh_type == SHT_RELA) {
            uint32_t target_idx = shdr[i].sh_info;
            uintptr_t target_base = 0;
            for (uint32_t j = 0; j < alloc_count; j++) {
                if (sections[j].sh_index == target_idx) {
                    target_base = sections[j].loaded_addr;
                    break;
                }
            }
            if (!target_base) continue;

            Elf64_Rela *rela = (Elf64_Rela *)((uintptr_t)file_buf + shdr[i].sh_offset);
            uint32_t count = shdr[i].sh_size / sizeof(Elf64_Rela);

            struct module_secmap secmap[64];
            for (uint32_t k = 0; k < alloc_count; k++) {
                secmap[k].elf_shndx = sections[k].sh_index;
                secmap[k].loaded_addr = sections[k].loaded_addr;
            }

            if (module_apply_rela((void *)target_base, rela, count,
                                  symtab, strtab, strtab_size,
                                  secmap, alloc_count) < 0) {
                module_free(module_base, total_size);
                kfree(file_buf);
                return NULL;
            }
        }
    }

    struct module *mod = kcalloc(1, sizeof(struct module));
    if (!mod) {
        module_free(module_base, total_size);
        kfree(file_buf);
        return NULL;
    }

    if (modinfo) {
        for (uint16_t i = 0; i < shnum; i++) {
            if (__builtin_strcmp(shstrtab + shdr[i].sh_name, ".modinfo") == 0) {
                const char *info = (const char *)file_buf + shdr[i].sh_offset;
                for (const char *p = info; p < info + shdr[i].sh_size; ) {
                    if (__builtin_strncmp(p, "name=", 5) == 0) {
                        const char *val = p + 5;
                        uint32_t len = 0;
                        while (val[len] && val[len] != '\n' && len < MODULE_NAME_LEN - 1) len++;
                        __builtin_memcpy(mod->name, val, len);
                        mod->name[len] = '\0';
                    }
                    while (p < info + shdr[i].sh_size && *p) p++;
                    p++;
                }
                break;
            }
        }
    }

    if (mod->name[0] == '\0') {
        __builtin_strncpy(mod->name, "unknown", MODULE_NAME_LEN - 1);
    }

    for (uint16_t i = 0; i < shnum; i++) {
        const char *name = shstrtab + shdr[i].sh_name;
        for (uint32_t j = 0; j < alloc_count; j++) {
            if (sections[j].sh_index == i) {
                if (__builtin_strcmp(name, ".init.text") == 0) {
                    mod->init = (int (*)(void))(void *)sections[j].loaded_addr;
                } else if (__builtin_strcmp(name, ".exit.text") == 0) {
                    mod->exit = (void (*)(void))(void *)sections[j].loaded_addr;
                }
                break;
            }
        }
    }

    if (!mod->init) {
        for (uint32_t j = 0; j < alloc_count; j++) {
            if (sections[j].sh_flags & SHF_EXECINSTR) {
                mod->init = (int (*)(void))(void *)sections[j].loaded_addr;
                break;
            }
        }
    }

    mod->base = module_base;
    mod->size = total_size;
    mod->refcount = 0;
    mod->state = MODULE_STATE_LOADED;

    kfree(file_buf);
    return mod;
}
