#include <kernel/module.h>
#include <kernel/mmu.h>
#include <kernel/lock.h>
#include <kernel/init.h>

static uintptr_t module_next_free = MODULE_REGION_BASE;
static spinlock_t module_alloc_lock;

void module_alloc_init(void) {
    spin_lock_init(&module_alloc_lock);
    module_next_free = MODULE_REGION_BASE;
}

void *module_alloc(size_t size) {
    size = ALIGN_UP(size, PAGE_SIZE);

    spin_lock(&module_alloc_lock);

    if (module_next_free < MODULE_REGION_BASE ||
        module_next_free - MODULE_REGION_BASE + size > MODULE_REGION_SIZE ||
        module_next_free + size < module_next_free) {
        spin_unlock(&module_alloc_lock);
        return NULL;
    }

    uintptr_t vaddr = module_next_free;
    module_next_free += size;

    spin_unlock(&module_alloc_lock);

    page_table_t *kmap = mmu_get_kernel_map();
    size_t npages = size / PAGE_SIZE;

    for (size_t i = 0; i < npages; i++) {
        page_t *pg = page_alloc(0);
        if (!pg) {
            for (size_t j = 0; j < i; j++)
                mmu_unmap(kmap, vaddr + j * PAGE_SIZE);
            return NULL;
        }
        uintptr_t phys = page_to_phys(pg);
        if (!mmu_map_4k(kmap, vaddr + i * PAGE_SIZE, phys, MMU_FLAGS_READ | MMU_FLAGS_WRITE | MMU_FLAGS_EXEC)) {
            page_free(pg, 0);
            return NULL;
        }
    }

    return (void *)vaddr;
}

void module_free(void *ptr, size_t size) {
    if (!ptr) return;
    size = ALIGN_UP(size, PAGE_SIZE);
    page_table_t *kmap = mmu_get_kernel_map();
    size_t npages = size / PAGE_SIZE;

    for (size_t i = 0; i < npages; i++) {
        uintptr_t vaddr = (uintptr_t)ptr + i * PAGE_SIZE;
        mmu_unmap(kmap, vaddr);
    }
    mmu_tlb_shootdown();
}

void module_set_rx(void *base, size_t size) {
    page_table_t *kmap = mmu_get_kernel_map();
    uintptr_t addr = (uintptr_t)base;
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        mmu_protect_page(kmap, addr + off, MMU_FLAGS_READ | MMU_FLAGS_EXEC);
    }
    mmu_tlb_shootdown();
}

void module_set_ro(void *base, size_t size) {
    page_table_t *kmap = mmu_get_kernel_map();
    uintptr_t addr = (uintptr_t)base;
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        mmu_protect_page(kmap, addr + off, MMU_FLAGS_READ);
    }
    mmu_tlb_shootdown();
}

late_initcall(module_alloc_init);
