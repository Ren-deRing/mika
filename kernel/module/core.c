#include <kernel/module.h>
#include <kernel/kmem.h>
#include <kernel/lock.h>
#include <kernel/printf.h>
#include <kernel/symbol.h>
#include <kernel/init.h>
#include <string.h>
#include <uapi/errno.h>

static struct module *g_modules = NULL;
static spinlock_t module_lock;

void module_list_lock(void) {
    spin_lock(&module_lock);
}

void module_list_unlock(void) {
    spin_unlock(&module_lock);
}

static void module_list_init(void) {
    spin_lock_init(&module_lock);
}

struct module *module_find(const char *name) {
    spin_lock(&module_lock);
    struct module *m = g_modules;
    while (m) {
        if (__builtin_strcmp(m->name, name) == 0) {
            spin_unlock(&module_lock);
            return m;
        }
        m = m->next;
    }
    spin_unlock(&module_lock);
    return NULL;
}

static void module_list_add(struct module *mod) {
    mod->next = g_modules;
    g_modules = mod;
}

static void module_list_remove(struct module *mod) {
    struct module **pp = &g_modules;
    while (*pp) {
        if (*pp == mod) {
            *pp = mod->next;
            mod->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

int module_load(struct module *mod) {
    if (!mod || !mod->init) return -EINVAL;

    spin_lock(&module_lock);
    struct module *existing = g_modules;
    while (existing) {
        if (__builtin_strcmp(existing->name, mod->name) == 0) {
            spin_unlock(&module_lock);
            return -EEXIST;
        }
        existing = existing->next;
    }

    mod->state = MODULE_STATE_COMING;
    module_list_add(mod);
    spin_unlock(&module_lock);

    int ret = mod->init();
    if (ret < 0) {
        spin_lock(&module_lock);
        module_list_remove(mod);
        spin_unlock(&module_lock);
        module_free(mod->base, mod->size);
        kfree(mod);
        return ret;
    }

    mod->state = MODULE_STATE_LOADED;
    dprintf("[module] '%s' loaded at %p (%zu bytes)\n", mod->name, mod->base, mod->size);
    return 0;
}

int module_unload(const char *name) {
    spin_lock(&module_lock);
    struct module *m = g_modules;
    while (m) {
        if (__builtin_strcmp(m->name, name) == 0) break;
        m = m->next;
    }

    if (!m) {
        spin_unlock(&module_lock);
        return -ENOENT;
    }

    if (m->refcount > 0) {
        spin_unlock(&module_lock);
        return -EBUSY;
    }

    m->state = MODULE_STATE_GOING;
    module_list_remove(m);
    spin_unlock(&module_lock);

    if (m->exit) m->exit();

    module_free(m->base, m->size);
    dprintf("[module] '%s' unloaded\n", m->name);
    kfree(m);
    return 0;
}

late_initcall(module_list_init);
