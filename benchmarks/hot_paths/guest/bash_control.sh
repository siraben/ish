set -e

i=0
acc=0
while [ "$i" -lt 100000 ]; do
    case "$((i % 5))" in
        0) acc=$((acc + i)) ;;
        1) acc=$((acc - i / 2)) ;;
        2) acc=$((acc ^ i)) ;;
        3) acc=$((acc + i * 3)) ;;
        4) acc=$((acc - 7)) ;;
    esac
    i=$((i + 1))
done

printf '%s\n' "$acc" >/tmp/bench-hot-paths/bash-control.out
