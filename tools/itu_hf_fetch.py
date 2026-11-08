"""Selective, pinned source/data fetcher for native Linux (also works on Windows)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
from urllib.parse import quote
from urllib.request import Request, urlopen

COMMIT = "82017594a1c6cacfaa7e86954c4ae7b3a5825a3d"
REPO = "https://api.github.com/repos/ITU-R-Study-Group-3/ITU-R-HF"
RAW = f"https://raw.githubusercontent.com/ITU-R-Study-Group-3/ITU-R-HF/{COMMIT}"


def read(url):
    with urlopen(Request(url, headers={"User-Agent": "ObriiSDR-optional-itu-hf-fetch"}), timeout=120) as response:
        return response.read()


def blob_sha(data):
    return hashlib.sha1(f"blob {len(data)}\0".encode("ascii") + data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parent.parent / "third_party/itu_hf")
    parser.add_argument("--data-root", type=Path, default=Path(os.environ.get("LOCALAPPDATA", os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))) / "ObriiSDR/itu_hf/data")
    parser.add_argument("--months", nargs="+", type=int, default=list(range(1, 13)))
    args = parser.parse_args()
    if not args.months or any(month < 1 or month > 12 for month in args.months):
        parser.error("months must be in 1..12")
    response = json.loads(read(f"{REPO}/git/trees/{COMMIT}?recursive=1"))
    if response.get("truncated"):
        raise RuntimeError("Pinned tree listing was truncated; refusing an incomplete fetch")
    tree = response["tree"]
    source_pattern = r"^(P533/Src/P533|P372/Src/P372|ITURHFProp/Src/ITURHFProp)/[^/]+\.(c|h)$"
    fixture_pattern = r"^ITURHFProp/Bin/(1-5-85|itu_old22012020)\.(in|out)$"
    sources = [item for item in tree if item["type"] == "blob" and
               (re.match(source_pattern, item["path"]) or re.match(fixture_pattern, item["path"]) or item["path"] == "README.md")]
    datasets = [item for item in tree if re.match(r"^P533/Data/(ionos\d\d\.bin|COEFF\d\dW\.txt|P1239-3 Decile Factors\.txt)$", item["path"])]
    selected = {"P1239-3 Decile Factors.txt"}
    for month in set(args.months):
        selected.update((f"ionos{month:02d}.bin", f"COEFF{month:02d}W.txt"))
    if not selected.issubset({Path(entry["path"]).name for entry in datasets}):
        raise RuntimeError("Required model data is missing from the pinned tree")
    files = [(entry, args.source_root / entry["path"]) for entry in sources]
    files += [(entry, args.data_root / Path(entry["path"]).name) for entry in datasets if Path(entry["path"]).name in selected]
    for entry, destination in files:
        if destination.exists() and destination.stat().st_size == entry["size"] and blob_sha(destination.read_bytes()) == entry["sha"]:
            print("Cached", entry["path"])
            continue
        data = read(RAW + "/" + quote(entry["path"]))
        if len(data) != entry["size"] or blob_sha(data) != entry["sha"]:
            raise RuntimeError("Pinned source/data blob verification failed: " + entry["path"])
        destination.parent.mkdir(parents=True, exist_ok=True)
        part = destination.with_name(destination.name + ".download")
        part.write_bytes(data)
        part.replace(destination)
        print("Verified", entry["path"])
    manifest = {"commit": COMMIT, "files": [{key: entry[key] for key in ("path", "size", "sha")} for entry in sources + datasets]}
    (args.source_root / "SOURCE_MANIFEST.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("Pinned source", COMMIT, "; data", args.data_root)


if __name__ == "__main__":
    main()
