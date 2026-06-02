#include <boot/bootinfo.h>

#include <kernel/init.h>
#include <kernel/cpu.h>
#include <kernel/printf.h>
#include <kernel/mmu.h>
#include <kernel/kmem.h>
#include <kernel/intc.h>
#include <kernel/clock.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/lock.h>
#include "string.h"
#include <kernel/exec.h>
#include <kernel/fs/vfs.h>
#include <uapi/fcntl.h>

bool ap_release = false;

static void do_early_initcalls(void) {
    for (initcall_t* call = __early_initcall_start; call < __early_initcall_end; call++) {
        if (!call || !*call) continue;
        (*call)();
    }
}

static void do_late_initcalls(void) {
    for (initcall_t* call = __late_initcall_start; call < __late_initcall_end; call++) {
        if (!call || !*call) continue;
        (*call)();
    }
}

void do_ap_initcalls(void) {
    for (initcall_t* call = __ap_initcall_start; call < __ap_initcall_end; call++) {
        if (!call || !*call) continue;
        (*call)();
    }
}

void generic_main(void) {
   do_late_initcalls();

    ap_release = true;
    __sync_synchronize();

    uint32_t* fb_ptr = (uint32_t*)g_boot_info.fb.fb_addr;
    uint32_t width = g_boot_info.fb.width;
    uint32_t height = g_boot_info.fb.height;
    uint32_t pixels_per_line = g_boot_info.fb.pitch / 4;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) fb_ptr[y * pixels_per_line + x] = 0xF3E0ED;
    }
    for (uint32_t y = height/2 - 50; y < height/2 + 50; y++) {
        for (uint32_t x = width/2 - 50; x < width/2 + 50; x++) fb_ptr[y * pixels_per_line + x] = 0xFFFFFF;
    }

    int fd;
    if (vfs_open("/bin/init", O_RDONLY, 0, &fd) == 0) {
        off_t size = vfs_lseek(fd, 0, SEEK_END);
        vfs_lseek(fd, 0, SEEK_SET);

        if (size > 0) {
            void* elf_buf = kmalloc(size);
            if (elf_buf) {
                vfs_read(fd, elf_buf, size);
                vfs_close(fd);

                char *init_argv[] = {"/bin/init", NULL};
                char *init_envp[] = {NULL};

                proc_exec(elf_buf, init_argv, init_envp);
                kfree(elf_buf);
            }
        }
    } else {
        dprintf("Error: /bin/init not found\n");
    }

    arch_irq_enable();

    while(1) { // idle
        arch_irq_enable();
        arch_halt();
    }
}

void generic_entry() {
    arch_irq_disable();

    early_init(g_boot_info.smp.bsp_hw_id);
    do_early_initcalls();

    extern void arch_start_thread(struct thread *t);
    arch_start_thread(curcpu->idle);
}

void ap_entry(CoreInfo* info) {
    arch_irq_disable();

    ap_early_init(info->hw_id);
    
    while (!ap_release) {
        arch_pause();
    }

    __sync_synchronize();
    do_ap_initcalls();

    for (;;) arch_halt();
}