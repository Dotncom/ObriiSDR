#!/usr/bin/env python3
"""Deterministic, synthetic-only bridge tests. --gnu requires GNU Radio.

All TCP tests bind IPv4 loopback on ephemeral ports. No RF or app access.
This file also serves as a local GNU plugin fixture for spawned workers.
"""

import argparse
import json
import math
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import obrii_gr_bridge as bridge
import numpy as np


TOKEN = "synthetic-selftest-only-0123456789abcdef"
RUN_GNU = False


def make_block(sample_rate):
    """Local-only fixtures selected by synthetic sample rate, not TCP code."""
    from gnuradio import blocks, filter, gr

    if sample_rate == 97000:
        return filter.fir_filter_ccc(1, [0.5, 0.25, 0.125])
    if sample_rate == 98000:
        return filter.fir_filter_ccc(2, [1.0])
    if sample_rate == 99000:
        return blocks.multiply_const_ff(2.0)
    if sample_rate == 100000:
        class MissingOutput(gr.basic_block):
            def __init__(self):
                super().__init__(name="Synthetic missing output", in_sig=[np.complex64],
                                 out_sig=[np.complex64])

            def general_work(self, inputs, _outputs):
                self.consume(0, len(inputs[0]))
                return 0

            def forecast(self, _noutput, _ninputs):
                return [1]
        return MissingOutput()
    if sample_rate == 101000:
        class TooMuchOutput(gr.basic_block):
            def __init__(self):
                super().__init__(name="Synthetic excess output", in_sig=[np.complex64],
                                 out_sig=[np.complex64])

            def general_work(self, inputs, outputs):
                count = min(len(inputs[0]), len(outputs[0]) // 2)
                outputs[0][:2 * count] = 1
                self.consume(0, count)
                return 2 * count

            def forecast(self, _noutput, _ninputs):
                return [1]
        return TooMuchOutput()
    if sample_rate == 102000:
        class NonFinite(gr.sync_block):
            def __init__(self):
                super().__init__(name="Synthetic non-finite output", in_sig=[np.complex64],
                                 out_sig=[np.complex64])

            def work(self, _inputs, outputs):
                outputs[0][:] = np.nan
                return len(outputs[0])
        return NonFinite()
    if sample_rate == 103000:
        class IdentityHierarchy(gr.hier_block2):
            def __init__(self):
                super().__init__("Synthetic hierarchy", gr.io_signature(1, 1, 8),
                                 gr.io_signature(1, 1, 8))
                self.connect(self, blocks.multiply_const_cc(1), self)
        return IdentityHierarchy()
    return blocks.multiply_const_cc(2.0)


def request(count=512, first=0, **changes):
    message = {"type": "work", "stream": "test-stream", "generation": "1", "epoch": "1",
               "firstSample": str(first), "sampleCount": count, "sampleRate": 96000,
               "centerHz": 100100000, "gap": False, "returnIq": True,
               "ffts": [{"id": "fft", "length": 512, "window": 0, "refreshMs": 20}]}
    message.update(changes)
    return message


def iq(values):
    return np.asarray(values, dtype="<c8").tobytes()


def spectrum(message, payload, index=0):
    fft = message["spectra"][index]
    return np.frombuffer(payload, dtype="<f4", count=fft["length"], offset=fft["offsetBytes"])


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class ReferenceTests(unittest.TestCase):
    def setUp(self):
        self.clock = Clock()
        self.session = bridge.DspSession(bridge.ProcessorFactory("numpy"), self.clock)
        self.addCleanup(self.session.close)

    def process(self, message, samples=None):
        if samples is None:
            samples = np.zeros(message["sampleCount"], dtype=np.complex64)
        return self.session.process(message, iq(samples))

    def test_tone_window_scaling_order_and_enbw(self):
        for code in range(5):
            with self.subTest(window=code):
                count, bin_index = 1024, -37
                tone = (0.5 * np.exp(2j * np.pi * bin_index * np.arange(count) / count))
                fft = {"id": "tone", "length": count, "window": code, "refreshMs": 20}
                message, payload = self.process(request(count, ffts=[fft]), tone)
                levels = spectrum(message, payload)
                self.assertEqual(int(levels.argmax()), count // 2 + bin_index)
                self.assertAlmostEqual(float(levels.max()), 20 * math.log10(0.5), places=4)
                self.assertEqual(payload[:count * 8], iq(tone))
                p = 2 * np.pi * np.arange(count) / (count - 1)
                windows = [np.ones(count), np.hanning(count), np.hamming(count),
                           0.35875 - 0.48829*np.cos(p) + 0.14128*np.cos(2*p) - 0.01168*np.cos(3*p),
                           0.21557895 - 0.41663158*np.cos(p) + 0.277263158*np.cos(2*p)
                           - 0.083578947*np.cos(3*p) + 0.006947368*np.cos(4*p)]
                expected_rbw = 96000 * np.sum(windows[code]**2) / np.sum(windows[code])**2
                self.assertAlmostEqual(message["spectra"][0]["rbwHz"], expected_rbw, places=10)
                self.assertEqual(message["spectra"][0]["binWidthHz"], 96000/count)
                self.assertTrue(np.isfinite(levels).all())
                self.assertGreaterEqual(float(levels.min()), -200)
                self.assertTrue(math.isfinite(message["processingMs"]))

    def test_history_refresh_and_two_fft_offsets(self):
        ffts = [{"id": "a", "length": 512, "window": 0, "refreshMs": 20},
                {"id": "b", "length": 1024, "window": 1, "refreshMs": 100}]
        result, _ = self.process(request(256, ffts=ffts))
        self.assertEqual(result["spectra"], [])
        result, raw = self.process(request(256, 256, ffts=ffts))
        self.assertEqual([f["id"] for f in result["spectra"]], ["a"])
        np.testing.assert_array_equal(spectrum(result, raw), -200)
        self.clock.now = 0.01
        result, _ = self.process(request(512, 512, ffts=ffts))
        self.assertEqual([f["id"] for f in result["spectra"]], ["b"])
        self.clock.now = 0.2
        result, raw = self.process(request(128, 1024, ffts=ffts))
        self.assertEqual(len(result["spectra"]), 2)
        self.assertEqual(result["spectra"][0]["offsetBytes"], 128*8)
        self.assertEqual(result["spectra"][1]["offsetBytes"], 128*8+512*4)
        self.assertEqual(len(raw), 128*8+512*4+1024*4)

    def test_discontinuities_reset_history_and_processor(self):
        changes = [{"gap": True}, {"firstSample": "999"}, {"generation": "2"}, {"epoch": "2"},
                   {"sampleRate": 192000}, {"centerHz": 123000000},
                   {"ffts": [{"id": "new", "length": 512, "window": 0, "refreshMs": 20}]},
                   {"ffts": [{"id": "fft", "length": 512, "window": 1, "refreshMs": 20}]},
                   {"ffts": [{"id": "fft", "length": 512, "window": 0, "refreshMs": 30}]}]
        for change in changes:
            with self.subTest(change=change):
                self.session.close()
                self.process(request(256))
                old = self.session.streams["test-stream"].processor
                result, _ = self.process(request(256, 256, **change))
                self.assertEqual(result["spectra"], [])
                self.assertIsNot(old, self.session.streams["test-stream"].processor)
        self.session.close()
        self.process(request(256))
        old = self.session.streams["test-stream"].processor
        result, _ = self.process(request(256, 256))
        self.assertEqual(len(result["spectra"]), 1)
        self.assertIs(old, self.session.streams["test-stream"].processor)

    def test_return_selection_preserves_filter_history(self):
        self.process(request(256))
        old = self.session.streams["test-stream"].processor
        result, raw = self.process(request(256, 256, returnIq=False))
        self.assertEqual(len(result["spectra"]), 1)
        self.assertEqual(result["iqBytes"], 0)
        self.assertEqual(len(raw), 512*4)
        self.assertIs(old, self.session.streams["test-stream"].processor)

    def test_stream_limit_and_independence(self):
        for key in ("a", "b"):
            self.process(request(256, stream=key))
        with self.assertRaises(bridge.ProtocolError):
            self.process(request(256, stream="c"))
        result, _ = self.process(request(256, 256, stream="a"))
        self.assertEqual(len(result["spectra"]), 1)
        self.assertEqual(self.session.streams["b"].used, 256)

    def test_maximum_sizes_and_finite_extremes(self):
        ffts = [{"id": str(i), "length": 65536, "window": i, "refreshMs": 20} for i in range(2)]
        for first in (0, 65536):
            result, raw = self.process(request(65536, first, ffts=ffts),
                                       np.full(65536, np.finfo(np.float32).max, np.complex64))
            self.assertLessEqual(len(raw), bridge.MAX_BINARY)
            self.assertEqual(self.session.streams["test-stream"].used, 65536)
            for index in range(len(result["spectra"])):
                self.assertTrue(np.isfinite(spectrum(result, raw, index)).all())
            self.clock.now += 1
        result, raw = self.process(request(512, returnIq=False))
        self.assertEqual(result["iqBytes"], 0)
        self.assertEqual(result["spectra"][0]["offsetBytes"], 0)
        self.assertEqual(len(raw), 512*4)

    def test_strict_schema(self):
        mutations = [{"type": "eval"}, {"sampleCount": True}, {"sampleCount": 0},
                     {"sampleCount": 65537}, {"sampleRate": float("inf")}, {"sampleRate": 999},
                     {"sampleRate": 2000001}, {"centerHz": float("nan")}, {"centerHz": -1},
                     {"centerHz": 8000000001}, {"generation": 1}, {"epoch": "-1"},
                     {"epoch": str(1 << 64)}, {"firstSample": "1.0"}, {"firstSample": "\u0661"},
                     {"firstSample": str((1 << 64)-1)}, {"stream": ""}, {"stream": "a"*129},
                     {"stream": "\ud800"}, {"gap": 0}, {"returnIq": 1}, {"ffts": {}},
                     {"unrecognized": "ignored?"}]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(bridge.ProtocolError):
                bridge.validate_work(request(**mutation), 512*8)
        for field, value in (("length", 513), ("length", True), ("window", 5), ("refreshMs", 19),
                             ("refreshMs", 2001), ("id", "")):
            message = request()
            message["ffts"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(bridge.ProtocolError):
                bridge.validate_work(message, 512*8)
        for ffts in ([request()["ffts"][0]]*2, [request()["ffts"][0]]*3):
            with self.assertRaises(bridge.ProtocolError):
                bridge.validate_work(request(ffts=ffts), 512*8)
        for value in (np.inf, -np.inf, np.nan):
            with self.assertRaises(bridge.ProtocolError):
                self.process(request(), np.full(512, value, np.complex64))
        with self.assertRaises(bridge.ProtocolError):
            self.session.process(request(), b"short")
        bridge.validate_work(request(1, (1 << 64)-2, generation=str((1 << 64)-1)), 8)


class LoopbackTests(unittest.TestCase):
    def setUp(self):
        self.server = bridge.BridgeServer("127.0.0.1", 0, TOKEN,
                                          bridge.ProcessorFactory("numpy"), timeout=0.75)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.connections = []
        self.addCleanup(self.cleanup)

    def cleanup(self):
        for connection in self.connections:
            connection.close()
        self.server.close()
        self.thread.join(5)
        self.assertFalse(self.thread.is_alive())

    def connect(self, authenticate=True):
        connection = socket.create_connection(self.server.address, timeout=3)
        self.connections.append(connection)
        if authenticate:
            bridge.send_frame(connection, {"type": "hello", "version": 1, "token": TOKEN})
            hello, payload = bridge.receive_frame(connection, 3)
            self.assertEqual(hello, {"type": "hello", "version": 1})
            self.assertEqual(payload, b"")
        return connection

    def assert_closed(self, connection):
        connection.settimeout(3)
        try:
            self.assertEqual(connection.recv(1), b"")
        except ConnectionResetError:
            pass
        connection.close()
        if self.server.client_thread is not None:
            self.server.client_thread.join(3)

    def rejected(self, raw, authenticated=True):
        connection = self.connect(authenticated)
        connection.sendall(raw)
        if authenticated:
            result, body = bridge.receive_frame(connection, 3)
            self.assertEqual(result["type"], "error")
            self.assertLessEqual(len(result["error"]), 128)
            self.assertNotIn(TOKEN, json.dumps(result))
            self.assertEqual(body, b"")
        self.assert_closed(connection)

    def test_fragmented_header_json_and_iq(self):
        connection = self.connect(False)
        hello = bridge.encode_frame({"type": "hello", "version": 1, "token": TOKEN})
        for byte in hello:
            connection.sendall(bytes([byte]))
        self.assertEqual(bridge.receive_frame(connection, 3)[0]["type"], "hello")
        tone = np.arange(37, dtype=np.float32) + 1j*np.arange(37, dtype=np.float32)[::-1]
        message = request(37, ffts=[])
        frame = bridge.encode_frame(message, iq(tone))
        for start in range(0, len(frame), 7):
            connection.sendall(frame[start:start+7])
        result, payload = bridge.receive_frame(connection, 3)
        self.assertEqual(result["type"], "result")
        self.assertEqual(payload, iq(tone))
        for field in ("stream", "generation", "epoch", "firstSample", "sampleCount", "sampleRate", "centerHz"):
            self.assertEqual(result[field], message[field])

    def test_authentication_and_no_work_before_auth(self):
        messages = [{"type": "hello", "version": 1, "token": "wrong"},
                    {"type": "hello", "version": True, "token": TOKEN},
                    {"type": "hello", "version": 1, "token": "\u2603"}, request()]
        for message in messages:
            with self.subTest(message_type=message["type"]):
                self.rejected(bridge.encode_frame(message), False)
        self.rejected(bridge.encode_frame({"type": "hello", "version": 1, "token": TOKEN}, b"x"), False)

    def test_one_client_and_reconnect(self):
        connection = self.connect()
        second = self.connect(False)
        second.settimeout(3)
        try:
            self.assertEqual(second.recv(1), b"")
        except ConnectionResetError:
            pass
        bridge.send_frame(connection, request(1, ffts=[]), iq([1+2j]))
        self.assertEqual(bridge.receive_frame(connection, 3)[1], iq([1+2j]))
        connection.close()
        self.server.client_thread.join(3)
        self.connect()

    def test_uint64_counters_above_json_double_precision(self):
        connection = self.connect()
        message = request(512, (1 << 60) + 17, generation=str((1 << 64)-1),
                          epoch=str((1 << 63)+13))
        bridge.send_frame(connection, message, iq(np.zeros(512)))
        reply, _ = bridge.receive_frame(connection, 3)
        for field in ("generation", "epoch", "firstSample"):
            self.assertEqual(reply[field], message[field])
            self.assertIsInstance(reply[field], str)

    def test_slow_consumer_send_deadline(self):
        connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.connections.append(connection)
        connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        connection.settimeout(3)
        connection.connect(self.server.address)
        bridge.send_frame(connection, {"type": "hello", "version": 1, "token": TOKEN})
        bridge.receive_frame(connection, 3)
        self.server.client.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)
        ffts = [{"id": str(i), "length": 65536, "window": i, "refreshMs": 20} for i in range(2)]
        bridge.send_frame(connection, request(65536, ffts=ffts), iq(np.ones(65536)), timeout=3)
        self.server.client_thread.join(3)
        self.assertFalse(self.server.client_thread.is_alive(), "slow consumer retained session")
        self.connect()

    def test_oversized_bad_magic_and_invalid_json_before_body(self):
        for header in (bridge.HEADER.pack(b"BADMAGIC", 1, 0),
                       bridge.HEADER.pack(bridge.MAGIC, bridge.MAX_JSON+1, 0),
                       bridge.HEADER.pack(bridge.MAGIC, 1, bridge.MAX_BINARY+1),
                       bridge.HEADER.pack(bridge.MAGIC, 0, 0)):
            self.rejected(header)
        for body in (b"{", b"[]", b"\xff", b'{"type":"work","type":"work"}',
                     b'{"type":"work","sampleRate":NaN}', b"["*2000+b"]"*2000):
            self.rejected(bridge.HEADER.pack(bridge.MAGIC, len(body), bridge.MAX_BINARY) + body)
        body = json.dumps(request()).encode()
        self.rejected(bridge.HEADER.pack(bridge.MAGIC, len(body), bridge.MAX_BINARY) + body)

    def test_unknown_request_and_nonfinite_input(self):
        self.rejected(bridge.encode_frame({"type": "run", "command": TOKEN}))
        self.rejected(bridge.encode_frame(request(), iq(np.full(512, np.nan))))

    def test_timeout_partial_and_slow_fragmented_frame(self):
        connection = self.connect(False)
        connection.sendall(b"OBD")
        self.assert_closed(connection)
        connection = self.connect()
        frame = bridge.encode_frame(request(1, ffts=[]), iq([0]))
        connection.sendall(frame[:5])
        time.sleep(0.45)
        connection.sendall(frame[5:10])
        start = time.monotonic()
        self.assert_closed(connection)
        # A reset-on-each-read timeout would take another 0.75 seconds here.
        self.assertLess(time.monotonic()-start, 0.65)

    def test_truncated_body_and_new_session_history(self):
        connection = self.connect()
        frame = bridge.encode_frame(request(), iq(np.zeros(512)))
        connection.sendall(frame[:-8])
        connection.shutdown(socket.SHUT_WR)
        self.assert_closed(connection)
        connection = self.connect()
        bridge.send_frame(connection, request(256), iq(np.zeros(256)))
        result, _ = bridge.receive_frame(connection, 3)
        self.assertEqual(result["spectra"], [])


class CliTests(unittest.TestCase):
    def test_check_numpy_and_secret_not_echoed(self):
        script = str(Path(bridge.__file__).resolve())
        result = subprocess.run([sys.executable, "-I", "-B", script, "--check", "--engine", "numpy"],
                                capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        result = subprocess.run([sys.executable, "-I", "-B", script, "--token", TOKEN],
                                capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(TOKEN.encode(), result.stdout + result.stderr)


class GnuTests(unittest.TestCase):
    def setUp(self):
        if not RUN_GNU:
            self.skipTest("enable --gnu for actual GNU Radio integration")
        self.plugin = str(Path(__file__).resolve())

    def test_default_identity_persistent_scheduler(self):
        processor = bridge.GnuProcessor(96000, timeout=10)
        self.addCleanup(processor.close)
        pid = processor.process_handle.pid
        for count in (1, 17, 4096, 65536, 31):
            values = (np.arange(count) / max(count, 1) + 0.25j).astype(np.complex64)
            np.testing.assert_array_equal(processor.process(values), values)
            self.assertEqual(processor.process_handle.pid, pid)

    def test_hierarchical_plugin(self):
        processor = bridge.GnuProcessor(103000, self.plugin, 10)
        self.addCleanup(processor.close)
        np.testing.assert_array_equal(processor.process(np.ones(37, np.complex64)), 1)

    def test_multiply_plugin_over_loopback(self):
        server = bridge.BridgeServer("127.0.0.1", 0, TOKEN,
                                    bridge.ProcessorFactory("gnuradio", self.plugin, 10), timeout=15)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            with socket.create_connection(server.address, timeout=15) as connection:
                bridge.send_frame(connection, {"type": "hello", "version": 1, "token": TOKEN})
                bridge.receive_frame(connection, 15)
                values = (0.125 * np.exp(2j*np.pi*7*np.arange(512)/512)).astype(np.complex64)
                for first in (0, 512):
                    bridge.send_frame(connection, request(first=first), iq(values))
                    result, payload = bridge.receive_frame(connection, 15)
                    np.testing.assert_array_equal(np.frombuffer(payload[:4096], dtype="<c8"), values*2)
                    if result["spectra"]:
                        self.assertAlmostEqual(float(spectrum(result, payload).max()),
                                               20*math.log10(0.25), places=4)
        finally:
            server.close()
            thread.join(15)
            self.assertFalse(thread.is_alive())

    def test_immediate_reconnect_waits_for_scheduler_cleanup(self):
        server = bridge.BridgeServer("127.0.0.1", 0, TOKEN,
                                    bridge.ProcessorFactory("gnuradio", timeout=10), timeout=10)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            for token in (TOKEN, "x"*64, TOKEN):
                with socket.create_connection(server.address, timeout=10) as connection:
                    bridge.send_frame(connection, {"type": "hello", "version": 1, "token": token})
                    if token != TOKEN:
                        try:
                            self.assertEqual(connection.recv(1), b"")
                        except ConnectionResetError:
                            pass
                        continue
                    self.assertEqual(bridge.receive_frame(connection, 10)[0]["type"], "hello")
                    bridge.send_frame(connection, request(1, ffts=[]), iq([1+2j]))
                    self.assertEqual(bridge.receive_frame(connection, 10)[1], iq([1+2j]))
        finally:
            server.close()
            thread.join(10)
            self.assertFalse(thread.is_alive())

    def test_fir_state_survives_packets_and_resets(self):
        session = bridge.DspSession(bridge.ProcessorFactory("gnuradio", self.plugin, 10))
        self.addCleanup(session.close)
        values = (np.arange(1200) % 11 / 11 + 0.125j).astype(np.complex64)
        expected = np.convolve(values, np.array([0.5, 0.25, 0.125], np.complex64))[:len(values)]
        actual = []
        pid = None
        for start, count in ((0, 127), (127, 509), (636, 564)):
            _, payload = session.process(request(count, start, sampleRate=97000, ffts=[]),
                                         iq(values[start:start+count]))
            actual.extend(np.frombuffer(payload, dtype="<c8"))
            current = session.streams["test-stream"].processor.process_handle.pid
            if pid is not None:
                self.assertEqual(current, pid)
            pid = current
        np.testing.assert_allclose(actual, expected, atol=1e-6)
        for changes in ({"gap": True}, {"epoch": "2"}, {"generation": "2"},
                        {"firstSample": "777"}, {"centerHz": 100200000}):
            message = request(127, 1200, sampleRate=97000, ffts=[], **changes)
            _, payload = session.process(message, iq(values[:127]))
            np.testing.assert_allclose(np.frombuffer(payload, dtype="<c8"), expected[:127], atol=1e-6)

    def test_reject_rate_signature_missing_excess_and_nonfinite_output(self):
        for rate in (98000, 99000):
            with self.subTest(rate=rate), self.assertRaises(bridge.ProtocolError):
                bridge.GnuProcessor(rate, self.plugin, 10)
        for rate in (100000, 101000, 102000):
            with self.subTest(rate=rate):
                processor = bridge.GnuProcessor(rate, self.plugin, 10)
                processor.timeout = 0.75
                try:
                    with self.assertRaises(bridge.ProtocolError):
                        processor.process(np.ones(512, np.complex64))
                    self.assertIsNone(processor.process_handle)
                finally:
                    processor.close()


def main():
    global RUN_GNU
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gnu", action="store_true")
    args = parser.parse_args()
    RUN_GNU = args.gnu
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
