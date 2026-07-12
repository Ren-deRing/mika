# Doppio OS (SMP-ready)

## Communication
## Security notes
- **SMEP/SMAP**: Do NOT enable in `early.c` until user-space pointer validation (`copy_from_user` bounce buffer) is added to ALL vnode write paths. Without the bounce buffer, SMAP would fault on every `ext2_write`/`ramfs_write` call.
- **Signal semantics**: Signal actions are per-`struct thread`, not per-`struct proc`. This is intentional for now — `CLONE_SIGHAND` is not implemented, so each thread has its own handler table.
- **`st_ino` field**: Currently leaks kernel vnode pointers to userspace. Fix: use a monotonically-increasing counter or hash of the pointer instead.

## Audit history

2026-07-12: Full audit (see Known Issues above).

## Response convention
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

## Testing

Test suite in `user/init/init.c` — single process (PID 1), single-threaded tests, some with fork/clone. Tests use `printf`/`fflush` via musl. To add a test: add function, add call in `main()`. Tests run sequentially until crash.

## Known Issues (2026-07-12 Audit)

### CRITICAL (fix pending)
- **SMEP/SMAP disabled**: CR4 lacks bit20/21. Cannot enable until a bounce-buffer write path exists (see Security notes).
- **`sys_write` bypasses `copy_from_user`**: passes `user_buf` directly to `vfs_write` → ext2/ramfs `memcpy` from user ptr. Works because SMAP is off, but breaks under SMAP.
- **`sys_kill`/`sys_tgkill`**: no UID permission check — any process can kill/tkill any other process.
- **ext2 SMP locking**: no per-filesystem lock → bitmap/inode/dir corruption under concurrent access.
- **`proc_put` TOCTOU**: two concurrent `proc_put` calls can double-free `struct proc` (use-after-free).
- **`vfs_open` `O_CREAT|O_EXCL`**: silently overwrites existing file instead of returning `-EEXIST`.
- **`child_name` buffer overflow**: `vfs.c:367` `child_name[NAME_MAX]` (255) overflows via `strcpy(child_name, clean_path)` when `clean_path` is 255 chars.
- **`ext2_set_block_ptr` indirect/dind/tind writes wrong block**: `tmp` reused after `ext2_read_block`, so `tmp[ind_idx]` no longer holds the block number (lines 361-369, 409-425).

### HIGH (fix pending)
- **Syscall table misassignments**: `[55]=sys_lchown` (should be `getsockopt`), `[273]=sys_eventfd` (should be `set_robust_list`). `getsockopt` silently no-ops; `pthread_create` doesn't register robust list.
- **`st_ino`/`d_ino` leaks kernel pointer**: vnode virtual addresses returned to userspace as inode numbers → KASLR bypass.
- **`O_APPEND` ignored**: `vfs_write` always writes at `f->f_pos`, not at file end.
- **`O_TRUNC` ignored**: opening existing file with `O_TRUNC` does not truncate.
- **`setup_user_stack` buffer overflow**: `TMP_STACK_SIZE=16384` — arg/env/auxv exceeding 16KB corrupts kernel heap.
- **`sys_epoll_wait` integer overflow**: `sizeof(epoll_event) * maxevents` wraps; heap buffer overflow.
- **`vmm_unmap` page-table leak**: intermediate tables freed without parent-refcount update.
- **Busy filesystem umount**: `vfs_umount` doesn't check open files → use-after-free.
- **`mremap`/`sys_mmap` ignores flags**: `(void)flags;` — `MAP_SHARED`/`MAP_PRIVATE`/`MAP_ANONYMOUS` not enforced.
- **CPIO initrd no bounds-check**: malformed cpio can cause arbitrary kernel memory read/write.

### MEDIUM (fix pending)
- `CLONE_VM`/`CLONE_SIGHAND`/`CLONE_FILES` defined but never checked in `sys_clone`.
- `sched_yield` (syscall 24) not implemented.
- `thread_create` modifies `p_threads` without `p_lock` — data race with `sys_exit`.
- `rwlock`/`rwsem` writer starvation — readers can starve writers indefinitely.
- Framebuffer mmap creates no VMA — tracked only in page table.
- `add_wait_queue` uses `spin_lock` (not `_irqsave`) → potential IRQ deadlock.
- PID wraps `INT_MAX` with no collision check.
- `ext2_truncate_blocks` only frees direct blocks — indirect blocks leak.
- `ext2_create`/`remove` can leak inodes/blocks on error paths.
- `blk_cache_flush` holds spinlock during disk I/O.
- `pivot_root` modifies mount list without `mount_lock`.
- `FBIOGET_FSCREENINFO` leaks physical framebuffer address via `smem_start`.
- Block-cache has no invalidation mechanism — stale data after direct disk writes.
- `ext2_write` computes `i_blocks` from `i_size`, not actual allocation count.
- `sys_access`/`faccessat` ignores mode bits — only checks path existence.
- `ramfs_rmdir` TOCTOU between empty-check and remove.
- `do_softirq` drops `RCU_SOFTIRQ` on loop-limit re-raise.
- `sched_boost`'s `last_boost_tick` is a global `static` — CPU race on read-modify.
- `write_unlock` (rwlock) uses plain store, not atomic release.

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