#!/usr/bin/env python3
"""Create a Debian .deb from a staged filesystem tree without dpkg-deb.

The package is a valid ar(debian-binary, control.tar.gz, data.tar.gz) archive;
this keeps release builds working on hosts that only have Python + tar + ar.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import subprocess
import tarfile
import tempfile
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--name", default="tqsdk")
    parser.add_argument("--version", required=True)
    parser.add_argument("--release", default="1")
    parser.add_argument("--architecture", default="amd64")
    parser.add_argument("--maintainer", default="TQSDK <louchangfeng@outlook.com>")
    parser.add_argument("--description", default="TQSDK gateway, node and web terminal")
    parser.add_argument("--depends", default="libc6, libstdc++6, libgcc-s1, passwd")
    parser.add_argument("--postinst", type=Path)
    return parser.parse_args()


def add_file(tf: tarfile.TarFile, path: Path, arcname: str) -> None:
    info = tf.gettarinfo(str(path), arcname=arcname)
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.mtime = 0
    if path.is_symlink():
        tf.addfile(info)
        return
    info.size = path.stat().st_size
    with path.open("rb") as handle:
        tf.addfile(info, handle)


def build_data_tar(stage: Path, target: Path) -> tuple[int, list[tuple[str, str]]]:
    installed_bytes = 0
    md5rows: list[tuple[str, str]] = []
    with tarfile.open(target, "w:gz", format=tarfile.GNU_FORMAT) as tf:
        for path in sorted(stage.rglob("*")):
            if path.is_dir() and not path.is_symlink():
                continue
            rel = path.relative_to(stage).as_posix()
            arcname = f"./{rel}"
            add_file(tf, path, arcname)
            if path.is_file() and not path.is_symlink():
                installed_bytes += path.stat().st_size
                digest = hashlib.md5(path.read_bytes()).hexdigest()
                md5rows.append((digest, rel))
    return installed_bytes, md5rows


def build_control_tar(
    target: Path,
    control: str,
    md5rows: list[tuple[str, str]],
    postinst: Path | None,
) -> None:
    with tarfile.open(target, "w:gz", format=tarfile.GNU_FORMAT) as tf:
        def add_text(name: str, data: str, mode: int) -> None:
            payload = data.encode()
            info = tarfile.TarInfo(f"./{name}")
            info.size = len(payload)
            info.mode = mode
            info.uid = 0
            info.gid = 0
            info.uname = "root"
            info.gname = "root"
            info.mtime = 0
            tf.addfile(info, io.BytesIO(payload))

        add_text("control", control, 0o644)
        md5 = "".join(f"{digest}  {name}\n" for digest, name in sorted(md5rows))
        add_text("md5sums", md5, 0o644)
        if postinst is not None:
            add_text("postinst", postinst.read_text(), 0o755)


def build_deb(control_tar: Path, data_tar: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tqsdk-deb-") as tmp:
        binary = Path(tmp) / "debian-binary"
        binary.write_text("2.0\n")
        subprocess.run(
            ["ar", "rcs", str(target), str(binary), str(control_tar), str(data_tar)],
            check=True,
            stdout=subprocess.DEVNULL,
        )


def main() -> int:
    args = parse_args()
    stage = args.stage.resolve()
    if not (stage / "usr" / "bin" / "tqsdk-node").is_file():
        raise SystemExit(f"staging tree is missing usr/bin/tqsdk-node: {stage}")

    with tempfile.TemporaryDirectory(prefix="tqsdk-deb-build-") as tmp:
        tmpdir = Path(tmp)
        data_tar = tmpdir / "data.tar.gz"
        installed_bytes, md5rows = build_data_tar(stage, data_tar)
        installed_kib = max(1, (installed_bytes + 1023) // 1024)
        control = (
            f"Package: {args.name}\n"
            f"Version: {args.version}-{args.release}\n"
            "Section: utils\n"
            "Priority: optional\n"
            f"Architecture: {args.architecture}\n"
            f"Maintainer: {args.maintainer}\n"
            f"Installed-Size: {installed_kib}\n"
            f"Depends: {args.depends}\n"
            f"Description: {args.description}\n"
            " TQSDK C++ data/trading gateway and SolidJS terminal.\n"
        )
        control_tar = tmpdir / "control.tar.gz"
        build_control_tar(control_tar, control, md5rows, args.postinst)
        build_deb(control_tar, data_tar, args.output.resolve())

    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
