#!/usr/bin/env python3
"""Create a Raspberry source archive with usable Unix file permissions."""

import argparse
import pathlib
import tarfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    workspace = pathlib.Path(__file__).resolve().parent.parent
    release = workspace / "release"
    source = args.source.resolve()
    output = args.output.resolve()
    if source.parent != release or not source.is_dir():
        parser.error("source must be a release staging directory")
    if output.parent != release or output.suffixes[-2:] != [".tar", ".gz"]:
        parser.error("output must be a release/*.tar.gz file")
    if source.is_symlink():
        parser.error("source must not be a symlink")

    with tarfile.open(output, "w:gz", format=tarfile.PAX_FORMAT) as archive:
        paths = [source, *sorted(source.rglob("*"))]
        for path in paths:
            if path.is_symlink() or not (path.is_file() or path.is_dir()):
                parser.error(f"unsupported source entry: {path}")
            relative = path.relative_to(release).as_posix()
            info = archive.gettarinfo(str(path), arcname=relative)
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mtime = int(path.stat().st_mtime)
            info.mode = 0o755 if path.is_dir() or path.suffix == ".sh" else 0o644
            if path.is_file():
                with path.open("rb") as source_file:
                    archive.addfile(info, source_file)
            else:
                archive.addfile(info)


if __name__ == "__main__":
    main()
