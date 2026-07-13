# Mika OS (SMP-ready)

## Communication
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
  - `sys_socket.c` (socket/bind/listen/accept/connect/sendmsg/recvmsg/sendto/recvfrom/setsockopt/getsockopt/socketpair)
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

## SKIPPED Issue Resolution Plan

### Phase 1 — Immediate (Tier 1)

#### 1. `write_unlock` plain store → atomic release
- File: `include/kernel/lock.h:68-71`
- Change: `__sync_synchronize(); lock->val = 0;` → `__atomic_store_n(&lock->val, 0, __ATOMIC_RELEASE);`

#### 2. `sched_boost`'s `last_boost_tick` → per-CPU
- File: `kernel/proc/sched.c:337`
- Change: Add `uint64_t last_boost_tick` to `struct cpu`, use `curcpu->last_boost_tick`
- Add field in `include/kernel/cpu.h`

#### 3. `do_softirq` max_loop check bug
- File: `kernel/misc/softirq.c:38`
- Change: Replace `max_loop < 0` condition with a loop-exceeded flag (currently always true, logs every softirq)

### Phase 2 — Medium Complexity (Tier 2)

#### 4. `vmm_unmap` page-table refcount leak
- File: `kernel/arch/x86_64/mmu.c:515-580`
- Change: Decrement parent page ref_count when freeing PT/PD/PDPT
  - Free tables[3] → tables[2].ref_count--
  - Free tables[2] → tables[1].ref_count--
  - Free tables[1] → tables[0].ref_count--
- Align pattern with `vmm_free_pt` and `vmm_destroy_map`

#### 5. `rwlock` writer starvation
- File: `include/kernel/lock.h:39-71`
- Change: Redesign val bit layout to `[31:writer_locked][30:writer_pending][29-0:reader_count]`
- `write_lock` sets writer-pending bit before CAS → new readers also check pending bit

#### 6. `rwsem` writer starvation
- File: `kernel/lib/lock.c:109-125`
- Change: `down_read` checks wait queue for pending writers before attempting `count >= 0` acquire

### Phase 3 — SMEP/SMAP + sys_write

#### 7a. `sys_write` bounce buffer
- File: `kernel/sys/sys_file.c:103-149`
- Change: Same pattern as `sys_read` — `copy_from_user(kbuf)` → `vfs_write(fd, kbuf, chunk)`
- Ensure vnode `.write` implementations receive kernel pointers (ramfs/ext2/blockdev)

#### 7b. SMEP/SMAP CR4 activation
- File: `kernel/arch/x86_64/early.c` (fpu_init or dedicated function)
- Change: Set CR4 bit 20 (SMEP), bit 21 (SMAP)
- `copy_from_user` uses `mmu_translate` → `phys_to_virt`, safe under SMAP

### Phase 4 — Permission System

#### 8. VFS permission check
- VFS wrapper functions: `vfs_permission(mode, uid, gid, bits)`, `vfs_may_open(vn, flags, cred)`
- Add R/W/X checks in `vfs_open` at open time
- Implement setter syscalls: `sys_umask`, `sys_setuid/setgid/seteuid/setegid`, `sys_chmod/fchmod`, `sys_chown/fchown`
- Extract existing `sys_access` logic into reusable `vfs_permission`

### Phase 5 — ext2 SMP Locking

#### 9. ext2 SMP protection
- Add `spinlock_t fs_lock` to `struct ext2_fs` → protects bitmap/BGD/inode alloc-free operations
- vnode-level locking: `vn->rwlock` + `vn->io_mutex` (matching ramfs pattern)
- Fix in-memory inode staleness: `ext2_vnode` hash table (share vnode by ino)
- Affected: `ext2_alloc_block`, `ext2_alloc_inode`, `ext2_free_*`, `ext2_add/remove_dirent`, `ext2_create/remove/rmdir`, `ext2_write`, `ext2_truncate_blocks`