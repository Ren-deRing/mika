#include <kernel/kstack.h>
#include <kernel/mmu.h>
#include <kernel/printf.h>

void* kstack_alloc(void) {
    page_t* p = page_alloc(KSTACK_ORDER);
    if (!p) {
        dprintf("kstack_alloc: Failed to allocate pages for kernel stack");
        return NULL;
    }

    uintptr_t stack_bottom_phys = page_to_phys(p);
    uintptr_t stack_bottom_virt = (uintptr_t)p2v(stack_bottom_phys);

    mmu_protect_page(mmu_get_kernel_map(), stack_bottom_virt, 0);

    void* stack_start = (void*)(stack_bottom_virt + PAGE_SIZE);
    return stack_start;
}

void kstack_free(void* stack) {
    if (!stack) return;

    uintptr_t stack_start_virt = (uintptr_t)stack;
    uintptr_t stack_bottom_virt = stack_start_virt - PAGE_SIZE;
    uintptr_t stack_bottom_phys = v2p((void*)stack_bottom_virt);

    page_free(phys_to_page(stack_bottom_phys), KSTACK_ORDER);
}
