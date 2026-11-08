"""Private, offline A/B checks of discriminator PCM; never requires privacy keys."""
import argparse
import pathlib
import re
import subprocess

import numpy as np


def fir(kind, rate):
    t = np.arange(-4 * rate // 4800, 4 * rate // 4800 + 1, dtype=float)
    if kind == "rrc":
        t /= rate / 4800.0
        beta = 0.2
        h = np.empty_like(t)
        for i, x in enumerate(t):
            if abs(x) < 1e-10:
                h[i] = 1 + beta * (4 / np.pi - 1)
            elif abs(abs(x) - 1 / (4 * beta)) < 1e-10:
                h[i] = beta / np.sqrt(2) * (
                    (1 + 2 / np.pi) * np.sin(np.pi / (4 * beta))
                    + (1 - 2 / np.pi) * np.cos(np.pi / (4 * beta)))
            else:
                h[i] = (np.sin(np.pi * x * (1 - beta))
                        + 4 * beta * x * np.cos(np.pi * x * (1 + beta))) / (
                            np.pi * x * (1 - (4 * beta * x) ** 2))
    else:
        cutoff = int(kind[2:])
        h = np.sinc(2 * cutoff / rate * t) * np.hamming(len(t))
    return h / h.sum()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pcm", type=pathlib.Path)
    parser.add_argument("dsd", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--rate", type=int, choices=(24000, 48000), default=24000)
    parser.add_argument("--legacy-mono", action="store_true",
                        help="Compare the old forced -nm decoder; default uses the managed DMR path")
    args = parser.parse_args()
    samples = np.fromfile(args.pcm, dtype="<i2").astype(float)
    args.output.mkdir(parents=True, exist_ok=True)
    for kind in ("baseline", "rrc", "lp3000", "lp4000", "lp4800"):
        if kind == "baseline":
            pcm = samples.astype("<i2")
        else:
            filtered = np.convolve(samples, fir(kind, args.rate), mode="full")[:len(samples)]
            pcm = np.clip(np.rint(filtered), -32768, 32767).astype("<i2")
        for filtering in (True, False):
            name = f"{args.pcm.stem}-{kind}-{'filter' if filtering else 'nofilter'}"
            command = [str(args.dsd.resolve()), "-fs", "-mc", "-i", "-",
                       "-s", str(args.rate), "-o", "null"]
            if args.legacy_mono:
                command.append("-nm")
            if not filtering:
                command.append("-l")
            result = subprocess.run(command, input=pcm.tobytes(), stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, timeout=30,
                                    cwd=args.output.resolve(),
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            log = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            (args.output / (name + ".log")).write_text(log, encoding="utf-8")
            codes = re.findall(r"Color Code=(\d+)", log)
            dominant = max(set(codes), key=codes.count) if codes else "--"
            print(name, "exit", result.returncode,
                  "sync-lines", len(codes), "other-CC-lines", sum(c != dominant for c in codes),
                  "FEC-lines", len(re.findall(r"^.*FEC ERR.*$", log, re.M)),
                  "CRC-lines", len(re.findall(r"^.*CRC ERR.*$", log, re.M)),
                  "MI-mismatch", log.count("MI Mismatch"),
                  "sync-loss", log.count("Sync: no sync"), flush=True)


if __name__ == "__main__":
    main()
