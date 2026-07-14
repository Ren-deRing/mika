#include <kernel/module.h>
#include <kernel/symbol.h>
#include <uapi/elf.h>

int module_apply_rela(void *base, const Elf64_Rela *rela, uint32_t count,
                      Elf64_Sym *symtab, const char *strtab, size_t strtab_size,
                      const struct module_secmap *secmap, uint32_t secmap_count) {
    (void)strtab_size;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t sym_idx = ELF64_R_SYM(rela[i].r_info);
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        Elf64_Sxword addend = rela[i].r_addend;
        uintptr_t loc = (uintptr_t)base + rela[i].r_offset;

        unsigned long sym_addr = 0;

        if (sym_idx != 0) {
            Elf64_Sym *sym = &symtab[sym_idx];
            int bind = ELF64_ST_BIND(sym->st_info);

            if (sym->st_shndx == SHN_UNDEF) {
                if (bind == STB_GLOBAL || bind == STB_WEAK) {
                    const char *name = strtab + sym->st_name;
                    sym_addr = kallsyms_lookup_name(name);
                    if (!sym_addr) {
                        return -1;
                    }
                } else {
                    continue;
                }
            } else if (sym->st_shndx == SHN_ABS) {
                sym_addr = sym->st_value;
            } else {
                /* ET_REL: st_value is section-relative.
                 * Look up the section's loaded address from the secmap. */
                for (uint32_t j = 0; j < secmap_count; j++) {
                    if (secmap[j].elf_shndx == sym->st_shndx) {
                        sym_addr = secmap[j].loaded_addr + sym->st_value;
                        break;
                    }
                }
                if (!sym_addr) {
                    sym_addr = (uintptr_t)base + sym->st_value;
                }
            }
        }

        switch (type) {
        case R_X86_64_64:
            *(uint64_t *)loc = sym_addr + addend;
            break;
        case R_X86_64_PC32:
        case R_X86_64_PLT32: {
            int32_t val = (int32_t)(sym_addr + addend - loc);
            *(int32_t *)loc = val;
            break;
        }
        case R_X86_64_32S: {
            int32_t val = (int32_t)(sym_addr + addend);
            *(int32_t *)loc = val;
            break;
        }
        case R_X86_64_32: {
            uint32_t val = (uint32_t)(sym_addr + addend);
            *(uint32_t *)loc = val;
            break;
        }
        case R_X86_64_NONE:
            break;
        default:
            return -1;
        }
    }
    return 0;
}
