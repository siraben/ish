set -e

path=/tmp/bench-hot-paths/dd-file-rw-4k.bin
rm -f "$path"
dd if=/tmp/bench-hot-paths/dev/zero of="$path" bs=4k count=32768 status=none
dd if="$path" of=/tmp/bench-hot-paths/dev/null bs=4k status=none
rm -f "$path"
