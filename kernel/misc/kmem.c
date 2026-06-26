/*
 * Mika Kernel Memory Allocator
 *
 * Copyright (c) 2026 Ren-deRing (JONGHYUN WON)
 * 
 * SPDX-License-Identifier: 0BSD
 */

#include <kernel/kmem.h>
#include <kernel/init.h>
#include <kernel/cpu.h>
#include <kernel/lock.h>
#include "kernel/printf.h"

#include <string.h>

const uint32_t kmem_class_sizes[KMEM_NUM_CLASSES] = {
    8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1280, 1536, 2048
};

static const uint8_t kmem_class_init_orders[KMEM_NUM_CLASSES] = {
    0, 0, 0, 0, 0, 0, 0, 0, // 8B ~ 192B: 4KB (Order 0)
    4, 4, 4, 4, 4, 4, 4, 4  // 256B ~ 2048B: 64KB (Order 4)
};

static kmem_depot_t depots[KMEM_NUM_CLASSES];

void kmem_init(void) {
    for (int i = 0; i < KMEM_NUM_CLASSES; i++) {
        spin_lock_init(&depots[i].lock);
        depots[i].full_mags = NULL;
        depots[i].empty_mags = NULL;
        depots[i].full_count = 0;
        depots[i].empty_count = 0;
        depots[i].current_chunk = 0;
        depots[i].objs_remaining = 0;
        depots[i].current_order = kmem_class_init_orders[i];
    }
}

static int kmem_get_index(size_t size) {
    if (size <= 8) return 0;
    if (size > 2048) return -1;

    for (int i = 1; i < KMEM_NUM_CLASSES; i++) {
        if (size <= kmem_class_sizes[i]) return i;
    }
    return -1;
}

static void* kmem_alloc_large(size_t size) {
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t order = 0;
    while ((1U << order) < pages) order++;

    page_t* p = page_alloc(order);
    if (!p) return NULL;

    p->obj_size = 0;
    p->order = order;

    return (void*)p2v(page_to_phys(p));
}

static kmem_magazine_t* kmem_internal_mag_alloc(void) {
    static kmem_magazine_t* free_mags = NULL;
    static spinlock_t mag_alloc_lock = {0};

    uint64_t flags = spin_lock_irqsave(&mag_alloc_lock);

    if (!free_mags) {
        page_t* p = page_alloc(0);
        if (!p) {
            spin_unlock_irqrestore(&mag_alloc_lock, flags);
            return NULL;
        }

        kmem_magazine_t* batch = (kmem_magazine_t*)p2v(page_to_phys(p));
        int count = PAGE_SIZE / sizeof(kmem_magazine_t);

        for (int i = 0; i < count - 1; i++) {
            batch[i].next = &batch[i+1];
        }
        batch[count-1].next = NULL;
        free_mags = batch;
    }

    kmem_magazine_t* mag = free_mags;
    free_mags = mag->next;

    spin_unlock_irqrestore(&mag_alloc_lock, flags);

    mag->top = 0;
    mag->next = NULL;
    return mag;
}

static kmem_magazine_t* kmem_get_empty_mag(int idx) {
    kmem_depot_t* d = &depots[idx];
    kmem_magazine_t* mag = NULL;

    uint64_t flags = spin_lock_irqsave(&d->lock);
    if (d->empty_mags) {
        mag = d->empty_mags;
        d->empty_mags = mag->next;
        d->empty_count--;
    }
    spin_unlock_irqrestore(&d->lock, flags);

    if (!mag) {
        mag = kmem_internal_mag_alloc();
    }
    mag->top = 0;
    return mag;
}

static void kmem_magazine_swap_full(int idx) {
    struct cpu* c = curcpu;
    kmem_depot_t* d = &depots[idx];
    kmem_magazine_t* full_mag = c->magazines[idx];

    uint64_t flags = spin_lock_irqsave(&d->lock);

    full_mag->next = d->full_mags;
    d->full_mags = full_mag;
    d->full_count++;

    if (d->empty_mags) {
        kmem_magazine_t* empty = d->empty_mags;
        d->empty_mags = empty->next;
        d->empty_count--;
        spin_unlock_irqrestore(&d->lock, flags);

        c->magazines[idx] = empty;
    } else {
        spin_unlock_irqrestore(&d->lock, flags);
        c->magazines[idx] = kmem_get_empty_mag(idx);
    }
}

static void kmem_setup_slab_page(page_t* p, uint32_t obj_size, uint8_t order) {
    uint32_t num_pages = (1U << order);
    uint32_t total_size = (PAGE_SIZE << order);
    uint32_t max_objs = total_size / obj_size;

    p->obj_size = obj_size;
    p->order = order;
    p->free_count = max_objs;
    p->is_free = false;

    page_t* curr_pg = p;
    for (uint32_t i = 0; i < num_pages; i++) {
        curr_pg->obj_size = obj_size;
        curr_pg++;
    }
}

static void* kmem_depot_refill(int idx) {
    kmem_depot_t* d = &depots[idx];
    uint32_t size = kmem_class_sizes[idx];

    uint64_t flags = spin_lock_irqsave(&d->lock);

    if (d->objs_remaining == 0) {
        uint8_t order = d->current_order;
        page_t* p = page_alloc(order);
        if (!p) {
            spin_unlock_irqrestore(&d->lock, flags);
            return NULL;
        }

        d->current_chunk = (uintptr_t)p2v(page_to_phys(p));
        d->objs_remaining = (PAGE_SIZE << order) / size;

        kmem_setup_slab_page(p, size, order);
    }

    void* obj = (void*)d->current_chunk;
    d->current_chunk += size;
    d->objs_remaining--;

    page_t* page = phys_to_page(v2p(obj));
    __atomic_sub_fetch(&page->free_count, 1, __ATOMIC_RELAXED);

    spin_unlock_irqrestore(&d->lock, flags);
    return obj;
}

static void kmem_magazine_swap_empty(int idx) {
    struct cpu* c = curcpu;
    kmem_depot_t* d = &depots[idx];
    kmem_magazine_t* empty_mag = c->magazines[idx];

    uint64_t flags = spin_lock_irqsave(&d->lock);

    empty_mag->next = d->empty_mags;
    d->empty_mags = empty_mag;
    d->empty_count++;

    if (d->full_mags) {
        kmem_magazine_t* full = d->full_mags;
        d->full_mags = full->next;
        d->full_count--;
        c->magazines[idx] = full;
        spin_unlock_irqrestore(&d->lock, flags);
    } else {
        spin_unlock_irqrestore(&d->lock, flags);

        kmem_magazine_t* new_mag = kmem_get_empty_mag(idx);
        for (int i = 0; i < KMEM_MAG_CAPACITY; i++) {
            void* obj = kmem_depot_refill(idx);
            if (!obj) break;
            new_mag->slots[new_mag->top++] = obj;
        }
        c->magazines[idx] = new_mag;
    }
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    if (size > KMEM_MAX_DIRECT) {
        return kmem_alloc_large(size);
    }

    int idx = kmem_get_index(size);
    if (idx == -1) return NULL;

    struct cpu* c = curcpu;

    if (!c->magazines[idx]) {
        c->magazines[idx] = kmem_get_empty_mag(idx);
    }

    kmem_magazine_t* mag = c->magazines[idx];

    if (mag->top > 0) {
        return mag->slots[--mag->top];
    }

    kmem_magazine_swap_empty(idx);

    mag = c->magazines[idx];

    if (mag->top > 0) {
        return mag->slots[--mag->top];
    }

    return NULL;
}

void kfree(void* ptr) {
    if (!ptr) return;

    page_t* page = phys_to_page(v2p(ptr));

    if (page->obj_size == 0) {
        page_free(page, page->order);
        return;
    }

    __atomic_add_fetch(&page->free_count, 1, __ATOMIC_RELAXED);

    int idx = kmem_get_index(page->obj_size);
    struct cpu* c = curcpu;

    if (!c->magazines[idx]) {
        c->magazines[idx] = kmem_get_empty_mag(idx);
    }

    kmem_magazine_t* mag = c->magazines[idx];

    if (mag->top < KMEM_MAG_CAPACITY) {
        mag->slots[mag->top++] = ptr;
        return;
    }

    kmem_magazine_swap_full(idx);

    mag = c->magazines[idx];
    mag->slots[mag->top++] = ptr;
}

void* krealloc(void* ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    page_t* page = phys_to_page(v2p(ptr));

    size_t old_size = (page->obj_size == 0) ? 
                      (PAGE_SIZE << page->order) : page->obj_size;

    if (size <= old_size) {
        return ptr;
    }

    void* new_ptr = kmalloc(size);
    if (!new_ptr) return NULL;

    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);

    return new_ptr;
}

void* kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void* kmalloc_aligned(size_t size, size_t alignment) {
    size_t total_size = size + alignment + sizeof(void*);
    void* raw = kmalloc(total_size);
    if (!raw) return NULL;

    uintptr_t addr = (uintptr_t)raw + sizeof(void*);
    void* aligned = (void*)((addr + alignment - 1) & ~(alignment - 1));

    ((void**)aligned)[-1] = raw;

    return aligned;
}

void kfree_aligned(void* ptr) {
    if (!ptr) return;
    void* raw = ((void**)ptr)[-1];
    kfree(raw);
}

mem_initcall(kmem_init, PRIO_LAST);