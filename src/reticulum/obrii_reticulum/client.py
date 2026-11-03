"""Owned child-process client usable by a designer, tests or an application host."""
from collections import deque
import json
import math
import os
from pathlib import Path
import queue
import subprocess
import threading
import uuid


class SidecarClient:
    def __init__(self, python, state_dir, *, transport=False, interfaces=("lora0",), mtu=255, bitrate=1100,
                 timeout=30, framing="obrn", access_file=None):
        if type(timeout) not in (int, float) or not math.isfinite(timeout) or not 0 < timeout <= 60:
            raise ValueError("Client timeout must be in (0, 60] seconds")
        self.timeout = timeout
        self._lock = threading.RLock()
        self._responses = queue.Queue(maxsize=8)
        self._errors = deque(maxlen=16)
        args = [str(python), "-u", "-m", "obrii_reticulum.service", "--state-dir", str(state_dir),
                "--mtu", str(mtu), "--bitrate", str(bitrate), "--framing", str(framing)]
        if transport:
            args.append("--transport")
        if access_file is not None:
            args += ["--access-file", str(access_file)]
        for name in interfaces:
            args += ["--interface", name]
        environment = os.environ.copy()
        environment["PYTHONPATH"] = str(Path(__file__).resolve().parents[1])
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        env=environment, text=True, encoding="utf-8", bufsize=1,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self._readers = [threading.Thread(target=self._read, daemon=True),
                         threading.Thread(target=self._stderr, daemon=True)]
        for reader in self._readers:
            reader.start()
        try:
            self.initial_status = self.call("status")
        except Exception:
            self.close(force=True)
            raise

    def _read(self):
        try:
            while line := self.process.stdout.readline(1024 * 1024):
                if not line.endswith("\n"):
                    break
                response = json.loads(line)
                self._responses.put(response, timeout=1)
        except Exception:
            pass
        finally:
            try:
                self._responses.put(None, timeout=1)
            except queue.Full:
                pass

    def _stderr(self):
        try:
            while line := self.process.stderr.readline(1000):
                self._errors.append(line)
        except (ValueError, OSError):
            pass

    def call(self, op, **arguments):
        with self._lock:
            if self.process.poll() is not None:
                raise RuntimeError("Reticulum sidecar exited: " + "".join(self._errors))
            identity = uuid.uuid4().hex
            request = json.dumps({"id": identity, "op": op, **arguments}, allow_nan=False, separators=(",", ":"))
            if len(request.encode("utf-8")) + 1 > 32768:
                raise ValueError("Sidecar request too large")
            try:
                self.process.stdin.write(request + "\n")
                self.process.stdin.flush()
            except (ValueError, OSError):
                self.close(force=True)
                raise RuntimeError("Reticulum sidecar input closed; process stopped") from None
            try:
                reply = self._responses.get(timeout=self.timeout)
            except queue.Empty:
                self.close(force=True)
                raise TimeoutError("Reticulum sidecar response timeout; process stopped") from None
            if reply is None:
                self.close(force=True)
                raise RuntimeError("Reticulum sidecar closed output: " + "".join(self._errors))
            if reply.get("id") != identity:
                self.close(force=True)
                raise RuntimeError("Reticulum response ID mismatch; process stopped")
            if not reply.get("ok"):
                raise ValueError(reply.get("error", "Reticulum command failed"))
            return reply["result"]

    def close(self, force=False):
        with self._lock:
            if self.process.poll() is None:
                if not force:
                    try:
                        self.call("shutdown")
                        self.process.wait(timeout=5)
                    except Exception:
                        force = True
                if force and self.process.poll() is None:
                    self.process.terminate()
                    try:
                        self.process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.process.wait(timeout=2)
            for reader in self._readers:
                reader.join(timeout=2)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                if stream:
                    stream.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
