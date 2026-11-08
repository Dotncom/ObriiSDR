"""Native helper regressions. Standard library only; no real/private paths."""
import argparse
import csv
import io
import hashlib
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

COMMIT = "82017594a1c6cacfaa7e86954c4ae7b3a5825a3d"
ROOT = Path(os.environ.get("LOCALAPPDATA", os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))) / "ObriiSDR/itu_hf"
PARSER = argparse.ArgumentParser()
PARSER.add_argument("--helper", required=True)
PARSER.add_argument("--vendor", default=str(Path(__file__).resolve().parent.parent / "third_party/itu_hf"))
PARSER.add_argument("--data", default="")
ARGS = None


def run_input(text):
    ROOT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="model-test-", dir=ROOT) as directory:
        job = Path(directory)
        (job / "request.in").write_text(text, encoding="ascii")
        process = subprocess.run([str(Path(ARGS.helper).resolve()), "-s", "-c", "request.in", "result.csv"],
                                 cwd=job, capture_output=True, timeout=120, check=False)
        # POSIX truncates 1000 to 232, but this pinned source's main returns 0.
        if process.returncode != 0:
            raise AssertionError(f"Pinned helper exit {process.returncode}: {process.stdout.decode(errors='replace')}")
        with (job / "result.csv").open(encoding="ascii", newline="") as stream:
            return [{key: float(value) for key, value in row.items()} for row in csv.DictReader(stream)]


def fixture(name, report=None):
    text = (Path(ARGS.vendor) / "ITURHFProp/Bin" / (name + ".in")).read_text(encoding="ascii").replace("\x1a", "")
    text = re.sub(r'^DataFilePath .*$', 'DataFilePath "' + Path(ARGS.data).resolve().as_posix() + '/"', text, flags=re.M)
    if report:
        text = re.sub(r'^RptFileFormat .*$', 'RptFileFormat "' + report + '"', text, flags=re.M)
    return text


def request(watts=1000, gain=0, bandwidth=1000, hour="1,7,13,19,24", frequencies="3.50,7.10,14.10,21.10", month=4):
    # Synthetic published endpoints; never an operator's own station.
    text = f'''PathName "Synthetic HF regression"
Path.L_tx.lat 1.42
Path.L_tx.lng 103.73
TXAntFilePath "ISOTROPIC"
TXGOS 0
RXAntFilePath "ISOTROPIC"
RXGOS {gain}
AntennaOrientation "TX2RX"
Path.year 1981
Path.month {month}
Path.hour {hour}
Path.SSN 140
Path.frequency {frequencies}
Path.txpower {10 * math.log10(watts / 1000)}
Path.BW {bandwidth}
Path.SNRr 10
Path.SNRXXp 90
Path.ManMadeNoise "RURAL"
Path.Modulation "ANALOG"
Path.SorL "SHORTPATH"
RptFileFormat "RPT_D | RPT_BMUF | RPT_BMUFD | RPT_OPMUF | RPT_OPMUFD | RPT_PR | RPT_NOISETOTAL | RPT_SNR | RPT_SNRD | RPT_SNRXX | RPT_BCR"
DataFilePath "{Path(ARGS.data).resolve().as_posix()}/"
latinc 1
lnginc 1
'''
    for corner in ("LL", "LR", "UL", "UR"):
        text += f"{corner}.lat 40.0\n{corner}.lng 116.4\n"
    return text


class ModelTests(unittest.TestCase):
    def test_pinned_source_and_data_blobs(self):
        import json
        manifest = json.loads((Path(ARGS.vendor) / "SOURCE_MANIFEST.json").read_text(encoding="utf-8-sig"))
        self.assertEqual(manifest["commit"], COMMIT)
        for entry in manifest["files"]:
            source = Path(ARGS.data) / Path(entry["path"]).name if entry["path"].startswith("P533/Data/") else Path(ARGS.vendor) / entry["path"]
            if entry["path"].startswith("P533/Data/") and not source.exists():
                continue
            with self.subTest(path=entry["path"]):
                content = source.read_bytes()
                self.assertEqual(len(content), entry["size"])
                self.assertEqual(hashlib.sha1(f"blob {len(content)}\0".encode("ascii") + content).hexdigest(), entry["sha"])

    def test_published_luxemburg_noise(self):
        rows = run_input(fixture("1-5-85"))
        self.assertEqual(len(rows), 1)
        published = (Path(ARGS.vendor) / "ITURHFProp/Bin/1-5-85.out").read_text(encoding="ascii")
        record = next(line for line in published.splitlines() if line.startswith("05,"))
        values = list(map(float, next(csv.reader(io.StringIO(record)))))
        names = ["month", "hour", "frequency", "FaA", "FaM", "FaG", "DuA", "DlA", "DuM", "DlM", "DuG", "DlG", "DuT", "DlT", "FamT"]
        for name, expected in zip(names, values):
            with self.subTest(name=name):
                self.assertAlmostEqual(rows[0][name], expected, delta=0.011)

    def test_published_kranji_muf(self):
        # The official fixture uses a directional RX pattern. Replace only that
        # pattern and compare antenna-independent MUFs, distance and external noise.
        text = fixture("itu_old22012020", "RPT_D | RPT_BMUF | RPT_BMUFD | RPT_OPMUF | RPT_OPMUFD | RPT_NOISETOTAL")
        text = re.sub(r'^RXAntFilePath .*$', 'RXAntFilePath "ISOTROPIC"', text, flags=re.M)
        row = run_input(text)[0]
        expected = {"distance": 4477.003, "BMUF": 27.770, "MUF50": 27.770,
                    "MUF90": 21.752, "MUF10": 36.427, "OPMUF": 36.101,
                    "OPMUF90": 28.277, "OPMUF10": 47.355, "FamT": 45.490}
        published = (Path(ARGS.vendor) / "ITURHFProp/Bin/itu_old22012020.out").read_text(encoding="ascii")
        for name, value in expected.items():
            with self.subTest(name=name):
                self.assertIn(f"{value:.3f}", published)
                self.assertAlmostEqual(row[name], value, delta=0.016)

    def test_grid_and_percentiles(self):
        rows = run_input(request())
        self.assertEqual(len(rows), 20)
        self.assertEqual({r["hour"] - 1 for r in rows}, {0, 6, 12, 18, 23})
        self.assertEqual(len({(r["hour"], r["frequency"]) for r in rows}), len(rows))
        for row in rows:
            self.assertTrue(all(math.isfinite(v) for v in row.values()))
            self.assertLessEqual(row["MUF90"], row["MUF50"])
            self.assertLessEqual(row["MUF50"], row["MUF10"])
            self.assertLessEqual(row["SNRXXp"], row["SNR"])
            self.assertGreaterEqual(row["BCR"], 0)
            self.assertLessEqual(row["BCR"], 100)
        self.assertGreater(len({r["BMUF"] for r in rows}), 1)
        self.assertGreater(len({r["SNR"] for r in rows}), 1)

    def test_power_gain_and_bandwidth_units(self):
        base = run_input(request(hour="13", frequencies="7.10"))[0]
        more_power = run_input(request(watts=10000, hour="13", frequencies="7.10"))[0]
        gain = run_input(request(gain=3, hour="13", frequencies="7.10"))[0]
        wide = run_input(request(bandwidth=10000, hour="13", frequencies="7.10"))[0]
        for key in ("PR", "SNR"):
            self.assertAlmostEqual(more_power[key] - base[key], 10, delta=0.011)
            self.assertAlmostEqual(gain[key] - base[key], 3, delta=0.011)
        self.assertAlmostEqual(wide["SNR"] - base["SNR"], -10, delta=0.011)
        self.assertAlmostEqual(wide["PR"], base["PR"], delta=0.011)
        self.assertGreaterEqual(more_power["BCR"], base["BCR"])
        self.assertLessEqual(wide["BCR"], base["BCR"])

    def test_all_month_data(self):
        for month in range(1, 13):
            if not (Path(ARGS.data) / f"ionos{month:02d}.bin").exists():
                continue
            with self.subTest(month=month):
                row = run_input(request(hour="13", frequencies="7.10", month=month))[0]
                self.assertTrue(all(math.isfinite(value) for value in row.values()))
                self.assertEqual(row["month"], month)

    def test_intermediate_and_long_path(self):
        intermediate = request(hour="13", frequencies="7.10,14.10,21.10").replace("116.4", "180.0").replace("40.0", "1.42")
        for row in run_input(intermediate):
            self.assertGreater(row["distance"], 7000)
            self.assertLess(row["distance"], 9000)
            self.assertTrue(all(math.isfinite(value) for value in row.values()))
        long_path = request(hour="13", frequencies="7.10,14.10,21.10").replace('"SHORTPATH"', '"LONGPATH"')
        for row in run_input(long_path):
            self.assertGreater(row["distance"], 30000)
            self.assertTrue(all(math.isfinite(value) for value in row.values()))
            self.assertGreaterEqual(row["BCR"], 0)
            self.assertLessEqual(row["BCR"], 100)


if __name__ == "__main__":
    ARGS, remaining = PARSER.parse_known_args()
    ARGS.data = ARGS.data or os.environ.get("ITU_HF_DATA", str(ROOT / "data"))
    unittest.main(argv=[__file__] + remaining)
