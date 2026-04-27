#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FS=${FS:-"$ROOT/e2e_out/testfs"}
ISH=${ISH:-"$ROOT/build/ish -f $FS"}
OUT=${OUT:-"$ROOT/e2e_out/bench-hot-paths"}
RUNS=${RUNS:-5}

mkdir -p "$OUT"

run_guest() {
    $ISH /bin/sh -lc "$1"
}

run_guest "mkdir -p /tmp/bench-hot-paths"
tar -cf - -C "$ROOT/benchmarks/hot_paths" guest | run_guest "tar xf - -C /tmp/bench-hot-paths"

summary="$OUT/summary.tsv"
raw="$OUT/raw.log"
: > "$summary"
: > "$raw"
printf 'benchmark\tbest_s\tavg_s\truns\n' >> "$summary"

while IFS='	' read -r name cmd; do
    [ -n "$name" ] || continue
    times=
    i=1
    while [ "$i" -le "$RUNS" ]; do
        printf '### %s run %s/%s\n' "$name" "$i" "$RUNS" | tee -a "$raw" >&2
        elapsed=$($ISH /usr/bin/time -f '%e' /bin/sh -lc "$cmd" 2>&1 >/dev/null | tail -1)
        printf '%s\t%s\t%s\n' "$name" "$i" "$elapsed" >> "$raw"
        times="$times $elapsed"
        i=$((i + 1))
    done
    awk -v name="$name" -v runs="$RUNS" '
        BEGIN {
            split(ARGV[1], vals, " ");
            ARGV[1] = "";
            best = -1;
            sum = 0;
            n = 0;
            for (i in vals) {
                if (vals[i] == "")
                    continue;
                v = vals[i] + 0;
                if (best < 0 || v < best)
                    best = v;
                sum += v;
                n++;
            }
            printf "%s\t%.3f\t%.3f\t%d\n", name, best, sum / n, runs;
        }
    ' "$times" >> "$summary"
done <<'EOF'
python_startup	/usr/bin/python3 -S -c "pass"
python_compute	/usr/bin/python3 /tmp/bench-hot-paths/guest/python_compute.py
python_imports	/usr/bin/python3 /tmp/bench-hot-paths/guest/python_imports.py
bash_control	/bin/bash /tmp/bench-hot-paths/guest/bash_control.sh
shell_pipeline	/bin/sh /tmp/bench-hot-paths/guest/shell_pipeline.sh
EOF

cat "$summary"
