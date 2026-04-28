set -e

dd if=/tmp/bench-hot-paths/dev/zero of=/tmp/bench-hot-paths/dev/null bs=64k count=4096 status=none
