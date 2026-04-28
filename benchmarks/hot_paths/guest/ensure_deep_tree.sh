root=/tmp/bench-hot-paths/recursive-ls-tree
marker=$root/.complete

if [ ! -f "$marker" ]; then
    rm -rf "$root"
    mkdir -p "$root"
    a=0
    while [ "$a" -lt 4 ]; do
        d1=$root/d$a
        mkdir "$d1"
        f=0
        while [ "$f" -lt 4 ]; do
            printf '%s\n' "$a-$f" > "$d1/file-$f.txt"
            f=$((f + 1))
        done
        b=0
        while [ "$b" -lt 4 ]; do
            d2=$d1/d$b
            mkdir "$d2"
            f=0
            while [ "$f" -lt 4 ]; do
                printf '%s\n' "$a-$b-$f" > "$d2/file-$f.txt"
                f=$((f + 1))
            done
            c=0
            while [ "$c" -lt 4 ]; do
                d3=$d2/d$c
                mkdir "$d3"
                f=0
                while [ "$f" -lt 4 ]; do
                    printf '%s\n' "$a-$b-$c-$f" > "$d3/file-$f.txt"
                    f=$((f + 1))
                done
                d=0
                while [ "$d" -lt 4 ]; do
                    d4=$d3/d$d
                    mkdir "$d4"
                    f=0
                    while [ "$f" -lt 4 ]; do
                        printf '%s\n' "$a-$b-$c-$d-$f" > "$d4/file-$f.txt"
                        f=$((f + 1))
                    done
                    d=$((d + 1))
                done
                c=$((c + 1))
            done
            b=$((b + 1))
        done
        a=$((a + 1))
    done
    : > "$marker"
fi
