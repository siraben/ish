#!/bin/sh
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
FS=${FS:-"$ROOT/e2e_out/testfs"}
ISH=${ISH:-"$ROOT/build/ish -f $FS"}
OUT=${OUT:-"$ROOT/e2e_out/bench-hot-paths"}
RUNS=${RUNS:-5}
WARMUPS=${WARMUPS:-1}
BENCH_FILTER=${BENCH_FILTER:-.}
LABEL=${LABEL:-$(basename "$ROOT")}

mkdir -p "$OUT"

run_guest() {
    $ISH /bin/sh -lc "$1"
}

run_guest "mkdir -p /tmp/bench-hot-paths"
tar -cf - -C "$SCRIPT_DIR" guest | run_guest "tar xf - -C /tmp/bench-hot-paths"
if [ "${BENCH_BIN_DIR:-}" != "" ]; then
    run_guest "mkdir -p /tmp/bench-hot-paths/bin"
    tar -cf - -C "$BENCH_BIN_DIR" . | run_guest "tar xf - -C /tmp/bench-hot-paths/bin"
fi

summary="$OUT/summary.tsv"
raw="$OUT/raw.log"
: > "$summary"
: > "$raw"
printf 'label\tbenchmark\tbest_s\tavg_s\tstdev_s\trsd_pct\truns\n' >> "$summary"

while IFS='	' read -r name required timer cmd; do
    [ -n "$name" ] || continue
    if ! printf '%s\n' "$name" | grep -Eq "$BENCH_FILTER"; then
        continue
    fi
    if [ "$required" != "-" ] && ! run_guest "command -v $required >/dev/null 2>&1"; then
        printf '### %s skipped: missing %s\n' "$name" "$required" | tee -a "$raw" >&2
        continue
    fi
    times=
    warmup=1
    while [ "$warmup" -le "$WARMUPS" ]; do
        printf '### %s warmup %s/%s\n' "$name" "$warmup" "$WARMUPS" | tee -a "$raw" >&2
        $ISH /bin/sh -lc "$cmd" >/dev/null 2>&1 || true
        warmup=$((warmup + 1))
    done
    i=1
    while [ "$i" -le "$RUNS" ]; do
        printf '### %s run %s/%s\n' "$name" "$i" "$RUNS" | tee -a "$raw" >&2
        if [ "$timer" = self ]; then
            output=$($ISH /bin/sh -lc "$cmd" 2>&1)
            elapsed=$(printf '%s\n' "$output" | awk '$1 == "elapsed_s" { value = $2 } END { print value }')
            if [ -z "$elapsed" ]; then
                printf '%s\n' "$output" >&2
                exit 1
            fi
        else
            elapsed=$($ISH /usr/bin/time -f '%e' /bin/sh -lc "$cmd" 2>&1 >/dev/null | tail -1)
        fi
        printf '%s\t%s\t%s\n' "$name" "$i" "$elapsed" >> "$raw"
        times="$times $elapsed"
        i=$((i + 1))
    done
    awk -v name="$name" -v label="$LABEL" '
        BEGIN {
            split(ARGV[1], vals, " ");
            ARGV[1] = "";
            best = -1;
            sum = 0;
            sumsq = 0;
            n = 0;
            for (i in vals) {
                if (vals[i] == "")
                    continue;
                v = vals[i] + 0;
                if (best < 0 || v < best)
                    best = v;
                sum += v;
                sumsq += v * v;
                n++;
            }
            if (n == 0)
                exit 1;
            avg = sum / n;
            variance = (sumsq / n) - (avg * avg);
            if (variance < 0)
                variance = 0;
            stdev = sqrt(variance);
            rsd = avg == 0 ? 0 : (100 * stdev / avg);
            printf "%s\t%s\t%.6f\t%.6f\t%.6f\t%.2f\t%d\n", label, name, best, avg, stdev, rsd, n;
        }
    ' "$times" >> "$summary"
done <<'EOF'
shell_startup	-	time	/bin/sh -lc "true"
shell_control	-	time	/bin/sh /tmp/bench-hot-paths/guest/shell_control.sh
shell_pipeline	-	time	/bin/sh /tmp/bench-hot-paths/guest/shell_pipeline.sh
fs_metadata	-	time	/bin/sh /tmp/bench-hot-paths/guest/fs_metadata.sh
prime_sieve	-	self	/tmp/bench-hot-paths/bin/prime_sieve
mandelbrot	-	self	/tmp/bench-hot-paths/bin/mandelbrot
file_io	-	self	/tmp/bench-hot-paths/bin/file_io
pipe_throughput	-	self	/tmp/bench-hot-paths/bin/pipe_throughput
gzip_payload	-	time	/bin/sh /tmp/bench-hot-paths/guest/gzip_payload.sh
python_startup	/usr/bin/python3	time	/usr/bin/python3 -S -c "pass"
python_compute	/usr/bin/python3	time	/usr/bin/python3 /tmp/bench-hot-paths/guest/python_compute.py
python_imports	/usr/bin/python3	time	/usr/bin/python3 /tmp/bench-hot-paths/guest/python_imports.py
bash_control	/bin/bash	time	/bin/bash /tmp/bench-hot-paths/guest/bash_control.sh
EOF

cat "$summary"
