set -e

path=/tmp/bench-hot-paths/dd-file-rw-64k.bin
rm -f "$path"
dd if=/tmp/bench-hot-paths/dev/zero of="$path" bs=64k count=2048 status=none
dd if="$path" of=/tmp/bench-hot-paths/dev/null bs=64k status=none
rm -f "$path"
