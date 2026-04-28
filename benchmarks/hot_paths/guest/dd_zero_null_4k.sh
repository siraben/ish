set -e

dd if=/tmp/bench-hot-paths/dev/zero of=/tmp/bench-hot-paths/dev/null bs=4k count=65536 status=none
