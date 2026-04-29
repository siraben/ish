#!/usr/bin/env python3
import math
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Benchmark:
    name: str
    command: str


def env_path(name: str, default: Path) -> Path:
    return Path(os.environ.get(name, str(default))).expanduser()


def benchmarks() -> list[Benchmark]:
    restores = int(os.environ.get("RESTORES_PER_RUN", "1"))
    if restores < 1:
        raise SystemExit("RESTORES_PER_RUN must be >= 1")
    return [
        Benchmark(
            f"nix_store_restore_nar_x{restores}",
            "set -eu; "
            f"for i in $(seq 1 {restores}); do "
            "out=/tmp/nix-unpack-out-$i; "
            "rm -rf $out; "
            "/root/.nix-profile/bin/nix-store --restore $out < /bench/nix-src.nar; "
            "test -f $out/src/libexpr/eval.cc; "
            "test -L $out/HACKING.md; "
            "done",
        ),
    ]


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print("+", " ".join(str(part) for part in cmd), file=sys.stderr)
    return subprocess.run([str(part) for part in cmd], check=True, **kwargs)


def copy_fakefs(src: Path, dst: Path) -> None:
    shutil.rmtree(dst, ignore_errors=True)
    dst.parent.mkdir(parents=True, exist_ok=True)
    run(["rsync", "-a", "--delete", f"{src}/", f"{dst}/"])


def summarize(times: list[float]) -> tuple[float, float, float, float]:
    best = min(times)
    avg = sum(times) / len(times)
    variance = (sum(t * t for t in times) / len(times)) - avg * avg
    stdev = math.sqrt(max(0.0, variance))
    rsd = 0.0 if avg == 0 else 100.0 * stdev / avg
    return best, avg, stdev, rsd


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    ish = env_path("ISH_BIN", root / "build-perf-rv64/ish")
    base_fs = env_path("BASE_FS", root / "e2e_out/nix-unpack/fs-base")
    out = env_path("OUT", root / "e2e_out/nix-unpack/bench")
    runs = int(os.environ.get("RUNS", "5"))
    warmups = int(os.environ.get("WARMUPS", "1"))
    label = os.environ.get("LABEL", "candidate")

    if not base_fs.is_dir():
        raise SystemExit(f"missing prepared fakefs: {base_fs}\nrun benchmarks/nix_unpack/prepare.py first")

    out.mkdir(parents=True, exist_ok=True)
    summary_path = out / "summary.tsv"
    raw_path = out / "raw.log"

    write_header = not summary_path.exists() or summary_path.stat().st_size == 0
    with summary_path.open("a", encoding="utf-8") as summary, raw_path.open("a", encoding="utf-8") as raw:
        if write_header:
            print("label\tbenchmark\tbest_s\tavg_s\tstdev_s\trsd_pct\truns", file=summary)
        for bench in benchmarks():
            for warmup in range(1, warmups + 1):
                fs = out / f"fs-{bench.name}-warmup-{warmup}"
                print(f"### {bench.name} warmup {warmup}/{warmups}", file=sys.stderr)
                copy_fakefs(base_fs, fs)
                run([ish, "-f", fs, "/bin/sh", "-lc", bench.command], stdout=subprocess.DEVNULL)
                shutil.rmtree(fs, ignore_errors=True)

            times: list[float] = []
            for i in range(1, runs + 1):
                fs = out / f"fs-{bench.name}-run-{i}"
                print(f"### {bench.name} run {i}/{runs}", file=sys.stderr)
                copy_fakefs(base_fs, fs)
                start = time.perf_counter()
                run([ish, "-f", fs, "/bin/sh", "-lc", bench.command], stdout=subprocess.DEVNULL)
                elapsed = time.perf_counter() - start
                shutil.rmtree(fs, ignore_errors=True)
                times.append(elapsed)
                print(f"{label}\t{bench.name}\t{i}\t{elapsed:.9f}", file=raw)

            best, avg, stdev, rsd = summarize(times)
            print(f"{label}\t{bench.name}\t{best:.6f}\t{avg:.6f}\t{stdev:.6f}\t{rsd:.2f}\t{len(times)}", file=summary)

    print(summary_path.read_text(encoding="utf-8"), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
