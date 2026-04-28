set -e

dir=/tmp/bench-hot-paths/fs-metadata
rm -rf "$dir"
mkdir -p "$dir"

i=0
while [ "$i" -lt 1500 ]; do
    printf '%s\n' "$i" > "$dir/file-$i"
    i=$((i + 1))
done

i=0
while [ "$i" -lt 1500 ]; do
    test -f "$dir/file-$i"
    i=$((i + 1))
done

ls "$dir" | wc -l >/tmp/bench-hot-paths/fs-metadata.out
