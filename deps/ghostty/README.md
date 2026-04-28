# Ghostty VT Library

This directory vendors `libghostty-vt` from `ghostty-org/ghostty`.

Source commit: `6590196661f769dd8f2b3e85d6c98262c4ec5b3b`

Build command used from the Ghostty checkout:

```sh
/opt/homebrew/opt/zig@0.15/bin/zig build -Demit-lib-vt=true -Demit-xcframework=true -Doptimize=ReleaseFast
```

The app links `ghostty-vt.xcframework` and compiles against the copied C
headers in `include/`.
