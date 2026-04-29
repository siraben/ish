# Nix Unpack Benchmark

This benchmark measures Nix NAR restore/unpack performance inside the riscv64
iSH fakefs. It keeps network out of the timed path by baking a local NAR into a
prepared fakefs fixture.

Default fixture inputs:

- Base fakefs: `e2e_out/bench-base/riscv64`
- Source tree for NAR generation: `/private/tmp/nix-src`
- Nix tarball cache: `e2e_out/nix-unpack/cache/nix-2.34.6-riscv64-linux.tar.xz`
- Prepared fakefs: `e2e_out/nix-unpack/fs-base`

Prepare once:

```sh
python3 benchmarks/nix_unpack/prepare.py
```

Run:

```sh
RUNS=5 WARMUPS=1 python3 benchmarks/nix_unpack/run.py
```

Set `LABEL=baseline` or `LABEL=candidate` to tag repeated runs. Results append to
`e2e_out/nix-unpack/bench/summary.tsv` and raw per-run timings append to
`e2e_out/nix-unpack/bench/raw.log`.

Set `RESTORES_PER_RUN=5` to run several full restores inside one timed guest
session when host/process noise dominates short runs.

The timed command is:

```sh
/root/.nix-profile/bin/nix-store --restore /tmp/nix-unpack-out < /bench/nix-src.nar
```

Measured on this branch with `RESTORES_PER_RUN=5 RUNS=5 WARMUPS=1`:

```text
baseline_x5            avg 3.683045s
path_stat_writeback_x5 avg 3.000889s
```

The writeback result defers fakefs metadata inserts/updates for exclusive file
creates, mkdirs, symlinks, and fd metadata changes, then flushes the accumulated
`paths` and `stats` rows in bulk.

Rejected experiments in this workload:

- Full generation invalidation of the fakefs stat cache: O(1), but loses useful
  cache entries and measured slower than targeted occupied-slot updates.
- Hash tags on fakefs stat-cache entries: worse locality from larger entries
  outweighed fewer string comparisons.
- Cached mount/source path lengths: did not move this benchmark.
- fd-table first-free tracking: valid O(n) to amortized O(1) cleanup, but not a
  Nix-unpack win without a separate fd-churn benchmark.
- Deferred fd-only stat writes without path create batching: did not improve the
  restore benchmark because create metadata remained the dominant SQLite path.
