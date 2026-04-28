set -e

set -- /var/cache/apk/APKINDEX*.tar.gz
[ -e "$1" ] || exit 77

/sbin/apk list >/tmp/bench-hot-paths/apk-list.out

lines=$(wc -l </tmp/bench-hot-paths/apk-list.out)
[ "$lines" -ge 1000 ] || exit 77
