#pragma once

#include <stdint.h>
#include <kernel/list.h>
#include <kernel/mmu.h>

struct proc;
struct vnode;

struct vm_area {
    uintptr_t       start;
    uintptr_t       end;
    uint32_t        flags;
    int64_t         file_offset;

    struct vnode   *vn;

    struct vm_area *rb_parent;
    struct vm_area *rb_left;
    struct vm_area *rb_right;
    int             rb_color;

    struct list_node vma_list;
};

void vma_insert(struct vm_area **root, struct list_node *head,
                struct vm_area *vma);
void vma_erase(struct vm_area **root, struct list_node *head,
               struct vm_area *vma);

struct vm_area *vma_find(struct vm_area *root, uintptr_t addr);
struct vm_area *vma_find_prev(struct vm_area *root, uintptr_t addr);
struct vm_area *vma_find_first(struct vm_area *root, uintptr_t addr);

static inline struct vm_area *vma_first(struct list_node *head) {
    if (list_empty(head)) return NULL;
    return list_entry(head->next, struct vm_area, vma_list);
}
static inline struct vm_area *vma_last(struct list_node *head) {
    if (list_empty(head)) return NULL;
    return list_entry(head->prev, struct vm_area, vma_list);
}
static inline struct vm_area *vma_next(struct vm_area *vma) {
    return list_entry(vma->vma_list.next, struct vm_area, vma_list);
}
static inline struct vm_area *vma_prev(struct vm_area *vma) {
    return list_entry(vma->vma_list.prev, struct vm_area, vma_list);
}

#define vma_for_each(pos, head) \
    for (pos = list_entry((head)->next, struct vm_area, vma_list); \
         &pos->vma_list != (head); \
         pos = list_entry(pos->vma_list.next, struct vm_area, vma_list))

struct vm_area *vma_alloc(uintptr_t start, uintptr_t end,
                          uint32_t flags,
                          struct vnode *vn, int64_t file_offset);
void vma_free(struct vm_area *vma);

struct vm_area *vma_split(struct vm_area **root, struct list_node *head,
                          struct vm_area *vma, uintptr_t addr);
void vma_merge_adjacent(struct vm_area **root, struct list_node *head,
                        struct vm_area *vma);
void vma_remove_range(struct vm_area **root, struct list_node *head,
                      uintptr_t start, uintptr_t end);

void __vma_resolve_fault(struct proc *p, uintptr_t addr,
                          struct vnode **vn_out, int64_t *offset_out,
                          uint32_t *flags_out);
uintptr_t vma_resolve_fault(struct proc *p, uintptr_t addr, uint32_t *flags_out);

uintptr_t vma_shared_lookup(struct vnode *vn, int64_t offset);
void vma_shared_register(struct vnode *vn, int64_t offset, uintptr_t phys);
void vma_shared_unregister(struct vnode *vn, int64_t offset);
void vma_shared_cleanup(void);
