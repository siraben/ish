#!/usr/bin/env python3
import os
import shutil
import subprocess
import sys
from pathlib import Path


C_BENCHMARKS = [
    "prime_sieve",
    "mandelbrot",
    "file_io",
    "file_io_small",
    "file_iov",
    "file_pread_pwrite_64k",
    "file_pread_pwrite_4k",
    "file_pread_pwrite_random_64k",
    "fstat_loop",
    "stat_misses",
    "stat_cache_large_misses",
    "stat_cache_write_txn",
    "socket_msg_iov",
    "syscall_identity",
    "time_syscalls",
    "futex_eagain",
    "eventfd_loop",
    "epoll_eventfd",
    "poll_many_ready",
    "mmap_faults",
    "fd_control",
    "fd_high_churn",
    "process_wait",
    "signal_mask",
    "identity_extended",
    "yield_loop",
    "file_copy",
    "file_copy_64k",
    "file_random_write",
    "pipe_throughput",
    "dev_urandom_stream",
    "memory_stream",
    "branch_chaining",
    "call_return",
    "hot_regs",
]


def env_path(name: str, default: Path) -> Path:
    return Path(os.environ.get(name, str(default))).expanduser()


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print("+", " ".join(str(part) for part in cmd), file=sys.stderr)
    return subprocess.run([str(part) for part in cmd], check=True, **kwargs)


def setup_build(root: Path, build: Path) -> None:
    if (build / "build.ninja").exists():
        run(["meson", "setup", build, root, "--reconfigure"])
    else:
        run(["meson", "setup", build, root])
    run(["ninja", "-C", build, "ish"])


def find_clang() -> str:
    configured = os.environ.get("CLANG")
    if configured:
        return configured
    for candidate in (
        "/opt/homebrew/opt/llvm/bin/clang",
        "/opt/homebrew/Cellar/llvm/22.1.3/bin/clang",
    ):
        if Path(candidate).exists():
            return candidate
    return "clang"


def compile_c_benchmarks(root: Path, clang: str, target: str, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for bench in C_BENCHMARKS:
        run([
            clang,
            f"--target={target}",
            "-O2",
            "-fno-builtin",
            "-nostdlib",
            "-static",
            "-fuse-ld=lld",
            root / "benchmarks/hot_paths/cbench/start.c",
            root / f"benchmarks/hot_paths/cbench/{bench}.c",
            "-o",
            out / bench,
        ])


def copy_fakefs(src: Path, dst: Path) -> None:
    if not src.is_dir():
        raise SystemExit(f"missing fakefs source: {src}")
    shutil.rmtree(dst, ignore_errors=True)
    dst.parent.mkdir(parents=True, exist_ok=True)
    rsync = shutil.which("rsync")
    if rsync:
        dst.mkdir(parents=True, exist_ok=True)
        run([rsync, "-a", "--delete", f"{src}/", f"{dst}/"])
    else:
        shutil.copytree(src, dst, symlinks=True)
    shutil.rmtree(dst / "data/tmp/bench-hot-paths", ignore_errors=True)


def has_apk_indexes(fs: Path) -> bool:
    return any((fs / "data/var/cache/apk").glob("APKINDEX*.tar.gz"))


def prepare_apk_indexes(runner: Path, fs: Path) -> None:
    if os.environ.get("PREPARE_APK_INDEXES", "1") != "1":
        return
    if has_apk_indexes(fs):
        return
    if not (fs / "data/sbin/apk").exists():
        return
    cmd = [runner, "-f", fs, "/bin/sh", "-lc", "apk update"]
    print("+", " ".join(str(part) for part in cmd), file=sys.stderr)
    result = subprocess.run([str(part) for part in cmd])
    if result.returncode != 0:
        print("apk index preparation failed; apk_list will skip unless indexes are present", file=sys.stderr)


def ensure_bases(i386_root: Path, rv64_root: Path, i386_build: Path, rv64_build: Path,
                 i386_seed_fs: Path, i386_base_fs: Path, rv64_base_fs: Path,
                 rv64_tar: Path) -> None:
    if not i386_base_fs.is_dir():
        if not i386_seed_fs.is_dir():
            raise SystemExit(
                f"missing i386 seed fakefs: {i386_seed_fs}\n"
                "run tests/e2e/e2e.bash -y in the i386 tree or set I386_SEED_FS/I386_BASE_FS"
            )
        copy_fakefs(i386_seed_fs, i386_base_fs)
        prepare_apk_indexes(i386_build / "ish", i386_base_fs)

    if not rv64_base_fs.is_dir():
        fakefsify = None
        for candidate in (
            i386_build / "tools/fakefsify",
            i386_root / "build/tools/fakefsify",
            rv64_root / "build-x86/tools/fakefsify",
        ):
            if candidate.exists():
                fakefsify = candidate
                break
        if fakefsify is None:
            raise SystemExit("could not find fakefsify; build the i386 tree or set RV64_BASE_FS")
        rv64_base_fs.parent.mkdir(parents=True, exist_ok=True)
        run([fakefsify, rv64_tar, rv64_base_fs])
        prepare_apk_indexes(rv64_build / "ish", rv64_base_fs)


def run_one(root: Path, label: str, runner: Path, fs: Path, out: Path, bin_dir: Path,
            runs: str, warmups: str, bench_filter: str) -> None:
    env = os.environ.copy()
    env.update({
        "LABEL": label,
        "ISH": f"{runner} -f {fs}",
        "FS": str(fs),
        "OUT": str(out),
        "RUNS": runs,
        "WARMUPS": warmups,
        "BENCH_FILTER": bench_filter,
        "BENCH_BIN_DIR": str(bin_dir),
    })
    run([sys.executable, root / "benchmarks/hot_paths/run.py"], env=env)


def read_tsv(path: Path) -> list[dict[str, str]]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines:
        return []
    header = lines[0].split("\t")
    return [dict(zip(header, line.split("\t"))) for line in lines[1:] if line]


def write_outputs(out: Path) -> None:
    i386_summary = out / "i386/summary.tsv"
    rv64_summary = out / "riscv64/summary.tsv"
    header = i386_summary.read_text(encoding="utf-8").splitlines()[0]
    rows = i386_summary.read_text(encoding="utf-8").splitlines()[1:]
    rows += rv64_summary.read_text(encoding="utf-8").splitlines()[1:]
    summary = out / "summary.tsv"
    summary.write_text(header + "\n" + "\n".join(rows) + "\n", encoding="utf-8")

    by_key = {(row["label"], row["benchmark"]): row for row in read_tsv(summary)}
    names = sorted({bench for _, bench in by_key})
    compare_header = [
        "benchmark",
        "i386_best_s",
        "i386_best_mib_s",
        "i386_avg_s",
        "i386_avg_mib_s",
        "i386_rsd_pct",
        "riscv64_best_s",
        "riscv64_best_mib_s",
        "riscv64_avg_s",
        "riscv64_avg_mib_s",
        "riscv64_rsd_pct",
        "riscv64_speedup_best",
    ]
    compare_lines = ["\t".join(compare_header)]
    for name in names:
        i386 = by_key.get(("i386", name))
        rv64 = by_key.get(("riscv64", name))
        if i386 is None or rv64 is None:
            continue
        rv64_best = float(rv64["best_s"])
        speedup = "inf" if rv64_best == 0 else f"{float(i386['best_s']) / rv64_best:.3f}x"
        compare_lines.append("\t".join([
            name,
            f"{float(i386['best_s']):.6f}",
            i386["best_mib_s"],
            f"{float(i386['avg_s']):.6f}",
            i386["avg_mib_s"],
            f"{float(i386['rsd_pct']):.2f}",
            f"{float(rv64['best_s']):.6f}",
            rv64["best_mib_s"],
            f"{float(rv64['avg_s']):.6f}",
            rv64["avg_mib_s"],
            f"{float(rv64['rsd_pct']):.2f}",
            speedup,
        ]))
    compare = out / "compare.tsv"
    compare.write_text("\n".join(compare_lines) + "\n", encoding="utf-8")

    print(summary.read_text(encoding="utf-8"), end="")
    print()
    print(compare.read_text(encoding="utf-8"), end="")


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    i386_root = env_path("I386_ROOT", Path("/Users/siraben/Git/ish"))
    rv64_root = env_path("RV64_ROOT", root)
    i386_build = env_path("I386_BUILD", i386_root / "build-bench")
    rv64_build = env_path("RV64_BUILD", rv64_root / "build-bench-rv64")
    out = env_path("OUT", root / "e2e_out/bench-compare")
    i386_seed_fs = env_path("I386_SEED_FS", i386_root / "e2e_out/testfs")
    i386_base_fs = env_path("I386_BASE_FS", root / "e2e_out/bench-base/i386")
    rv64_base_fs = env_path("RV64_BASE_FS", root / "e2e_out/bench-base/riscv64")
    i386_fs = env_path("I386_FS", out / "fs/i386")
    rv64_fs = env_path("RV64_FS", out / "fs/riscv64")
    rv64_tar = env_path("RV64_TAR", rv64_root / ".rv64-rootfs-cache/alpine-minirootfs-3.23.4-riscv64.tar.gz")
    runs = os.environ.get("RUNS", "5")
    warmups = os.environ.get("WARMUPS", "1")
    bench_filter = os.environ.get("BENCH_FILTER", ".")

    out.mkdir(parents=True, exist_ok=True)
    setup_build(i386_root, i386_build)
    setup_build(rv64_root, rv64_build)

    clang = find_clang()
    compile_c_benchmarks(root, clang, "i386-linux-gnu", out / "bin/i386")
    compile_c_benchmarks(root, clang, "riscv64-linux-gnu", out / "bin/riscv64")

    ensure_bases(i386_root, rv64_root, i386_build, rv64_build, i386_seed_fs,
                 i386_base_fs, rv64_base_fs, rv64_tar)
    copy_fakefs(i386_base_fs, i386_fs)
    copy_fakefs(rv64_base_fs, rv64_fs)

    run_one(root, "i386", i386_build / "ish", i386_fs, out / "i386",
            out / "bin/i386", runs, warmups, bench_filter)
    run_one(root, "riscv64", rv64_build / "ish", rv64_fs, out / "riscv64",
            out / "bin/riscv64", runs, warmups, bench_filter)
    write_outputs(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
