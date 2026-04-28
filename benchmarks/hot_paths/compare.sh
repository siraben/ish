#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
I386_ROOT=${I386_ROOT:-/Users/siraben/Git/ish}
RV64_ROOT=${RV64_ROOT:-$ROOT}
I386_BUILD=${I386_BUILD:-$I386_ROOT/build-bench}
RV64_BUILD=${RV64_BUILD:-$RV64_ROOT/build-bench-rv64}
I386_FS=${I386_FS:-$I386_ROOT/e2e_out/testfs}
RV64_FS=${RV64_FS:-$RV64_ROOT/e2e_out/testfs-rv64}
RV64_TAR=${RV64_TAR:-$RV64_ROOT/.rv64-rootfs-cache/alpine-minirootfs-3.23.4-riscv64.tar.gz}
OUT=${OUT:-$ROOT/e2e_out/bench-compare}
RUNS=${RUNS:-5}
WARMUPS=${WARMUPS:-1}
BENCH_FILTER=${BENCH_FILTER:-.}
CLANG=${CLANG:-}

mkdir -p "$OUT"

setup_build() {
    root=$1
    build=$2
    if [ ! -f "$build/build.ninja" ]; then
        meson setup "$build" "$root"
    else
        meson setup "$build" "$root" --reconfigure
    fi
    ninja -C "$build" ish
}

setup_build "$I386_ROOT" "$I386_BUILD"
setup_build "$RV64_ROOT" "$RV64_BUILD"

if [ -z "$CLANG" ]; then
    if [ -x /opt/homebrew/opt/llvm/bin/clang ]; then
        CLANG=/opt/homebrew/opt/llvm/bin/clang
    elif [ -x /opt/homebrew/Cellar/llvm/22.1.3/bin/clang ]; then
        CLANG=/opt/homebrew/Cellar/llvm/22.1.3/bin/clang
    else
        CLANG=clang
    fi
fi

compile_c_benchmarks() {
    target=$1
    out=$2
    mkdir -p "$out"
    for bench in prime_sieve mandelbrot file_io file_io_small file_random_write pipe_throughput dev_urandom_stream memory_stream branch_chaining call_return hot_regs; do
        "$CLANG" --target="$target" -O2 -fno-builtin -nostdlib -static -fuse-ld=lld \
            "$ROOT/benchmarks/hot_paths/cbench/start.c" \
            "$ROOT/benchmarks/hot_paths/cbench/$bench.c" \
            -o "$out/$bench"
    done
}

compile_c_benchmarks i386-linux-gnu "$OUT/bin/i386"
compile_c_benchmarks riscv64-linux-gnu "$OUT/bin/riscv64"

if [ ! -d "$I386_FS" ]; then
    echo "missing i386 fakefs: $I386_FS" >&2
    echo "run tests/e2e/e2e.bash -y in $I386_ROOT or set I386_FS" >&2
    exit 1
fi

if [ ! -d "$RV64_FS" ]; then
    fakefsify=
    for candidate in "$I386_BUILD/tools/fakefsify" "$I386_ROOT/build/tools/fakefsify" "$RV64_ROOT/build-x86/tools/fakefsify"; do
        if [ -x "$candidate" ]; then
            fakefsify=$candidate
            break
        fi
    done
    if [ -z "$fakefsify" ]; then
        echo "could not find fakefsify; build the i386 tree or set RV64_FS" >&2
        exit 1
    fi
    "$fakefsify" "$RV64_TAR" "$RV64_FS"
fi

run_one() {
    label=$1
    runner=$2
    fs=$3
    out=$4
    bin_dir=$5
    LABEL=$label ISH="$runner -f $fs" FS="$fs" OUT="$out" RUNS="$RUNS" WARMUPS="$WARMUPS" BENCH_FILTER="$BENCH_FILTER" BENCH_BIN_DIR="$bin_dir" "$ROOT/benchmarks/hot_paths/run.sh"
}

run_one i386 "$I386_BUILD/ish" "$I386_FS" "$OUT/i386" "$OUT/bin/i386"
run_one riscv64 "$RV64_BUILD/ish" "$RV64_FS" "$OUT/riscv64" "$OUT/bin/riscv64"

summary="$OUT/summary.tsv"
{
    head -1 "$OUT/i386/summary.tsv"
    tail -n +2 "$OUT/i386/summary.tsv"
    tail -n +2 "$OUT/riscv64/summary.tsv"
} > "$summary"

awk '
    BEGIN {
        FS = OFS = "\t";
        print "benchmark", "i386_best_s", "i386_best_mib_s", "i386_avg_s", "i386_avg_mib_s", "i386_rsd_pct", "riscv64_best_s", "riscv64_best_mib_s", "riscv64_avg_s", "riscv64_avg_mib_s", "riscv64_rsd_pct", "riscv64_vs_i386_best";
    }
    NR == 1 { next }
    {
        best[$1, $2] = $3;
        avg[$1, $2] = $4;
        rsd[$1, $2] = $6;
        best_mib[$1, $2] = $7;
        avg_mib[$1, $2] = $8;
        seen[$2] = 1;
    }
    END {
        for (name in seen) {
            if (("i386", name) in best && ("riscv64", name) in best) {
                if (best["i386", name] == 0)
                    ratio = "inf";
                else
                    ratio = sprintf("%.3fx", best["riscv64", name] / best["i386", name]);
                printf "%s\t%.6f\t%s\t%.6f\t%s\t%.2f\t%.6f\t%s\t%.6f\t%s\t%.2f\t%s\n", name, best["i386", name], best_mib["i386", name], avg["i386", name], avg_mib["i386", name], rsd["i386", name], best["riscv64", name], best_mib["riscv64", name], avg["riscv64", name], avg_mib["riscv64", name], rsd["riscv64", name], ratio;
            }
        }
    }
' "$summary" > "$OUT/compare.unsorted.tsv"
{
    head -1 "$OUT/compare.unsorted.tsv"
    tail -n +2 "$OUT/compare.unsorted.tsv" | sort
} > "$OUT/compare.tsv"
rm -f "$OUT/compare.unsorted.tsv"

cat "$summary"
printf '\n'
cat "$OUT/compare.tsv"
