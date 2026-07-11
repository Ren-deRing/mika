# Doppio OS (SMP-ready)

## Communication
- Respond in Korean.
- Think/reason in English (use English for analysis, debugging thoughts).
- When user asks in Korean, answer in Korean.

## Build & Run

- Build: `make -j4` (all targets) or `make setup` (one-time musl build)  
  Use `-j4` not `-j$(nproc)` for stable linking.
- Run: `make run` → QEMU with debug output on serial.  
  Uses OVMF UEFI, 8G RAM, KVM, `-d int,cpu_reset`.
- Quick test with timeout: `timeout 30 make run`
- Clean: `make clean` (removes `build/` and `bin/`)

## Compiler

- Kernel: `clang -target x86_64-unknown-none-elf`, freestanding, `-mcmodel=kernel`, no SSE/MMX/AVX
- Userspace: `clang -target x86_64-pc-linux-musl`, PIE, `-ffreestanding`, linked against musl with `-dynamic-linker /lib/ld-musl-x86_64.so.1`
- Assembler: `nasm` (NASM syntax) for x86_64 assembly; kernel `.S` files use `as` syntax through clang

## Kernel Architecture

- `kernel/arch/x86_64/` — CPU init, GDT/IDT, interrupt stubs, syscall entry, MMU, context switch
- `kernel/proc/` — ELF loader (`elf.c`), process lifecycle (`proc.c`), scheduler (`sched.c`), exec (`exec.c`)
- `kernel/sys/` — syscall implementations:
  - `syscall.c` (dispatch), `sys_file.c` (read/write/ioctl/fcntl/flock/pipe/epoll/eventfd/signalfd/timerfd/ftruncate/fallocate)
  - `sys_fs.c` (mkdir/rmdir/mount/unlink/rename/access/stat/readlink/symlink/getdents64/memfd_create)
  - `sys_socket.c` (socket/bind/listen/accept/connect/sendmsg/recvmsg/sendto/recvfrom/setsockopt/socketpair)
  - `sys_poll.c` (poll/select)
  - `sys_mmap.c` (mmap/munmap/mprotect/brk/mremap)
  - `sys_proc.c` (fork/clone/exec/exit/wait4/set_tid_address)
  - `sys_getid.c` (getpid/uname/getuid/getgid/setsid/lchown)
  - `sys_sig.c` (signals), `sys_fd.c` (epoll/fcntl), `sys_pipe.c`
  - `sys_futex.c` (futex), `sys_timer.c` (posix timers), `sys_time.c` (clock_gettime/nanosleep/getrandom/etc)
  - `sys_shm.c`, `sys_sem.c`, `sys_drm.c`
- `kernel/vfs/` — ramfs, initrd, vnode operations
- `kernel/mm/` — kmem (slab allocator), VMA, kasan (KASAN runtime)
- `kernel/lib/` — lock (spinlock/rwsem), kprintf (serial), clock
- `kernel/misc/` — kstack (kernel stack allocator, remaining misc)
- Entry: `boot/` (Limine protocol), `kernel/arch/x86_64/early.c`, `kernel/generic.c`

## Notable Issues & Quirks

### ELF Loader Ignores Segment Permissions
`kernel/proc/elf.c` lines 68 and 208 map all `PT_LOAD` segments with `MMU_FLAGS_USER | MMU_FLAGS_WRITE | MMU_FLAGS_EXEC` regardless of `p_flags`. `.data.rel.ro` is mapped writable and executable. Compare with `kernel/arch/x86_64/mmu.c:483` which correctly checks `PF_W`/`PF_X` for the kernel's own mapping.

### No RELRO or PT_GNU_STACK handling
Kernel ELF loader doesn't handle `PT_GNU_RELRO` or `PT_GNU_STACK` — RELRO pages remain writable, and there's no NX stack enforcement.

### Copy-on-Write
Implemented in `kernel/arch/x86_64/mmu.c:870`, `mmu_protect_page`, and `vmm_map`. CoW uses `X86_PTE_COW` bit. When CoW is triggered, the page is copied and made writable. Race conditions in CoW are handled with re-validation of PTE.

### Page Table Locking
`mmu_get_lock` returns per-PML4 spinlock. All `mmu_map/unmap/protect` operations take this lock. `vmm_get_pte` does NOT take the lock.

### DMA / framebuffer
Simple linear framebuffer mapped from boot info. GPU-related syscalls in `sys_drm.c` (likely stubs).

## Testing

Test suite in `user/init/init.c` — single process (PID 1), single-threaded tests, some with fork/clone. Tests use `printf`/`fflush` via musl. To add a test: add function, add call in `main()`. Tests run sequentially until crash.

## VFS
- `vfs/ramfs.c` — in-memory filesystem
- `vfs/initrd.c` — initrd cpio archive loader
- `vfs/vfs.c` — path resolution, file descriptor management
- `vfs/vnode.c` — vnode abstraction with ops table

## Remote Debugging
- To modify kernel: edit files, `make -j4`, check build
- To modify test program: edit `user/init/init.c`, `make -j4`
- To add kernel debug output: `dprintf("format", args)` in kernel; appears on QEMU serial console
- To run kernel: make run with timeout
- Page fault handler at `kernel/arch/x86_64/mmu.c:870` prints CR2, RIP, error code
- General exception handler at `kernel/arch/x86_64/idt.c:139` prints vector number, RIP, error code
- SIGSEGV delivery: `kernel/sys/sys_sig.c:56` (`check_signals`)
- Kernel is compiled with `-fomit-frame-pointer` (default at `-O2`), so RBP-based stack traces in panic dumps are unreliable.