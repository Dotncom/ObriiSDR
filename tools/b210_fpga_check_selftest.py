#!/usr/bin/env python3
"""Exercise the B210 comparison, old CLI fallback and restore without a radio."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import b210_fpga_check as runner


FAKE_CLI = r"""#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
if args[0] == "devices":
    print(json.dumps({"deviceArgs": "type=b200, name=Clone, product=B210, serial=A1", "index": 0}))
    sys.exit()
def value(key):
    return args[args.index(key) + 1]
assert "--rx-dual" in args and "--progress" in args
assert value("--clock-source") == "internal"
assert value("--block-samples") == "65536"
assert os.environ["OBRII_UHD_NUM_RECV_FRAMES"] == "256"
rate = int(value("--rate"))
samples = rate * int(value("--seconds"))
image = Path(value("--fpga-image")).name
with open(os.environ["FAKE_LOG"], "a") as log:
    print(image, file=log)
diagnostics = dict(overflows=0, timeouts=0, timestampGaps=0, missingSamplesPerChannel=0,
                   blocksWithoutTimestamp=0, maxReadPauseUs=5)
print(json.dumps(dict(event="started", sampleRateHz=rate, centerFrequencyHz=int(value("--freq")))))
print(json.dumps(dict(event="progress", uhdRxDiagnostics={**diagnostics, "maxReadPauseUs": 17})))
print(json.dumps(dict(event="progress", uhdRxDiagnostics=diagnostics)))
if os.environ["FAKE_MODE"] == "failed" and image == "candidate.bin":
    diagnostics["timestampGaps"] = 1
if os.environ["FAKE_MODE"] == "baseline-gap" and image == "baseline.bin":
    diagnostics["overflows"] = 1
stopped = dict(event="stopped", samples=samples, rxChannels=[0, 1], elapsedSeconds=2)
if os.environ["FAKE_MODE"] != "old":
    stopped["uhdRxDiagnostics"] = diagnostics
print(json.dumps(stopped))
"""

FAKE_PROBE = """#!/usr/bin/env python3
import os
import sys
assert sys.argv[1:3] == ["--args", "type=b200,serial=A1,fpga=" + os.environ["BASELINE_IMAGE"]]
with open(os.environ["FAKE_LOG"], "a") as log:
    print("restore", file=log)
"""


class RunnerTest(unittest.TestCase):
    def test_nmea_ignores_bad_checksum_and_only_returns_aggregate(self):
        payload = b"$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\n"
        result = runner.summarize_nmea({b"GGA": payload + payload.replace(b"*47", b"*00")})
        self.assertTrue(result["fixReported"])
        self.assertEqual(result["satellitesUsed"], 8)
        self.assertEqual(result["sentences"], {"GGA": 1, "RMC": 0, "GSV": 0})
        self.assertNotIn("4807.038", json.dumps(result))

    @unittest.skipUnless(sys.platform.startswith("linux"), "Hardware runner uses Linux")
    def test_fake_cli_via_stdin(self):
        source = Path(runner.__file__).read_bytes()
        wrapper = ("import sys; exec(compile(sys.stdin.buffer.read(), '<test-b210>', 'exec'), "
                   "{'__name__':'__main__'})")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, content in (("lab-fake-cli", FAKE_CLI), ("probe", FAKE_PROBE)):
                path = root / name
                path.write_text(content, encoding="utf-8")
                path.chmod(0o700)
            baseline = (root / "baseline.bin").resolve()
            candidate = root / "candidate.bin"
            baseline.write_bytes(b"baseline")
            candidate.write_bytes(b"candidate")
            fingerprint = hashlib.sha256(candidate.read_bytes()).hexdigest()
            log = root / "radio.log"
            command = [sys.executable, "-c", wrapper, "--cli", str(root / "lab-fake-cli"),
                       "--uhd-probe", str(root / "probe"), "--baseline", str(baseline),
                       "--candidate", str(candidate), "--seconds", "2"]
            env = {**os.environ, "XDG_RUNTIME_DIR": directory,
                   "FAKE_LOG": str(log), "BASELINE_IMAGE": str(baseline)}
            for mode, expected, comparison in (("clean", 0, "clean"), ("failed", 1, "failed"),
                                                ("old", 1, "partial"), ("baseline-gap", 1, "failed")):
                with self.subTest(mode=mode):
                    log.write_text("")
                    child = subprocess.run(command + ["--expected-candidate-sha256", fingerprint],
                                           input=source, capture_output=True, timeout=12,
                                           env={**env, "FAKE_MODE": mode})
                    report = json.loads(child.stdout)
                    self.assertEqual(child.returncode, expected, child.stderr)
                    self.assertEqual(report["comparison"], comparison)
                    self.assertEqual(report["candidate"]["sha256"], fingerprint)
                    self.assertEqual(report["candidate"]["rx"]["channels"], [0, 1])
                    self.assertTrue(report["restored"])
                    self.assertEqual(log.read_text().splitlines(),
                                     ["baseline.bin", "candidate.bin", "restore"])
                    if mode == "old":
                        self.assertEqual(report["candidate"]["rx"]["diagnostics"]["maxReadPauseUs"], 17)
            log.write_text("")
            child = subprocess.run(command + ["--expected-candidate-sha256", "0" * 64],
                                   input=source, capture_output=True, timeout=12, env=env)
            self.assertEqual(child.returncode, 2)
            self.assertEqual(log.read_text(), "")
            self.assertIn("SHA-256", json.loads(child.stdout)["error"])


if __name__ == "__main__":
    unittest.main()
