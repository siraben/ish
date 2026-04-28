set -e

payload=/tmp/bench-hot-paths/gzip-payload.txt
archive=/tmp/bench-hot-paths/gzip-payload.txt.gz
rm -f "$payload" "$archive"

i=0
while [ "$i" -lt 4000 ]; do
    printf 'record-%05d alpha beta gamma delta epsilon %05d\n' "$i" "$((i * 17 % 9973))"
    i=$((i + 1))
done > "$payload"

gzip -9 -c "$payload" > "$archive"
gzip -dc "$archive" | wc -c >/tmp/bench-hot-paths/gzip-payload.out
