#include <kernel/vma.h>
#include <kernel/kmem.h>
#include <kernel/lock.h>
#include <kernel/printf.h>
#include <string.h>

#define RED   0
#define BLACK 1

static inline int  rb_color(struct vm_area *v) { return v ? v->rb_color : BLACK; }
static inline void rb_set_color(struct vm_area *v, int c) { if (v) v->rb_color = c; }
static inline struct vm_area *rb_parent(struct vm_area *v) { return v ? v->rb_parent : NULL; }
static inline void rb_set_parent(struct vm_area *v, struct vm_area *p) { if (v) v->rb_parent = p; }
static inline struct vm_area *rb_grandparent(struct vm_area *v) { return rb_parent(rb_parent(v)); }
static inline struct vm_area *rb_uncle(struct vm_area *v) {
    struct vm_area *p = rb_parent(v);
    struct vm_area *g = rb_parent(p);
    if (!g) return NULL;
    return (p == g->rb_left) ? g->rb_right : g->rb_left;
}
static inline int rb_is_left_child(struct vm_area *v) {
    return rb_parent(v) && v == rb_parent(v)->rb_left;
}
static inline int rb_is_right_child(struct vm_area *v) {
    return rb_parent(v) && v == rb_parent(v)->rb_right;
}

static void rb_rotate_left(struct vm_area **root, struct vm_area *x) {
    struct vm_area *y = x->rb_right;
    x->rb_right = y->rb_left;
    rb_set_parent(y->rb_left, x);
    y->rb_left = x;

    struct vm_area *p = rb_parent(x);
    if (!p) {
        *root = y;
    } else if (x == p->rb_left) {
        p->rb_left = y;
    } else {
        p->rb_right = y;
    }
    rb_set_parent(y, p);
    rb_set_parent(x, y);
}

static void rb_rotate_right(struct vm_area **root, struct vm_area *x) {
    struct vm_area *y = x->rb_left;
    x->rb_left = y->rb_right;
    rb_set_parent(y->rb_right, x);
    y->rb_right = x;

    struct vm_area *p = rb_parent(x);
    if (!p) {
        *root = y;
    } else if (x == p->rb_right) {
        p->rb_right = y;
    } else {
        p->rb_left = y;
    }
    rb_set_parent(y, p);
    rb_set_parent(x, y);
}

static void rb_insert_fixup(struct vm_area **root, struct vm_area *z) {
    while (rb_color(rb_parent(z)) == RED) {
        struct vm_area *p = rb_parent(z);
        struct vm_area *g = rb_parent(p);

        if (p == g->rb_left) {
            struct vm_area *u = g->rb_right;
            if (rb_color(u) == RED) {
                rb_set_color(p, BLACK);
                rb_set_color(u, BLACK);
                rb_set_color(g, RED);
                z = g;
            } else {
                if (z == p->rb_right) {
                    z = p;
                    rb_rotate_left(root, z);
                    p = rb_parent(z);
                    g = rb_parent(p);
                }
                rb_set_color(p, BLACK);
                rb_set_color(g, RED);
                rb_rotate_right(root, g);
            }
        } else {
            struct vm_area *u = g->rb_left;
            if (rb_color(u) == RED) {
                rb_set_color(p, BLACK);
                rb_set_color(u, BLACK);
                rb_set_color(g, RED);
                z = g;
            } else {
                if (z == p->rb_left) {
                    z = p;
                    rb_rotate_right(root, z);
                    p = rb_parent(z);
                    g = rb_parent(p);
                }
                rb_set_color(p, BLACK);
                rb_set_color(g, RED);
                rb_rotate_left(root, g);
            }
        }
    }
    rb_set_color(*root, BLACK);
}

static void rb_transplant(struct vm_area **root,
                          struct vm_area *u, struct vm_area *v)
{
    struct vm_area *up = rb_parent(u);
    if (!up) {
        *root = v;
    } else if (u == up->rb_left) {
        up->rb_left = v;
    } else {
        up->rb_right = v;
    }
    rb_set_parent(v, up);
}

static struct vm_area *rb_minimum(struct vm_area *v) {
    while (v && v->rb_left) v = v->rb_left;
    return v;
}

static struct vm_area *rb_successor(struct vm_area *v) {
    if (v->rb_right) return rb_minimum(v->rb_right);
    struct vm_area *p = rb_parent(v);
    while (p && v == p->rb_right) {
        v = p;
        p = rb_parent(p);
    }
    return p;
}

static void rb_erase_fixup(struct vm_area **root,
                           struct vm_area *x,
                           struct vm_area *xp,
                           int x_is_left)
{
    while (x != *root && rb_color(x) == BLACK) {
        struct vm_area *w;
        if (x_is_left) {
            w = xp->rb_right;
            if (rb_color(w) == RED) {
                rb_set_color(w, BLACK);
                rb_set_color(xp, RED);
                rb_rotate_left(root, xp);
                w = xp->rb_right;
            }
            if (rb_color(w->rb_left) == BLACK && rb_color(w->rb_right) == BLACK) {
                rb_set_color(w, RED);
                x = xp;
                xp = rb_parent(x);
                if (xp) x_is_left = (x == xp->rb_left);
            } else {
                if (rb_color(w->rb_right) == BLACK) {
                    rb_set_color(w->rb_left, BLACK);
                    rb_set_color(w, RED);
                    rb_rotate_right(root, w);
                    w = xp->rb_right;
                }
                rb_set_color(w, rb_color(xp));
                rb_set_color(xp, BLACK);
                rb_set_color(w->rb_right, BLACK);
                rb_rotate_left(root, xp);
                x = *root;
                break;
            }
        } else {
            w = xp->rb_left;
            if (rb_color(w) == RED) {
                rb_set_color(w, BLACK);
                rb_set_color(xp, RED);
                rb_rotate_right(root, xp);
                w = xp->rb_left;
            }
            if (rb_color(w->rb_right) == BLACK && rb_color(w->rb_left) == BLACK) {
                rb_set_color(w, RED);
                x = xp;
                xp = rb_parent(x);
                if (xp) x_is_left = (x == xp->rb_left);
            } else {
                if (rb_color(w->rb_left) == BLACK) {
                    rb_set_color(w->rb_right, BLACK);
                    rb_set_color(w, RED);
                    rb_rotate_left(root, w);
                    w = xp->rb_left;
                }
                rb_set_color(w, rb_color(xp));
                rb_set_color(xp, BLACK);
                rb_set_color(w->rb_left, BLACK);
                rb_rotate_right(root, xp);
                x = *root;
                break;
            }
        }
    }
    rb_set_color(x, BLACK);
}

static void rb_erase_node(struct vm_area **root, struct vm_area *z) {
    struct vm_area *y = z;
    struct vm_area *x = NULL;
    struct vm_area *xp = NULL;
    int x_is_left = 0;
    int y_orig_color = rb_color(y);

    if (!z->rb_left) {
        x = z->rb_right;
        xp = rb_parent(z);
        x_is_left = (xp && z == xp->rb_left);
        rb_transplant(root, z, z->rb_right);
    } else if (!z->rb_right) {
        x = z->rb_left;
        xp = rb_parent(z);
        x_is_left = (xp && z == xp->rb_left);
        rb_transplant(root, z, z->rb_left);
    } else {
        y = rb_minimum(z->rb_right);
        y_orig_color = rb_color(y);
        x = y->rb_right;
        if (rb_parent(y) == z) {
            xp = y;
            x_is_left = 0;
        } else {
            xp = rb_parent(y);
            x_is_left = (x && x == xp->rb_left) || (!x && y == xp->rb_left);
            rb_transplant(root, y, y->rb_right);
            y->rb_right = z->rb_right;
            rb_set_parent(y->rb_right, y);
        }
        rb_transplant(root, z, y);
        y->rb_left = z->rb_left;
        rb_set_parent(y->rb_left, y);
        rb_set_color(y, rb_color(z));
    }

    if (y_orig_color == BLACK) {
        rb_erase_fixup(root, x, xp, x_is_left);
    }
}

void vma_insert(struct vm_area **root, struct list_node *head,
                struct vm_area *vma)
{
    struct vm_area *y = NULL;
    struct vm_area *x = *root;

    while (x) {
        y = x;
        if (vma->start < x->start)
            x = x->rb_left;
        else
            x = x->rb_right;
    }

    rb_set_parent(vma, y);
    vma->rb_left = NULL;
    vma->rb_right = NULL;
    vma->rb_color = RED;

    if (!y) {
        *root = vma;
        list_add(&vma->vma_list, head);
    } else if (vma->start < y->start) {
        y->rb_left = vma;
        list_add_tail(&vma->vma_list, &y->vma_list);
    } else {
        y->rb_right = vma;
        list_add(&vma->vma_list, &y->vma_list);
    }

    rb_insert_fixup(root, vma);
}

void vma_erase(struct vm_area **root, struct list_node *head,
               struct vm_area *vma)
{
    (void)head;
    list_del(&vma->vma_list);
    rb_erase_node(root, vma);
}

// 특정 addr의 VMA 찾기
struct vm_area *vma_find(struct vm_area *root, uintptr_t addr)
{
    struct vm_area *v = root;
    while (v) {
        if (addr < v->start)
            v = v->rb_left;
        else if (addr >= v->end)
            v = v->rb_right;
        else
            return v;
    }
    return NULL;
}

// 전 VMA 탐색
struct vm_area *vma_find_prev(struct vm_area *root, uintptr_t addr)
{
    struct vm_area *v = root;
    struct vm_area *candidate = NULL;
    while (v) {
        if (addr > v->start) {
            candidate = v;
            v = v->rb_right;
        } else {
            v = v->rb_left;
        }
    }
    return candidate;
}

struct vm_area *vma_find_first(struct vm_area *root, uintptr_t addr)
{
    struct vm_area *v = root;
    struct vm_area *candidate = NULL;
    while (v) {
        if (addr < v->start) {
            candidate = v;
            v = v->rb_left;
        } else if (addr >= v->end) {
            v = v->rb_right;
        } else {
            return v;
        }
    }
    return candidate;
}

struct vm_area *vma_alloc(uintptr_t start, uintptr_t end,
                          uint32_t flags,
                          struct vnode *vn, int64_t file_offset)
{
    struct vm_area *vma = kmalloc(sizeof(struct vm_area));
    if (!vma) return NULL;

    vma->start = start;
    vma->end = end;
    vma->flags = flags;
    vma->file_offset = file_offset;

    if (vn) {
        vma->vn = vn;
        vref(vn);
    } else {
        vma->vn = NULL;
    }

    vma->rb_parent = NULL;
    vma->rb_left = NULL;
    vma->rb_right = NULL;
    vma->rb_color = RED;
    list_init(&vma->vma_list);

    return vma;
}

void vma_free(struct vm_area *vma)
{
    if (vma->vn) {
        vput(vma->vn);
    }
    kfree(vma);
}

// VMA 분할: [start, addr) + [addr, end)
struct vm_area *vma_split(struct vm_area **root, struct list_node *head,
                          struct vm_area *vma, uintptr_t addr)
{
    if (addr <= vma->start || addr >= vma->end) return vma;

    struct vm_area *new_vma = vma_alloc(addr, vma->end, vma->flags,
                                        vma->vn, vma->file_offset);
    if (!new_vma) return NULL;

    new_vma->file_offset = vma->file_offset + (addr - vma->start);

    vma->end = addr;

    vma_insert(root, head, new_vma);
    return new_vma;
}

// 같은 flags/vn 을 가진 인접 VMA들과 병합 시도
void vma_merge_adjacent(struct vm_area **root, struct list_node *head,
                        struct vm_area *vma)
{
    // next와 병합 시도
    if (vma->vma_list.next != head) {
        struct vm_area *next = vma_next(vma);
        if (vma->end == next->start &&
            vma->flags == next->flags &&
            vma->vn == next->vn &&
            vma->file_offset + (int64_t)(vma->end - vma->start) == next->file_offset)
        {
            vma->end = next->end;
            vma_erase(root, head, next);
            vma_free(next);
        }
    }

    // prev와 병합 시도
    if (vma->vma_list.prev != head) {
        struct vm_area *prev = vma_prev(vma);
        if (prev->end == vma->start &&
            prev->flags == vma->flags &&
            prev->vn == vma->vn &&
            prev->file_offset + (int64_t)(prev->end - prev->start) == vma->file_offset)
        {
            prev->end = vma->end;
            vma_erase(root, head, vma);
            vma_free(vma);
            return;
        }
    }
}

void vma_remove_range(struct vm_area **root, struct list_node *head,
                      uintptr_t start, uintptr_t end)
{
    while (1) {
        struct vm_area *vma = vma_find_first(*root, start);
        if (!vma || vma->start >= end) break;

        struct vm_area *erase_vma = NULL;

        if (vma->start < start) {
            struct vm_area *right = vma_split(root, head, vma, start);
            if (right) {
                if (right->end > end) {
                    struct vm_area *tail = vma_split(root, head, right, end);
                    if (tail)
                        erase_vma = tail;
                    else
                        break;
                } else {
                    erase_vma = right;
                }
            } else {
                break;
            }
        } else if (vma->end > end) {
            struct vm_area *right = vma_split(root, head, vma, end);
            if (right) {
                erase_vma = vma;
            }
        } else {
            erase_vma = vma;
        }

        if (erase_vma) {
            vma_erase(root, head, erase_vma);
            vma_free(erase_vma);
        }
    }
}

// Demand Fault 만능해결꾼 (PF Fault 핸들러에서 호출됨)
uintptr_t vma_resolve_fault(struct proc *p, uintptr_t addr) {
    struct vnode *vn = NULL;
    int64_t file_offset = 0;
    uint32_t vma_flags = 0;

    uint64_t lk = spin_lock_irqsave(&p->p_vma_lock);
    struct vm_area *vma = vma_find(p->p_vma_root, addr);
    if (vma) {
        vma_flags = vma->flags;
        if (vma->vn) {
            vn = vma->vn;
            vref(vn);
            file_offset = vma->file_offset + (int64_t)(addr - vma->start);
        }
    }
    spin_unlock_irqrestore(&p->p_vma_lock, lk);

    // No VMA
    if (!vma)
        return 0;

    // Anonymous / No backing vnode
    if (!vn)
        return 0;

    // File-backed
    page_t *pg = page_alloc(0);
    if (!pg) {
        vput(vn);
        return 0;
    }
    uintptr_t phys = page_to_phys(pg);

    // 캐시먼저보기
    if (vma_flags & MMU_FLAGS_SHARED) {
        uintptr_t cached = vma_shared_lookup(vn, file_offset);
        if (cached) {
            page_free(pg, 0);
            vput(vn);
            return cached;
        }
    }

    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    if (!vn->ops || !vn->ops->read) {
        page_free(pg, 0);
        vput(vn);
        return 0;
    }
    int n = vn->ops->read(vn, phys_to_virt(phys), PAGE_SIZE, file_offset);
    if (n < 0) {
        page_free(pg, 0);
        vput(vn);
        return 0;
    }

    if (vma_flags & MMU_FLAGS_SHARED) {
        vma_shared_register(vn, file_offset, phys);
    }

    vput(vn);
    return phys;
}
