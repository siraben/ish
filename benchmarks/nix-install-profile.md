# RV64 Nix Install Profile

Measured on the simulator Alpine rootfs while installing Nix 2.34.6
(`nix-2.34.6-riscv64-linux.tar.xz`) through `build-rv64-strace-stderr/ish`.

## Syscall mix

Source: `/tmp/ish-nix-strace2.log`, a strace run covering download unpacking,
copying into `/nix/store`, and Nix store registration. Total observed syscalls:
471,166.

| rank | syscall | count | percent |
| ---: | --- | ---: | ---: |
| 1 | write | 230,170 | 48.85% |
| 2 | read | 148,535 | 31.52% |
| 3 | mkdirat | 12,118 | 2.57% |
| 4 | newfstatat | 11,885 | 2.52% |
| 5 | clock_gettime | 9,897 | 2.10% |
| 6 | fcntl | 8,383 | 1.78% |
| 7 | openat | 6,157 | 1.31% |
| 8 | close | 5,857 | 1.24% |
| 9 | rt_sigaction | 4,513 | 0.96% |
| 10 | umask | 4,422 | 0.94% |
| 11 | lseek | 4,060 | 0.86% |
| 12 | recvfrom | 4,024 | 0.85% |
| 13 | writev | 3,516 | 0.75% |
| 14 | mmap | 2,479 | 0.53% |
| 15 | munmap | 2,036 | 0.43% |
| 16 | fchownat | 2,015 | 0.43% |
| 17 | fchmodat | 2,015 | 0.43% |
| 18 | utimensat | 2,015 | 0.43% |
| 19 | unlinkat | 1,705 | 0.36% |
| 20 | sendfile | 1,509 | 0.32% |
| 21 | ppoll | 1,486 | 0.32% |

The install is overwhelmingly file-I/O shaped: `read` + `write` account for
80.37% of all syscalls. Metadata churn (`mkdirat`, `newfstatat`, `openat`,
`close`, `fchownat`, `fchmodat`, `utimensat`, `unlinkat`) is the next major
group. SQLite-backed Nix DB work shows up as `fcntl`, `pread64`, `pwrite64`,
`fsync`, and file mapping calls, but none individually dominates the count.

## CPU sample

Source: `/tmp/ish-nix-cpu-sample.txt`, sampled with macOS `sample` at 10 ms
intervals against `build-bench-rv64/ish` while repeatedly running
`nix-env --version`. This is a proxy for Nix startup and DB-open paths, not the
full download phase.

Important active-thread stacks:

| area | evidence |
| --- | --- |
| RV64 gadget execution | `cpu_run_to_interrupt` -> `cpu_step_to_interrupt`, with samples in `gadget_rv_ld`, `gadget_rv_lw`, `gadget_rv_addi`, `gadget_rv_sd_sp`, `gadget_rv_amo`, `gadget_rv_branch`, and load/store fast/slow labels in `entry.S`. |
| Block generation / decode | `fiber_block_compile` -> `gen_step` -> `rv_decode` / `rv_decode_32`; compressed expansion appears via `rv_expand_compressed`. |
| TLB slow path | `rv_gadget_load_slow` -> `tlb_handle_miss` appears in active samples. |
| Kernel/fs overhead | Syscall samples are mostly `rv_sys_openat` -> `sys_openat` -> path normalization / fakefs / realfs. |
| Metadata DB overhead | Some fs paths enter `fake-db.c` and host `sqlite3_step`, including SQLite btree/page-cache and `fcntl` lock paths. |
| Wait overhead | The main task often sits in `wait4` / `pthread_cond_wait` while guest child processes run; that is not CPU burn, but it is visible in whole-process samples. |

Optimization implications:

1. The highest payoff syscall-side work is reducing file I/O and metadata
   overhead: batching/caching path metadata, reducing fakefs SQLite lookups on
   hot `openat`/`newfstatat` paths, and keeping `sendfile`/copy paths efficient.
2. The highest payoff CPU-side work remains RV64 memory-operation lowering:
   loads/stores, stack-relative load/store gadgets, TLB hit/miss paths, and
   block decode/compile cost during process startup.
3. Nix startup repeatedly touches signal setup, profile symlinks, SQLite locks,
   and path normalization, but those are secondary compared with raw read/write
   volume and interpreter memory gadgets.
