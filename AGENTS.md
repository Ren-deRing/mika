# Doppio OS

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

### Current Bug: fflush #GP (stdout corruption)
A deterministic user-space #GP occurs at `fflush+0xc` (musl libc) after enough `printf`/`fflush` calls. The `stdout` pointer (`.data.rel.ro`) gets overwritten with `0xf7f6f5f4f3f2f1f0` (sequential bytes f0-f7). Corruption is cumulative, user-space only (confirmed by kernel watch at every syscall entry). When protecting stdout's page with `mprotect(..., PROT_READ)`, the crash changes location but remains #GP — suggesting the corruption mechanism is more complex than a direct write to stdout's page.

### spin_lock_irqsave on p_lock causes #GP in vmm_destroy_map
`cli`/`popfq` (`arch_irq_save`/`arch_irq_restore`)가 KVM guest에서 page table page를 손상시킴.
- 증상: `proc_alloc_fd`에 `spin_lock_irqsave` 사용 시 ~80% #GP at `vmm_destroy_map+0x128` (mov (%r12,%r9,8),%r10 — non-canonical page table pointer)
- 영향 범위: `p_lock` 접근에서 `spin_lock_irqsave`를 `spin_lock`으로 사용. 타이머 핸들러는 `spin_trylock` 사용.
- 남은 `spin_lock_irqsave` 사용처 (sys_close, fork, exec, fdget, page fault handler): crash 관찰 안 됨
  - 추정: crash는 호출 빈도에 의존적 (proc_alloc_fd가 가장 많이 호출됨)
- `spin_lock`은 UP에서 충분히 안전 (타이머가 `spin_trylock`만 사용 + no SMP contention)
- 원인 미상

### Duplicated FAIL macro
`user/init/init.c:43-44`: `FAIL` is defined twice identically.

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

## Refactoring Plan (Concurrency / Memory Safety + MI/MD)

Overall goal: adopt Linux-like fdget/fdput, atomic_inc_not_zero, and sleepable rwsem for VMA tree, plus clear MI/MD separation for multi-arch.

### Verification
각 Step은 아래 두 단계로 검증하며, **unrelated crash는 Known Issues에 기록하고 계속 진행**:
1. **`make -j4`** (build) + **`timeout 10 make run`** (부트 + init 첫 출력) — 필수 (HEAD가 아닌 TAIL 추천)
2. **`timeout 30 make run`** (test suite 전면) — Step의 직접적인 변경으로 기존 test가 FAIL하면 수정. pre-existing bug / unrelated crash는 AGENTS.md `## Known Issues`에 기록하고 skip

### Step 0: Infrastructure
- `include/kernel/atomic.h` (new): `atomic_inc_not_zero(uint32_t *ptr)` — CAS-based, returns 1 if value was >0 and was incremented
- `include/kernel/lock_types.h` + `lock.h` + `kernel/misc/lock.c`: sleepable `rw_semaphore_t` (mutex-style wait queue, reader/writer count)
- `include/kernel/fs/file.h`: declare `fdget(int fd)` / `fdput(struct file *f)` (defined in `vfs.c:710-722` but no header decl)

### Step 0b: Syscall file reorganization ✅ (build + boot verified)

**목표**: `kernel/sys/`의 거대한 파일들을 단일 책임 파일로 분할.

**분할 결과**:

| 원본 | 분할 후 |
|------|---------|
| `sys_socket.c` (1330→1040) | socket ops 유지; **poll/select** → `sys_poll.c`; **ftruncate/fallocate** → `sys_file.c` |
| `sys_file.c` (888→618) | read/write/ioctl/fcntl/flock/epoll/eventfd/signalfd/timerfd + ftruncate/fallocate 유지 |
| | **mkdir/rmdir/mount/unlink/rename/access/stat/readlink/symlink/getdents64/memfd_create** → `sys_fs.c` |
| `sys_sync.c` (500) | **삭제** — futex → `sys_futex.c`, timer → `sys_timer.c`, time/api → `sys_time.c` |
| `sys_proc.c` (709→638) | fork/clone/exec/exit/wait4 유지; **getpid/uname/getuid/getgid/etc** → `sys_getid.c` |
| `sys_drm.c` (606) | real code 유지 (EINVAL stubs는 실제 ioctl 구현이어서 유지) |
| `sys_mem.c` (778) | **→ `sys_mmap.c`** (rename) |

### Phase 1.5: MI/MD Separation ✅ (build + boot verified)

**목표**: MI/MD 경계를 명확히 하고, inline asm과 `p2v`/`v2p`를 MI 코드에서 제거하여 멀티아치 기반 마련.

**완료 항목**:
- `include/kernel/mmu_types.h` (new): `pgprot_t`, `phys_addr_t`, `vm_flags_t`, `ARCH_KERNEL_BASE`
- `include/kernel/mmu.h`: `phys_to_virt()`/`virt_to_phys()` 추가, p2v/v2p를 alias로 유지, `KERNEL_BASE` → `ARCH_KERNEL_BASE`
- `include/kernel/cpu.h`: `arch_fpu_save/restore`, `arch_get_random_seed`, `arch_panic_halt`, `arch_get_user_addr_limit`, `arch_get_ticks` 선언
- `kernel/arch/x86_64/cpu.c`: 위 arch hooks 구현 (xsaveq/fxsave, rdtsc, cli;hlt 등)
- MI 코드 inline asm 제거: `sys_proc.c` (xsaveq/fxsave → arch_fpu_save), `sys_time.c` (rdtsc → arch_get_random_seed, mfence → __sync_synchronize, sys_arch_prctl → `#ifdef __x86_64__`), `kasan.c` (cli;hlt → arch_panic_halt), `syscall.c` (USER_ADDR_LIMIT → arch_get_user_addr_limit())
- 전역 rename: `p2v(` → `phys_to_virt(`, `v2p(` → `virt_to_phys(` (MI + MD 모든 .c 파일)
- 디렉토리 정리: `kernel/mm/` (kmem/vma/kasan), `kernel/lib/` (lock/kprintf/clock)
```
kernel/
  mm/                           # MI memory management
    kmem.c, vma.c, kasan.c
  lib/                          # MI library
    lock.c, kprintf.c, clock.c
  proc/                         # MI process
    elf.c, exec.c, proc.c, sched.c, signal.c
  fs/                           # MI VFS
    vfs.c, vnode.c, file.c, ramfs.c, initrd.c
  sys/                          # MI syscalls (Step 0b 정리 완료)
  arch/x86_64/                  # MD
  misc/                         # remaining misc (kstack.c)
```

### Step 1: Phase 1 — fdget/fdput 전면 도입 (~30 sites) ✅ (build + boot + 10/10 run verified)
- **`proc_alloc_fd`**: `file_ref(f)` 추가 → fd leak 근본 원인 해결
- **`sys_pipe2`** / **`sys_socketpair`** rollback `p_fd_table[...]=NULL`: `p_lock` 안에서 수행
- All unlocked `p_fd_table[fd]` reads → `fdget(fd)` / `fdput(f)` pattern
  - `sys_file.c`: read/write/fstat/flock/ioctl/fcntl
  - `sys_mem.c`: mmap fb0 check
  - `sys_fd.c`: signalfd/timerfd_settime/timerfd_gettime
  - `sys_epoll.c`: check_fd_readiness/epoll_ctl/epoll_wait
  - `sys_socket.c`: setsockopt/listen/accept/connect/sendto/recvfrom/poll_check_fd
  - `initrd.c`: unlocked read

### Step 2: Phase 1b — Error rollback + ELF permission + socket cleanup fix
- **`elf.c:68,208`**: 모든 PT_LOAD를 `MMU_FLAGS_USER | WRITE | EXEC`로 매핑하던 것을 `p_flags` 반영으로 수정 (2줄: `p_flags & PF_W ? MMU_FLAGS_WRITE : 0`, `p_flags & PF_X ? MMU_FLAGS_EXEC : 0`)
- **`sys_socket.c:378-379`**: `sock_free(s)` 제거 (vput이 이미 sock_free 호출) → double-free fix
- **`sys_socket.c:553-556`**: `sock_free(server_chan)` 제거 → double-free fix
- **`sys_socket.c:1227-1228`**: cleanup 순서 변경 (peer 링크 먼저 끊기) → UAF fix
- **`sys_proc.c:155-167`**: fork VMA clone 실패 시 지금까지 만든 VMAs 정리 → leak fix
- **`sys_proc.c:508-528`**: execve 실패 시 stack VMA rollback

### Step 3: Phase 2 — vput TOCTOU + atomic_inc_not_zero
- **`vref`**: lockless `__atomic_fetch_add` → guard with `atomic_inc_not_zero`
- **`vget`**: `ref_count==0`이면 increment 생략, `-EBUSY` return
- **`pipe_inactive`** / **`eventfd_inactive`** / etc: `__atomic_exchange_n(&vp->data, NULL)` for atomic handoff
- **`vput`**: set `vp->v_reclaimable = 1` BEFORE releasing lock; after inactive, re-check ref_count AND v_reclaimable

### Step 4: Phase 3 — VMA lock → rw_semaphore
- `struct proc.p_vma_lock: spinlock_t` → `rw_semaphore_t`
- Page fault handler: `down_read()` (sleepable, VMA locked during entire fault resolution)
- `sys_mmap/munmap/brk/mprotect/mremap`: `down_write()` / `up_write()`
- `sys_fork/exec`: `down_read()` on parent
- Lock order: `rwsem` → `mmu_lock(spinlock)` → OK
- **Effect**: VMA→PTE TOCTOU 제거 (rwsem으로 VMA가 fault 내내 보호됨)

### Step 5: Phase 3b — VMA edge case fix
- `mprotect`: `vma_split()` 실패 시 rollback 추가 (sys_mem.c:413-421)
- `mremap` move mode: partial failure → rollback으로 재설계 (atomic move)
- `brk`: 기존 mmap과 overlap 체크 (sys_mem.c:345-361)
- `munmap`: VMA 제거 전에 pages 먼저 unmap (sys_mem.c:320-326)
- `mmap` /dev/fb0: VMA 생성 누락 수정

### Step 6: Wait queue + softirq infrastructure
**가장 큰 구조적 결함**: 모든 IPC가 busy-poll (`while(empty) { unlock; thread_yield(); lock; }`).
- **`include/kernel/wait.h`** (new): `wait_queue_head_t`, `wait_event_interruptible(wqh, condition)`, `wake_up(wqh)`, `wake_up_all(wqh)`
- **Pattern**: caller가 condition을 만족할 때까지 wait queue에서 sleep. writer가 data를 쓴 후 `wake_up()` → sleeper 재개.
- **`pipe_read/write`**: busy-poll → `wait_event_interruptible` + `wake_up`으로 교체
- **`eventfd/signalfd/timerfd`**: 동일 패턴 교체
- **`sys_wait4`**: child exit 시 `wake_up` (현재는 부모가 주기적으로 poll)
- **`epoll`**: `check_fd_readiness` 대신 `poll_wait()` callback 등록 → epoll이 O(1) wakeup 가능
- **Softirq**: `include/kernel/softirq.h` (new) + `kernel/misc/softirq.c` — TIMER_SOFTIRQ 정의, `raise_softirq()`, `do_softirq()`, per-CPU `irq_nest_count`
  - `posix_timers_tick()`을 IRQ context에서 softirq로 이동 (매 tick마다 모든 프로세스/타이머 스캔 — 현재 IRQ latency의 주범)
  - `arch_timer_handler`: tick++ + raise_softirq + EOI만 남기고 경량화
  - `keyboard_handler`: 변경 불필요 (이미 단순 FIFO push만 함)
- **irq.S 수정**: `do_softirq()`를 IRQ return path에 추가 (before `t_need_resched` check)
- **Effect**: kernel I/O latency이 scheduler timeslice에서 microsecond 단위로 개선 + worst-case IRQ latency 1000x 감소. **Phase 4와 독립적이므로 순서 무관.**

### Step 7: Phase 4 — copy_to_user while holding spinlock
- (wait queue 도입 이후, pipe/eventfd/copy 구조가 더 간단해짐 — `wait_event` 후 lock 밖에서 copy)
- **`pipe_read`**: lock → while empty && alive → wait_event → lock → copy to local → unlock → copy_to_user
- **`pipe_write`**: `copy_from_user` to local → lock → write to buffer → wake_up → unlock
- **`eventfd_read`** / **`signalfd_read`** / **`timerfd_read`**: same pattern
- **`sys_wait4`** (`sys_proc.c:604-608`): `copy_to_user`를 `p_lock` 밖으로 이동

### Step 8: Lock order 문서화 + 데드락 수정 + dead lock 제거
- **`sock_close` / `sock_free` ABBA 데드락 수정** (`sys_socket.c:293-301`): `s->lock` → `peer->lock` 순서를 소켓 주소 기준으로 정렬 or trylock
- **`p_vm_lock` 삭제** (proc.h:121): `mmu_get_lock()`으로만 접근, 필드 자체 불필요
- **`vn->lock` 삭제** (vnode.c:68, vnode.h): 초기화만 하고 사용 안 함
- **`f->f_lock` 삭제** (file.c:11): `file_lock()` 아무도 안 부름

### Step 9: Return convention + dead code cleanup
- **`copy_to_user/from_user`** (`syscall.c:23-93`): `-1` → `-EFAULT` 반환값 수정
- **`vget` 삭제** (vnode.c:158-169): dead code, `vfs_cached_lookup`가 inline으로 동일 로직 수행
- **`user/init/init.c:43-44`**: FAIL 중복 정의 제거

### Step 10 (Future): Thread model + signal queueing
**두 번째로 큰 구조적 결함**: signal이 bitmask + first-thread-only delivery.
- `sys_kill(pid, sig)`: thread group 전체를 순회하며 signal을 not-blocking thread에 delivery
- `sigprocmask`: stubs → 실제 구현 (per-thread signal mask)
- Signal queue: bitmask → `struct sigqueue` linked list + `siginfo_t` per-signal (SIGRTMIN용)
- `t_sig_actions`: per-thread → shared `sighand_struct` (CLONE_SIGHAND 구현)
- fork/clone: 중복 코드 통합 (공통 `copy_process` 함수)

### Step 11: VFS quick wins (Structural cleanup)
**문제**: 8번 중복된 path splitting + hash table 불변식 깨짐 + abstraction violation
- **`split_parent_child()` helper 추출**: `vfs.c`에서 8회 중복된 strrchr('/') 패턴(각 14줄)을 단일 helper로 통합 → ~120줄 제거
- **`vfs_hash_remove()` 추가** (`vnode.c`): hash table에서 vnode 제거 함수 (현재는 vput에서만 제거)
- **`vfs_rename` 수정**: rename 시 `vfs_hash_remove(old)` + `vfs_hash_insert(new)` 호출 → cross-directory rename 후에도 cache 일관성 유지
- **`vfs_mkdir` 수정**: `vfs_hash_insert` 누락 수정 (현재 mkdir 후 cache miss)
- **`vfs_bind` 처리**: abstraction violation (ramfs_node에 직접 접근) → `#if 0` 또는 mount 기반으로 재설계
- **`initrd.c:mkdir_p`** → VFS public helper로 승격
- **`do_file_io` helper**: vfs_read/vfs_write 중복 제거

### Step 12: Mount infrastructure (Optional)
- **`struct mount` 정의** (`include/kernel/fs/vnode.h`): mnt_root, mnt_parent, mnt_flags
- **Mount table** 추가: 전역 list of `struct mount`
- **`vfs_lookup_impl` 수정**: 각 component resolve 후 mount point 체크 + traverse
  - `curr->mnt != NULL`이면 `curr = curr->mnt->mnt_root`로 follow
  - ".."에서 mount root escape 처리
- **`vfs_mount`/`vfs_umount`** syscall stub → 구현
- **`vfs_bind` 대체**: bind mount를 mount table entry로 구현 (동일 vnode tree 공유)

### Files affected (estimated)
~42 files across kernel/: lock.h, lock_types.h, lock.c, atomic.h (new), file.h, proc.h, vma.h,
proc.c (x3: alloc_fd, fork clone, vma_lock type), vnode.c, vnode.h, vfs.h, vfs.c, file.c, initrd.c,
sys_file.c, sys_mmap.c, sys_pipe.c, sys_fd.c, sys_epoll.c, sys_socket.c, sys_proc.c, syscall.c,
mmu.c, vma.c, ramfs.c, elf.c, wait.h (new), softirq.h (new) + all inactive callbacks (pipe, eventfd, signalfd, timerfd), irq.S, cpu.c, cpu.h, idt.c, keyboard.c, timer.c, clock.c, sched.c
