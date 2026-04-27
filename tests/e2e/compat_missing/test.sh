#!/bin/sh
set -e

gcc mov_ds.c -o mov_ds
./mov_ds

gcc -msse2 pmullw.c -o pmullw
./pmullw

gcc seccomp_354.c -o seccomp_354
./seccomp_354

gcc -pthread cow_stress.c -o cow_stress
./cow_stress
