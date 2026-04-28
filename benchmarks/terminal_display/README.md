# Terminal Display Benchmarks

This directory contains a standalone microbenchmark for the terminal display
paths touched by the Ghostty experiment.

Build and run from the repository root:

```sh
clang -fobjc-arc -framework Foundation \
  -Ideps/ghostty/include \
  benchmarks/terminal_display/terminal_display_bench.m \
  deps/ghostty/ghostty-vt.xcframework/macos-arm64_x86_64/libghostty-vt.a \
  -o /tmp/terminal_display_bench

/tmp/terminal_display_bench 16777216 4096
```

The benchmark compares:

- `hterm-bridge`: the old Objective-C output bridge cost, matching the
  `NSString` Latin-1 conversion, escaping, and `exports.write(...)` JavaScript
  string construction used before this branch.
- `ghostty-vt`: `libghostty-vt` parsing plus render-state update for the same
  byte chunks.

It does not include WebKit JavaScript evaluation or hterm DOM rendering for the
old path, and it does not include UIKit drawing for the new path. Those are app
runtime costs; this benchmark isolates the terminal-output hot path that can run
repeatably outside the simulator.

Median of five runs on April 28, 2026, using 16 MiB payloads in 4 KiB chunks:

| Payload | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| plain text | 180.05 ms / 88.87 MiB/s | 87.99 ms / 181.83 MiB/s | 2.05x |
| SGR color text | 183.61 ms / 87.14 MiB/s | 113.72 ms / 140.70 MiB/s | 1.61x |
| cursor control | 178.52 ms / 89.63 MiB/s | 169.99 ms / 94.12 MiB/s | 1.05x |
