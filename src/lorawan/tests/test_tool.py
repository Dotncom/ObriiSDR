import os
from pathlib import Path
import subprocess
import unittest


@unittest.skipUnless(os.environ.get("OBRII_LORAWAN_TOOL"), "Offline tool not configured")
class OfflineTool(unittest.TestCase):
    def run_tool(self, text):
        return subprocess.run([os.environ["OBRII_LORAWAN_TOOL"], "--pipe"], input=text, text=True,
            encoding="ascii", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)

    def test_independent_vectors(self):
        vectors = [line.split("\t") for line in (Path(__file__).parent / "vectors.tsv").read_text(encoding="ascii").splitlines()
                   if line and not line.startswith("#")]
        result = self.run_tool("\n".join(v[0] for v in vectors) + "\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), ["ok " + v[1] for v in vectors])
        commands, expected = [], []
        for command, wire in vectors:
            p = command.split()
            if p[0] != "data-encode":
                continue
            commands.append(f"data-decode {p[1]} {p[2]} {p[5]} {wire}")
            expected.append("ok " + " ".join((p[3], p[4], p[5], str(int(p[6], 16)), p[7], p[8], p[9])))
        decoded = self.run_tool("\n".join(commands) + "\n")
        self.assertEqual(decoded.returncode, 0, decoded.stderr)
        self.assertEqual(decoded.stdout.splitlines(), expected)

    def test_invalid_and_oversized_commands(self):
        result = self.run_tool("unknown\njoin-request secret short key -1\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(len(result.stdout.splitlines()), 2)
        self.assertNotIn("secret", result.stdout + result.stderr)
        self.assertTrue(all(line.startswith("error ") for line in result.stdout.splitlines()))
        for text in ("x" * 4097 + "\n", "\r" * 4097 + "\n", "unterminated"):
            with self.subTest(size=len(text)):
                result = self.run_tool(text)
                self.assertEqual(result.returncode, 1)
                self.assertFalse(result.stdout)
