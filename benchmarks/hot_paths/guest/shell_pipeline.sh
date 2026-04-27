set -e

out=/tmp/bench-hot-paths/pipeline.txt
rm -f "$out"
i=0
while [ "$i" -lt 2500 ]; do
    printf 'line-%04d value-%04d\n' "$i" "$((i * 17 % 97))"
    i=$((i + 1))
done | sort | awk '{print $2}' | uniq -c > "$out"

wc -l "$out" >/dev/null
