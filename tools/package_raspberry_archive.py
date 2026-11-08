"""Archive the sanitized Raspberry stage with portable, reproducible metadata."""

import argparse
import gzip
import os
import pathlib
import re
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version")
    parser.add_argument("--epoch", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version) or args.epoch < 0:
        parser.error("Expected a semantic release version and nonnegative epoch")
    root = pathlib.Path(__file__).resolve().parent.parent
    release = root / "release"
    source = release / "ObriiSDR-raspberry-source"
    output = release / f"ObriiSDR-v{args.version}-raspberry-source.tar.gz"
    if not source.is_dir() or source.resolve() != source:
        parser.error("Create the ordinary sanitized source stage first")
    entries = [source] + sorted(source.rglob("*"))
    for path in entries:
        if path.is_symlink() or getattr(path, "is_junction", lambda: False)() or not path.resolve().is_relative_to(source):
            parser.error("Source stage contains a link or an escaping path")
        if not path.is_dir() and not path.is_file():
            parser.error("Source stage contains a non-regular entry")
    if output.resolve().parent != release or output.is_symlink():
        parser.error("Archive output must be an ordinary file in the release folder")
    with output.open("wb") as raw:
        with gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=args.epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in entries:
                    name = path.relative_to(release).as_posix()
                    info = archive.gettarinfo(str(path), name)
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = args.epoch
                    info.pax_headers = {}
                    info.mode = 0o755 if path.is_dir() or path.suffix == ".sh" else 0o644
                    if path.is_file():
                        with path.open("rb") as stream:
                            archive.addfile(info, stream)
                    else:
                        archive.addfile(info)
    print(f"Created {output.name}: {len(entries)} entries; normalized owners and modes")


if __name__ == "__main__":
    main()
