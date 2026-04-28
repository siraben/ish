#!/usr/bin/env python3
import math
import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Benchmark:
    name: str
    required: str | None
    bytes: int | None
    timer: str
    command: str


BENCHMARKS = [
    Benchmark("shell_startup", None, None, "time", '/bin/sh -lc "true"'),
    Benchmark("shell_control", None, None, "time", "/bin/sh /tmp/bench-hot-paths/guest/shell_control.sh"),
    Benchmark("shell_pipeline", None, None, "time", "/bin/sh /tmp/bench-hot-paths/guest/shell_pipeline.sh"),
    Benchmark("fs_metadata", None, None, "time", "/bin/sh /tmp/bench-hot-paths/guest/fs_metadata.sh"),
    Benchmark("recursive_ls_deep", "/bin/ls", None, "time", "/bin/sh /tmp/bench-hot-paths/guest/recursive_ls_deep.sh"),
    Benchmark("prime_sieve", None, None, "self", "/tmp/bench-hot-paths/bin/prime_sieve"),
    Benchmark("mandelbrot", None, None, "self", "/tmp/bench-hot-paths/bin/mandelbrot"),
    Benchmark("file_io", None, 16 * 1024 * 1024, "self", "/tmp/bench-hot-paths/bin/file_io"),
    Benchmark("file_io_small", None, 8 * 1024 * 1024, "self", "/tmp/bench-hot-paths/bin/file_io_small"),
    Benchmark("file_copy", None, 48 * 1024 * 1024, "self", "/tmp/bench-hot-paths/bin/file_copy"),
    Benchmark("file_copy_64k", None, 48 * 1024 * 1024, "self", "/tmp/bench-hot-paths/bin/file_copy_64k"),
    Benchmark("file_random_write", None, 24 * 1024 * 1024, "self", "/tmp/bench-hot-paths/bin/file_random_write"),
    Benchmark("pipe_throughput", None, 32 * 1024 * 1024, "self", "/tmp/bench-hot-paths/bin/pipe_throughput"),
    Benchmark("dev_urandom_stream", None, 16 * 1024 * 1024, "self", "/tmp/bench-hot-paths/bin/dev_urandom_stream"),
    Benchmark("memory_stream", None, 256 * 1024 * 1024, "self", "/tmp/bench-hot-paths/bin/memory_stream"),
    Benchmark("branch_chaining", None, None, "self", "/tmp/bench-hot-paths/bin/branch_chaining"),
    Benchmark("call_return", None, None, "self", "/tmp/bench-hot-paths/bin/call_return"),
    Benchmark("hot_regs", None, None, "self", "/tmp/bench-hot-paths/bin/hot_regs"),
    Benchmark("dd_zero_null_4k", "/bin/dd", 256 * 1024 * 1024, "time", "/bin/sh /tmp/bench-hot-paths/guest/dd_zero_null_4k.sh"),
    Benchmark("dd_zero_null_64k", "/bin/dd", 256 * 1024 * 1024, "time", "/bin/sh /tmp/bench-hot-paths/guest/dd_zero_null_64k.sh"),
    Benchmark("dd_file_rw_4k", "/bin/dd", 256 * 1024 * 1024, "time", "/bin/sh /tmp/bench-hot-paths/guest/dd_file_rw_4k.sh"),
    Benchmark("dd_file_rw_64k", "/bin/dd", 256 * 1024 * 1024, "time", "/bin/sh /tmp/bench-hot-paths/guest/dd_file_rw_64k.sh"),
    Benchmark("gzip_payload", None, None, "time", "/bin/sh /tmp/bench-hot-paths/guest/gzip_payload.sh"),
    Benchmark("python_startup", "/usr/bin/python3", None, "time", '/usr/bin/python3 -S -c "pass"'),
    Benchmark("python_compute", "/usr/bin/python3", None, "time", "/usr/bin/python3 /tmp/bench-hot-paths/guest/python_compute.py"),
    Benchmark("python_memory", "/usr/bin/python3", None, "time", "/usr/bin/python3 /tmp/bench-hot-paths/guest/python_memory.py"),
    Benchmark("python_file_io", "/usr/bin/python3", 8 * 1024 * 1024, "time", "/usr/bin/python3 /tmp/bench-hot-paths/guest/python_file_io.py"),
    Benchmark("python_imports", "/usr/bin/python3", None, "time", "/usr/bin/python3 /tmp/bench-hot-paths/guest/python_imports.py"),
    Benchmark("bash_control", "/bin/bash", None, "time", "/bin/bash /tmp/bench-hot-paths/guest/bash_control.sh"),
]


def run(cmd: str, *, stdin=None, stdout=None, stderr=None, check=True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, shell=True, stdin=stdin, stdout=stdout, stderr=stderr, text=False, check=check)


def run_text(cmd: str, *, check=True) -> str:
    result = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=check)
    return result.stdout


def guest_cmd(ish: str, command: str) -> str:
    return f"{ish} /bin/sh -lc {shlex.quote(command)}"


def guest_run(ish: str, command: str, *, check=True) -> str:
    return run_text(guest_cmd(ish, command), check=check)


def copy_dir_to_guest(ish: str, source: Path, dest: str) -> None:
    tar = subprocess.Popen(["tar", "-cf", "-", "-C", str(source), "."], stdout=subprocess.PIPE)
    extract = subprocess.run(
        guest_cmd(ish, f"mkdir -p {shlex.quote(dest)} && tar xf - -C {shlex.quote(dest)}"),
        shell=True,
        stdin=tar.stdout,
        check=True,
    )
    if tar.stdout is not None:
        tar.stdout.close()
    tar.wait()
    if tar.returncode != 0 or extract.returncode != 0:
        raise subprocess.CalledProcessError(tar.returncode or extract.returncode, "tar")


def parse_self_elapsed(output: str) -> float:
    elapsed = None
    for line in output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0] == "elapsed_s":
            elapsed = parts[1]
    if elapsed is None:
        print(output, file=sys.stderr)
        raise RuntimeError("self-timed benchmark did not print elapsed_s")
    return float(elapsed)


def run_benchmark(ish: str, bench: Benchmark) -> float:
    if bench.timer == "self":
        output = guest_run(ish, bench.command)
        return parse_self_elapsed(output)
    output = run_text(f"{ish} /usr/bin/time -f '%e' /bin/sh -lc {shlex.quote(bench.command)}")
    for line in reversed(output.splitlines()):
        try:
            return float(line.strip())
        except ValueError:
            continue
    print(output, file=sys.stderr)
    raise RuntimeError("time output did not contain elapsed seconds")


def summarize(times: list[float], byte_count: int | None) -> tuple[float, float, float, float, str, str]:
    best = min(times)
    avg = sum(times) / len(times)
    variance = (sum(t * t for t in times) / len(times)) - avg * avg
    stdev = math.sqrt(max(0.0, variance))
    rsd = 0.0 if avg == 0 else 100.0 * stdev / avg
    if byte_count is None:
        return best, avg, stdev, rsd, "-", "-"
    mib = byte_count / 1048576.0
    return best, avg, stdev, rsd, f"{mib / best:.2f}", f"{mib / avg:.2f}"


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    root = script_dir.parent.parent
    fs = os.environ.get("FS", str(root / "e2e_out/testfs"))
    ish = os.environ.get("ISH", f"{root}/build/ish -f {fs}")
    out = Path(os.environ.get("OUT", str(root / "e2e_out/bench-hot-paths")))
    runs = int(os.environ.get("RUNS", "5"))
    warmups = int(os.environ.get("WARMUPS", "1"))
    bench_filter = re.compile(os.environ.get("BENCH_FILTER", "."))
    label = os.environ.get("LABEL", root.name)
    bin_dir = os.environ.get("BENCH_BIN_DIR")

    out.mkdir(parents=True, exist_ok=True)
    summary_path = out / "summary.tsv"
    raw_path = out / "raw.log"

    guest_run(ish, "mkdir -p /tmp/bench-hot-paths")
    guest_run(
        ish,
        "mkdir -p /tmp/bench-hot-paths/dev && "
        "rm -f /tmp/bench-hot-paths/dev/null /tmp/bench-hot-paths/dev/zero "
        "/tmp/bench-hot-paths/dev/random /tmp/bench-hot-paths/dev/urandom && "
        "mknod /tmp/bench-hot-paths/dev/null c 1 3 && "
        "mknod /tmp/bench-hot-paths/dev/zero c 1 5 && "
        "mknod /tmp/bench-hot-paths/dev/random c 1 8 && "
        "mknod /tmp/bench-hot-paths/dev/urandom c 1 9",
    )
    copy_dir_to_guest(ish, script_dir / "guest", "/tmp/bench-hot-paths/guest")
    if bin_dir:
        copy_dir_to_guest(ish, Path(bin_dir), "/tmp/bench-hot-paths/bin")

    with summary_path.open("w", encoding="utf-8") as summary, raw_path.open("w", encoding="utf-8") as raw:
        print("label\tbenchmark\tbest_s\tavg_s\tstdev_s\trsd_pct\tbest_mib_s\tavg_mib_s\truns", file=summary)
        for bench in BENCHMARKS:
            if not bench_filter.search(bench.name):
                continue
            if bench.required is not None:
                probe = subprocess.run(
                    guest_cmd(ish, f"command -v {shlex.quote(bench.required)} >/dev/null 2>&1"),
                    shell=True,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                if probe.returncode != 0:
                    print(f"### {bench.name} skipped: missing {bench.required}", file=sys.stderr)
                    print(f"### {bench.name} skipped: missing {bench.required}", file=raw)
                    continue

            for warmup in range(1, warmups + 1):
                print(f"### {bench.name} warmup {warmup}/{warmups}", file=sys.stderr)
                subprocess.run(guest_cmd(ish, bench.command), shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            times: list[float] = []
            for i in range(1, runs + 1):
                print(f"### {bench.name} run {i}/{runs}", file=sys.stderr)
                elapsed = run_benchmark(ish, bench)
                times.append(elapsed)
                print(f"{bench.name}\t{i}\t{elapsed:.9f}", file=raw)

            best, avg, stdev, rsd, best_mib, avg_mib = summarize(times, bench.bytes)
            print(
                f"{label}\t{bench.name}\t{best:.6f}\t{avg:.6f}\t{stdev:.6f}\t{rsd:.2f}\t{best_mib}\t{avg_mib}\t{len(times)}",
                file=summary,
            )

    print(summary_path.read_text(encoding="utf-8"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
