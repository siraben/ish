set -e

. /tmp/bench-hot-paths/guest/ensure_deep_tree.sh
find "$root" -exec stat {} \; >/tmp/bench-hot-paths/find-stat-deep.out
