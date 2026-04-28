set -e

. /tmp/bench-hot-paths/guest/ensure_deep_tree.sh
LC_ALL=C ls -R "$root" >/tmp/bench-hot-paths/recursive-ls-deep.out
