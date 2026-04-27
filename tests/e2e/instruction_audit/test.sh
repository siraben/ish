#!/bin/sh
set -e

gcc cmpxchg_reg.c -o cmpxchg_reg
./cmpxchg_reg

gcc -msse shufps_alias.c -o shufps_alias
./shufps_alias
