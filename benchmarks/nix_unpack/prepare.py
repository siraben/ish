#!/usr/bin/env python3
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path


NIX_VERSION = "2.34.6"
NIX_TARBALL = f"nix-{NIX_VERSION}-riscv64-linux.tar.xz"
NIX_URL = f"https://releases.nixos.org/nix/nix-{NIX_VERSION}/{NIX_TARBALL}"


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    print("+", " ".join(shlex.quote(str(part)) for part in cmd), file=sys.stderr)
    return subprocess.run([str(part) for part in cmd], check=True, **kwargs)


def run_shell(cmd: str, **kwargs) -> subprocess.CompletedProcess:
    print("+", cmd, file=sys.stderr)
    return subprocess.run(cmd, shell=True, check=True, **kwargs)


def env_path(name: str, default: Path) -> Path:
    return Path(os.environ.get(name, str(default))).expanduser()


def copy_fakefs(src: Path, dst: Path) -> None:
    if not src.is_dir():
        raise SystemExit(f"missing fakefs source: {src}")
    shutil.rmtree(dst, ignore_errors=True)
    dst.parent.mkdir(parents=True, exist_ok=True)
    run(["rsync", "-a", "--delete", f"{src}/", f"{dst}/"])


def guest_cmd(ish: Path, fs: Path, command: str) -> list[str]:
    return [ish, "-f", fs, "/bin/sh", "-lc", command]


def copy_file_to_guest(ish: Path, fs: Path, source: Path, dest_dir: str) -> None:
    tar = subprocess.Popen(
        ["tar", "-cf", "-", "-C", str(source.parent), source.name],
        stdout=subprocess.PIPE,
    )
    try:
        run(
            guest_cmd(ish, fs, f"mkdir -p {shlex.quote(dest_dir)} && tar xf - -C {shlex.quote(dest_dir)}"),
            stdin=tar.stdout,
        )
    finally:
        if tar.stdout is not None:
            tar.stdout.close()
    if tar.wait() != 0:
        raise subprocess.CalledProcessError(tar.returncode, "tar")


def ensure_nar(source: Path, nar: Path) -> None:
    if nar.exists():
        return
    if not source.exists():
        raise SystemExit(f"missing NAR source tree: {source}")
    nar.parent.mkdir(parents=True, exist_ok=True)
    with nar.open("wb") as out:
        run(["nix-store", "--dump", source], stdout=out)


def ensure_tarball(tarball: Path) -> None:
    if tarball.exists():
        return
    tarball.parent.mkdir(parents=True, exist_ok=True)
    run(["curl", "-fL", NIX_URL, "-o", tarball])


def install_nix(ish: Path, fs: Path) -> None:
    copy_file_to_guest(ish, fs, env_path("NIX_TARBALL", Path("e2e_out/nix-unpack/cache") / NIX_TARBALL), "/tmp/nix-setup")
    install = (
        "set -eu; "
        "mkdir -p /nix /dev; chmod 0755 /nix; chown root /nix; "
        "[ -e /dev/ptmx ] || mknod /dev/ptmx c 5 2; "
        "cd /tmp/nix-setup; tar -xJf " + shlex.quote(NIX_TARBALL) + "; "
        "cd " + shlex.quote(f"nix-{NIX_VERSION}-riscv64-linux") + "; "
        "sed -i 's/cp -RP --preserve=ownership,timestamps/cp -a/' install; "
        "export USER=root HOME=/root NIX_BECOME= "
        "NIX_CONFIG='build-users-group =\nexperimental-features = nix-command flakes' "
        "NIX_INSTALLER_NO_MODIFY_PROFILE=1 NIX_INSTALLER_NO_CHANNEL_ADD=1; "
        "sh install --no-daemon --yes --no-channel-add --no-modify-profile; "
        "mkdir -p /etc/nix; "
        "printf 'build-users-group =\\nexperimental-features = nix-command flakes\\n' > /etc/nix/nix.conf; "
        "rm -rf /tmp/nix-setup"
    )
    run(guest_cmd(ish, fs, install))


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    ish = env_path("ISH_BIN", root / "build-perf-rv64/ish")
    seed_fs = env_path("SEED_FS", root / "e2e_out/bench-base/riscv64")
    out_fs = env_path("OUT_FS", root / "e2e_out/nix-unpack/fs-base")
    cache = env_path("CACHE", root / "e2e_out/nix-unpack/cache")
    nar_source = env_path("NAR_SOURCE", Path("/private/tmp/nix-src"))
    nar = env_path("NAR", cache / "nix-src.nar")
    tarball = env_path("NIX_TARBALL", cache / NIX_TARBALL)

    if not ish.exists():
        run(["ninja", "-C", root / "build-perf-rv64", "ish"])

    ensure_tarball(tarball)
    ensure_nar(nar_source, nar)
    copy_fakefs(seed_fs, out_fs)
    install_nix(ish, out_fs)
    copy_file_to_guest(ish, out_fs, nar, "/bench")
    run(guest_cmd(
        ish,
        out_fs,
        "set -eu; "
        "/root/.nix-profile/bin/nix --version; "
        "/root/.nix-profile/bin/nix-store --version; "
        "rm -rf /tmp/nix-unpack-out; "
        "/root/.nix-profile/bin/nix-store --restore /tmp/nix-unpack-out < /bench/nix-src.nar; "
        "test $(find /tmp/nix-unpack-out -mindepth 1 | wc -l) -eq 3256; "
        "rm -rf /tmp/nix-unpack-out",
    ))
    print(out_fs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

