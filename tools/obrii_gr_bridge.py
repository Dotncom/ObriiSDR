#!/usr/bin/env python3
"""Bounded, software-only Obrii DSP v1 endpoint. No radio/device access.

GNU Radio runs in at most eight persistent, locally configured worker processes.
Only validated CF32 samples cross into the workers; TCP never carries code.
"""

import argparse
import hmac
import importlib.util
import ipaddress
import json
import math
import multiprocessing
import os
from pathlib import Path
import socket
import struct
import sys
import threading
import time
from dataclasses import dataclass


_dll_handles = []
if os.name == "nt":
    for _directory in (Path(sys.executable).parent / "Library" / "bin",
                       Path(sys.executable).parent / "DLLs"):
        if _directory.is_dir():
            _dll_handles.append(os.add_dll_directory(str(_directory)))

import numpy as np


MAGIC = b"OBDSP001"
HEADER = struct.Struct("<8sII")
MAX_JSON = 65536
MAX_BINARY = 2097152
MAX_SAMPLES = 65536
MAX_OUTPUT_SAMPLES = 131072
MAX_RATE_FACTOR = 64
MAX_INPUT_RESERVE = 4096
MAX_STREAMS = 8
UINT64_MAX = (1 << 64) - 1
FFT_LENGTHS = frozenset(1 << n for n in range(9, 17))
TOKEN_ENV = "OBRII_DSP_TOKEN"


class ProtocolError(Exception):
    """Messages are fixed, non-sensitive strings, never peer/plugin input."""


def _require(condition, reason):
    if not condition:
        raise ProtocolError(reason)


def _fields(value, names):
    _require(type(value) is dict and set(value) == set(names.split()),
             "invalid request fields")


def _integer(value, low, high):
    _require(type(value) is int and low <= value <= high, "invalid integer")
    return value


def _number(value, low, high):
    _require(type(value) in (int, float), "invalid number")
    try:
        result = float(value)
    except (ValueError, OverflowError):
        raise ProtocolError("invalid number") from None
    _require(math.isfinite(result) and low <= result <= high, "invalid number")
    return result


def _identifier(value):
    _require(type(value) is str and 1 <= len(value) <= 128, "invalid identifier")
    try:
        value.encode("utf-8")
    except UnicodeError:
        raise ProtocolError("invalid identifier") from None
    return value


def _uint64(value):
    _require(type(value) is str and 1 <= len(value) <= 20
             and all("0" <= c <= "9" for c in value), "invalid uint64 string")
    number = int(value)
    _require(number <= UINT64_MAX, "invalid uint64 string")
    return number


def validate_token(value):
    _require(type(value) is str and 32 <= len(value) <= MAX_JSON
             and value.isascii(), "token must contain at least 32 ASCII characters")
    return value.encode("ascii")


def validate_hello(message, binary_length, token):
    _fields(message, "type version token")
    _require(message["type"] == "hello" and type(message["version"]) is int
             and message["version"] == 1 and binary_length == 0, "authentication failed")
    supplied = message["token"]
    _require(type(supplied) is str and supplied.isascii(), "authentication failed")
    _require(hmac.compare_digest(supplied.encode("ascii"), token), "authentication failed")


def validate_work(message, binary_length):
    _require(message.get("type") == "work", "unknown request")
    _fields(message, "type stream generation epoch firstSample sampleCount sampleRate "
            "centerHz gap returnIq ffts")
    _identifier(message["stream"])
    _uint64(message["generation"])
    _uint64(message["epoch"])
    first = _uint64(message["firstSample"])
    count = _integer(message["sampleCount"], 1, MAX_SAMPLES)
    _require(first + count <= UINT64_MAX, "sample position overflow")
    _require(binary_length == count * 8, "IQ payload length mismatch")
    _number(message["sampleRate"], 1000, 2000000)
    _number(message["centerHz"], 0, 8e9)
    _require(type(message["gap"]) is bool and type(message["returnIq"]) is bool,
             "invalid boolean")
    ffts = message["ffts"]
    _require(type(ffts) is list and len(ffts) <= 2, "invalid FFT list")
    identifiers = set()
    for fft in ffts:
        _fields(fft, "id length window refreshMs")
        identifier = _identifier(fft["id"])
        _require(identifier not in identifiers, "duplicate FFT identifier")
        identifiers.add(identifier)
        length = _integer(fft["length"], 512, MAX_SAMPLES)
        _require(length in FFT_LENGTHS, "unsupported FFT length")
        _integer(fft["window"], 0, 4)
        _integer(fft["refreshMs"], 20, 2000)


def _json_pairs(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, "duplicate JSON field")
        result[key] = value
    return result


def _json_constant(_value):
    raise ProtocolError("non-finite JSON number")


def _remaining(deadline):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("frame deadline")
    return remaining


def _receive_exact(connection, length, deadline):
    data = bytearray(length)
    view = memoryview(data)
    offset = 0
    while offset < length:
        connection.settimeout(_remaining(deadline))
        received = connection.recv_into(view[offset:])
        if not received:
            raise EOFError
        offset += received
    return data


def receive_frame(connection, timeout, validator=None):
    # One absolute deadline covers fragmented header, JSON and binary together.
    deadline = time.monotonic() + timeout
    magic, json_length, binary_length = HEADER.unpack(
        _receive_exact(connection, HEADER.size, deadline))
    _require(magic == MAGIC, "invalid frame magic")
    _require(1 <= json_length <= MAX_JSON and binary_length <= MAX_BINARY,
             "frame length exceeded")
    try:
        message = json.loads(_receive_exact(connection, json_length, deadline).decode("utf-8"),
                             object_pairs_hook=_json_pairs, parse_constant=_json_constant)
    except (ValueError, UnicodeError, RecursionError):
        raise ProtocolError("malformed JSON") from None
    _require(type(message) is dict, "JSON object required")
    if validator is not None:
        validator(message, binary_length)
    # Validate metadata and expected IQ size before allocating the binary body.
    return message, _receive_exact(connection, binary_length, deadline)


def encode_frame(message, payload=b""):
    try:
        encoded = json.dumps(message, ensure_ascii=True, allow_nan=False,
                             separators=(",", ":")).encode("utf-8")
    except (ValueError, TypeError, RecursionError):
        raise ProtocolError("invalid response") from None
    _require(len(encoded) <= MAX_JSON and len(payload) <= MAX_BINARY,
             "frame length exceeded")
    return HEADER.pack(MAGIC, len(encoded), len(payload)) + encoded + payload


def send_frame(connection, message, payload=b"", timeout=5.0):
    connection.settimeout(timeout)
    connection.sendall(encode_frame(message, payload))


def window_coefficients(length, window):
    phase = 2 * np.pi * np.arange(length, dtype=np.float64) / (length - 1)
    coefficients = (
        (1.0,),
        (0.5, -0.5),
        (0.54, -0.46),
        (0.35875, -0.48829, 0.14128, -0.01168),
        (0.21557895, -0.41663158, 0.277263158, -0.083578947, 0.006947368),
    )[window]
    result = np.full(length, coefficients[0], dtype=np.float64)
    for harmonic, coefficient in enumerate(coefficients[1:], 1):
        result += coefficient * np.cos(harmonic * phase)
    return result


class ReferenceFft:
    def __init__(self, config):
        self.config = dict(config)
        self.window = window_coefficients(config["length"], config["window"])
        self.window_sum = float(self.window.sum())
        self.enbw = config["length"] * float(np.dot(self.window, self.window)) / self.window_sum**2
        self.last_emitted = None

    def compute(self, history, rate, now):
        length = self.config["length"]
        if len(history) < length:
            return None
        if self.last_emitted is not None and now - self.last_emitted < self.config["refreshMs"] / 1000:
            return None
        transform = np.fft.fftshift(np.fft.fft(history[-length:] * self.window))
        amplitude = np.abs(transform) / self.window_sum
        levels = (20 * np.log10(np.maximum(amplitude, 1e-10))).astype("<f4")
        _require(np.isfinite(levels).all(), "non-finite FFT output")
        self.last_emitted = now
        return {"id": self.config["id"], "length": length,
                "binWidthHz": rate / length, "rbwHz": rate / length * self.enbw}, levels.tobytes()


class NumpyProcessor:
    numerator = denominator = 1
    input_reserve = 0

    def process(self, samples):
        return samples.copy()

    def close(self):
        pass


def _load_gnu_block(plugin, sample_rate):
    from gnuradio import blocks, gr

    if plugin is None:
        block = blocks.multiply_const_cc(1.0)
    else:
        spec = importlib.util.spec_from_file_location("obrii_local_dsp_plugin", plugin)
        _require(spec is not None and spec.loader is not None, "plugin load failed")
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        block = module.make_block(sample_rate)
    numerator = denominator = 1
    reserve = None
    if type(block) is tuple:
        _require(len(block) in (3, 4), "invalid local rate contract")
        if len(block) == 4:
            reserve = block[3]
        block, numerator, denominator = block[:3]
        _integer(numerator, 1, MAX_RATE_FACTOR)
        _integer(denominator, 1, MAX_RATE_FACTOR)
        _require(math.gcd(numerator, denominator) == 1 and numerator <= 2 * denominator,
                 "unsupported local rate ratio")
    _number(sample_rate * numerator / denominator, 1000, 2000000)
    for signature in (block.input_signature(), block.output_signature()):
        _require(signature.min_streams() == 1 and signature.max_streams() == 1
                 and signature.sizeof_stream_item(0) == gr.sizeof_gr_complex,
                 "plugin must have one complex input and output")
    if hasattr(block, "relative_rate"):
        _require(math.isclose(block.relative_rate(), numerator / denominator, rel_tol=1e-12),
                 "plugin rate declaration mismatch")
    if numerator != denominator:
        if reserve is None:
            _require(hasattr(block, "history"), "hierarchy requires explicit input reserve")
            history = block.history()
            # GNU 3.10 installs rational FIR taps lazily on its first work call;
            # history() alone still reports 1 before the scheduler runs.
            if all(hasattr(block, name) for name in ("taps", "interpolation", "decimation")):
                history = max(history, (len(block.taps()) + block.interpolation() - 1) // block.interpolation())
            reserve = ((history - 1 + denominator - 1) // denominator + 1) * denominator
        _integer(reserve, denominator, MAX_INPUT_RESERVE)
        _require(reserve % denominator == 0, "input reserve must be whole rate quanta")
        if hasattr(block, "set_output_multiple"):
            multiple = math.lcm(numerator, block.output_multiple())
            _require(multiple <= 4096, "unsupported plugin output quantum")
            block.set_output_multiple(multiple)
    else:
        _require(reserve in (None, 0), "same-rate reserve unsupported")
        reserve = 0
    return gr, block, numerator, denominator, reserve


class _StreamingGraph:
    """Single bounded transaction in flight; source never pads or terminates it."""

    def __init__(self, plugin, sample_rate, output):
        gr, block, self.numerator, self.denominator, self.input_reserve = _load_gnu_block(plugin, sample_rate)
        # A Python block's gateway does not own its Python wrapper's lifetime.
        self.block = block
        self.condition = threading.Condition()
        self.closed = False
        self.error = False
        self.samples = np.empty(0, dtype=np.complex64)
        self.emitted = 0
        self.expected = 0
        self.total_input = self.total_output = self.delivered = 0
        self.capacity = MAX_OUTPUT_SAMPLES + 2 * MAX_INPUT_RESERVE + 2 * MAX_RATE_FACTOR
        self.pending = np.empty(self.capacity, dtype=np.complex64)
        self.output = output
        state = self

        class Source(gr.sync_block):
            def __init__(self):
                super().__init__(name="Obrii bounded source", in_sig=None, out_sig=[np.complex64])

            def work(self, _inputs, outputs):
                with state.condition:
                    while state.emitted == state.expected and not state.closed and not state.error:
                        state.condition.wait(0.05)
                    if state.closed or state.error:
                        return -1
                    count = min(len(outputs[0]), state.expected - state.emitted)
                    outputs[0][:count] = state.samples[state.emitted:state.emitted + count]
                    state.emitted += count
                    state.condition.notify_all()
                    return count

        class Sink(gr.sync_block):
            def __init__(self):
                super().__init__(name="Obrii bounded sink", in_sig=[np.complex64], out_sig=None)

            def work(self, inputs, _outputs):
                values = inputs[0]
                with state.condition:
                    end = state.total_output + len(values)
                    limit = (state.total_input * state.numerator + state.denominator - 1) // state.denominator
                    if (end > limit or end - state.delivered > state.capacity
                            or not np.isfinite(values).all()):
                        state.error = True
                        state.condition.notify_all()
                        return -1
                    start = state.total_output % state.capacity
                    first = min(len(values), state.capacity - start)
                    state.pending[start:start + first] = values[:first]
                    state.pending[:len(values) - first] = values[first:]
                    state.total_output = end
                    state.condition.notify_all()
                    return len(values)

        self.source, self.sink = Source(), Sink()
        self.graph = gr.top_block("Obrii software-only DSP")
        for node in (self.source, block):
            node.set_max_output_buffer(8192)
        self.graph.connect(self.source, block, self.sink)
        self.graph.start(max_noutput_items=4096)

    def process(self, samples, timeout):
        deadline = time.monotonic() + timeout
        with self.condition:
            _require(not self.error and not self.closed, "GNU streaming failure")
            self.samples = samples
            self.expected = len(samples)
            self.emitted = 0
            self.total_input += len(samples)
            # Reserve filter history plus forecast lookahead; GNU general blocks
            # may wait for real startup history. Never fabricate padding samples.
            if self.numerator == self.denominator:
                target = self.total_input
            else:
                target = max(0, (self.total_input - self.input_reserve) // self.denominator) * self.numerator
            count = target - self.delivered
            _require(0 <= count <= MAX_OUTPUT_SAMPLES, "GNU output limit exceeded")
            self.condition.notify_all()
            while (self.total_output < target or self.emitted != self.expected) and not self.error:
                remaining = deadline - time.monotonic()
                _require(remaining > 0, "GNU output timeout")
                self.condition.wait(remaining)
            _require(not self.error and self.emitted == self.expected, "GNU output count mismatch")
            start = self.delivered % self.capacity
            first = min(count, self.capacity - start)
            self.output[:first] = self.pending[start:start + first]
            self.output[first:count] = self.pending[:count - first]
            self.delivered = target
            return count

    def close(self):
        with self.condition:
            self.closed = True
            self.condition.notify_all()
        self.graph.stop()
        self.graph.wait()


def _gnu_worker(pipe, input_storage, output_storage, plugin, sample_rate, timeout):
    graph = None
    try:
        source = np.frombuffer(input_storage, dtype=np.complex64)
        sink = np.frombuffer(output_storage, dtype=np.complex64)
        graph = _StreamingGraph(plugin, sample_rate, sink)
        pipe.send_bytes(b"R" + struct.pack("<III", graph.numerator, graph.denominator, graph.input_reserve))
        while True:
            command = pipe.recv_bytes(5)
            if command == b"Q":
                break
            _require(len(command) == 5 and command[:1] == b"W", "invalid local command")
            count = struct.unpack("<I", command[1:])[0]
            _require(1 <= count <= MAX_SAMPLES, "invalid local sample count")
            produced = graph.process(source[:count], timeout)
            pipe.send_bytes(b"D" + struct.pack("<I", produced))
    except Exception:
        # Plugin exception text is deliberately not exposed to a client or log.
        try:
            pipe.send_bytes(b"E")
        except (OSError, EOFError):
            pass
    finally:
        if graph is not None:
            try:
                graph.close()
            except Exception:
                pass
        pipe.close()


class GnuProcessor:
    """A persistent scheduler isolated so a stalled plugin can be terminated."""

    def __init__(self, sample_rate, plugin=None, timeout=5.0, cancel=None):
        self.timeout = timeout
        self.cancel = cancel
        self.process_handle = None
        context = multiprocessing.get_context("spawn")
        self.input_storage = context.RawArray("f", MAX_SAMPLES * 2)
        self.output_storage = context.RawArray("f", MAX_OUTPUT_SAMPLES * 2)
        self.input = np.frombuffer(self.input_storage, dtype=np.complex64)
        self.output = np.frombuffer(self.output_storage, dtype=np.complex64)
        self.pipe, child_pipe = context.Pipe(duplex=True)
        self.process_handle = context.Process(target=_gnu_worker, args=(
            child_pipe, self.input_storage, self.output_storage, plugin, sample_rate, timeout),
            name="Obrii GNU stream", daemon=True)
        try:
            self.process_handle.start()
            child_pipe.close()
            ready = self._reply(b"R", "GNU worker startup failed or timed out", 13)
            self.numerator, self.denominator, self.input_reserve = struct.unpack("<III", ready[1:])
        except Exception:
            child_pipe.close()
            self.close()
            raise ProtocolError("GNU worker startup failed or timed out") from None

    def _reply(self, expected, reason, length):
        try:
            deadline = time.monotonic() + self.timeout
            while True:
                _require(self.cancel is None or not self.cancel.is_set(), "GNU processing cancelled")
                remaining = deadline - time.monotonic()
                _require(remaining > 0, reason)
                if self.pipe.poll(min(0.05, remaining)):
                    break
            reply = self.pipe.recv_bytes(length)
            _require(len(reply) == length and reply[:1] == expected, reason)
            return reply
        except (OSError, EOFError):
            raise ProtocolError(reason) from None

    def process(self, samples):
        _require(1 <= len(samples) <= MAX_SAMPLES, "invalid local sample count")
        try:
            self.input[:len(samples)] = samples
            # Fixed shared arrays and tiny control messages avoid blocking on a
            # full IQ pipe when a plugin holds the worker's interpreter hostage.
            self.pipe.send_bytes(b"W" + struct.pack("<I", len(samples)))
            reply = self._reply(b"D", "GNU processing failed or timed out", 5)
            count = struct.unpack("<I", reply[1:])[0]
            _require(count <= MAX_OUTPUT_SAMPLES, "GNU output limit exceeded")
            result = self.output[:count].copy()
            _require(np.isfinite(result).all(), "non-finite GNU output")
            return result
        except (OSError, EOFError, ProtocolError):
            self.close()
            raise ProtocolError("GNU processing failed or timed out") from None

    def close(self):
        process = self.process_handle
        if process is None:
            return
        self.process_handle = None
        if process.pid is not None:
            try:
                self.pipe.send_bytes(b"Q")
            except (OSError, EOFError):
                pass
            process.join(0.25)
            if process.is_alive():
                process.terminate()
                process.join(0.5)
            if process.is_alive():
                process.kill()
                process.join(0.5)
            if process.is_alive():
                raise ProtocolError("GNU worker shutdown failed")
            process.close()
        self.pipe.close()


@dataclass(frozen=True)
class ProcessorFactory:
    engine: str = "gnuradio"
    plugin: str = None
    timeout: float = 5.0

    def __call__(self, sample_rate, cancel=None):
        if self.engine == "numpy":
            return NumpyProcessor()
        return GnuProcessor(sample_rate, self.plugin, self.timeout, cancel)


def _configuration(message):
    return (_uint64(message["generation"]), _uint64(message["epoch"]),
            message["sampleRate"], message["centerHz"],
            tuple((f["id"], f["length"], f["window"], f["refreshMs"]) for f in message["ffts"]))


class _Stream:
    def __init__(self, message, factory, cancel=None):
        self.configuration = _configuration(message)
        self.next_sample = _uint64(message["firstSample"])
        self.ffts = [ReferenceFft(config) for config in message["ffts"]]
        capacity = max((f.config["length"] for f in self.ffts), default=0)
        self.history = np.empty(capacity, dtype=np.complex64)
        self.used = 0
        self.processor = (factory(message["sampleRate"], cancel=cancel) if isinstance(factory, ProcessorFactory)
                          else factory(message["sampleRate"]))
        self.numerator = self.processor.numerator
        self.denominator = self.processor.denominator
        self.input_reserve = self.processor.input_reserve
        self.rate = message["sampleRate"] * self.numerator / self.denominator
        self.origin = self.next_sample
        self.output_next = 0 if self.numerator != self.denominator else self.next_sample
        self.output_gap = True

    def append(self, values):
        capacity = len(self.history)
        count = min(len(values), capacity)
        retained = min(self.used, capacity - count)
        if count:
            self.history[:retained] = self.history[self.used - retained:self.used]
            self.history[retained:retained + count] = values[-count:]
        self.used = retained + count


class DspSession:
    def __init__(self, factory=None, clock=time.monotonic, cancel=None):
        self.factory = factory if factory is not None else ProcessorFactory()
        self.clock = clock
        self.cancel = cancel
        self.streams = {}

    def process(self, message, payload):
        started = time.monotonic()
        validate_work(message, len(payload))
        samples = np.frombuffer(payload, dtype="<c8")
        _require(np.isfinite(samples).all(), "non-finite IQ input")
        key = message["stream"]
        stream = self.streams.get(key)
        first = _uint64(message["firstSample"])
        if stream is not None and (message["gap"] or first != stream.next_sample
                                   or stream.configuration != _configuration(message)):
            stream.processor.close()
            del self.streams[key]
            stream = None
        if stream is None:
            _require(len(self.streams) < MAX_STREAMS, "stream limit exceeded")
            stream = _Stream(message, self.factory, self.cancel)
            self.streams[key] = stream
        try:
            result = stream.processor.process(samples)
            total = first + len(samples) - stream.origin
            target = (max(0, (total - stream.input_reserve) // stream.denominator) * stream.numerator
                      if stream.numerator != stream.denominator else first + len(samples))
            _require(isinstance(result, np.ndarray) and result.ndim == 1
                     and result.dtype == np.complex64 and len(result) <= MAX_OUTPUT_SAMPLES
                     and len(result) == target - stream.output_next
                     and target <= UINT64_MAX and np.isfinite(result).all(),
                     "invalid processor output")
        except Exception:
            del self.streams[key]
            stream.processor.close()
            raise
        stream.next_sample = first + len(samples)
        stream.append(result)
        iq = result.astype("<c8", copy=False).tobytes() if message["returnIq"] else b""
        chunks = [iq]
        offset = len(iq)
        spectra = []
        now = self.clock()
        for fft in stream.ffts:
            frame = fft.compute(stream.history[:stream.used], stream.rate, now) if len(result) else None
            if frame is not None:
                metadata, levels = frame
                metadata["offsetBytes"] = offset
                spectra.append(metadata)
                chunks.append(levels)
                offset += len(levels)
        reply = {name: message[name] for name in (
            "stream", "generation", "epoch", "firstSample", "sampleRate", "centerHz", "sampleCount")}
        reply.update(type="result", iqBytes=len(iq), spectra=spectra,
                     processingMs=(time.monotonic() - started) * 1000)
        if stream.numerator != stream.denominator:
            reply["output"] = {"sampleRate": stream.rate, "firstSample": str(stream.output_next),
                               "sampleCount": len(result), "originFirstSample": str(stream.origin),
                               "rateNumerator": stream.numerator, "rateDenominator": stream.denominator,
                               "inputReserve": stream.input_reserve,
                               "gap": stream.output_gap}
        stream.output_next += len(result)
        if len(result):
            stream.output_gap = False
        return reply, b"".join(chunks)

    def close(self):
        failed = False
        try:
            for stream in self.streams.values():
                try:
                    stream.processor.close()
                except Exception:
                    failed = True
        finally:
            self.streams.clear()
        if failed:
            raise ProtocolError("stream cleanup failed")


class BridgeServer:
    def __init__(self, bind, port, token, factory=None, timeout=5.0):
        address = ipaddress.ip_address(bind)
        _integer(port, 0, 65535)
        _number(timeout, 0.05, 30)
        self.token = validate_token(token)
        self.factory = factory if factory is not None else ProcessorFactory()
        self.timeout = timeout
        self.stopped = threading.Event()
        self.client_thread = None
        self.client = None
        family = socket.AF_INET6 if address.version == 6 else socket.AF_INET
        self.listener = socket.socket(family, socket.SOCK_STREAM)
        try:
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            if family == socket.AF_INET6:
                self.listener.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            self.listener.bind((str(address), port))
            self.listener.listen(1)
            self.listener.settimeout(0.1)
            self.address = self.listener.getsockname()
        except Exception:
            self.listener.close()
            raise

    def _serve_client(self, connection):
        authenticated = False
        session = DspSession(self.factory, cancel=self.stopped)
        try:
            receive_frame(connection, self.timeout,
                          lambda message, size: validate_hello(message, size, self.token))
            send_frame(connection, {"type": "hello", "version": 1, "rationalRateVersion": 1}, timeout=self.timeout)
            authenticated = True
            while not self.stopped.is_set():
                message, payload = receive_frame(connection, self.timeout, validate_work)
                result, output = session.process(message, payload)
                send_frame(connection, result, output, self.timeout)
        except (TimeoutError, OSError, EOFError):
            pass
        except Exception as error:
            if authenticated:
                reason = str(error) if isinstance(error, ProtocolError) else "processing failed"
                try:
                    send_frame(connection, {"type": "error", "error": reason},
                               timeout=min(self.timeout, 0.25))
                except (OSError, ProtocolError):
                    pass
        finally:
            connection.close()
            try:
                session.close()
            except ProtocolError:
                self.stopped.set()

    def serve_forever(self):
        try:
            while not self.stopped.is_set():
                try:
                    connection, _peer = self.listener.accept()
                except TimeoutError:
                    continue
                except OSError:
                    if self.stopped.is_set():
                        break
                    raise
                # A peer can reconnect immediately after closing its socket,
                # before GNU teardown finishes. This is one bounded admission
                # wait, not another authenticated session or a request queue.
                if self.client_thread is not None and self.client_thread.is_alive():
                    self.client_thread.join(0.5)
                # Also reserve the single slot during authentication and cleanup.
                if self.stopped.is_set() or (self.client_thread is not None
                                             and self.client_thread.is_alive()):
                    connection.close()
                    continue
                self.client = connection
                self.client_thread = threading.Thread(target=self._serve_client,
                                                      args=(connection,), daemon=True)
                self.client_thread.start()
        finally:
            self.close()

    def close(self):
        self.stopped.set()
        self.listener.close()
        if self.client is not None:
            try:
                self.client.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if self.client_thread is not None and self.client_thread is not threading.current_thread():
            self.client_thread.join()


def check_runtime(factory):
    """No sockets, files or hardware sources: exercise two contiguous blocks."""
    processor = factory(96000)
    try:
        for first in (0, 1024):
            tone = (0.25 * np.exp(2j * np.pi * np.arange(first, first + 1024) / 32)).astype(np.complex64)
            result = processor.process(tone)
            _require(result.ndim == 1 and len(result) <= MAX_OUTPUT_SAMPLES
                     and np.isfinite(result).all(), "runtime check failed")
            if factory.plugin is None:
                _require(np.array_equal(result, tone), "identity check failed")
        fft = ReferenceFft({"id": "check", "length": 1024, "window": 0, "refreshMs": 20})
        _, raw = fft.compute(tone, 96000, 0.0)
        levels = np.frombuffer(raw, dtype="<f4")
        _require(int(levels.argmax()) == 544 and abs(float(levels.max()) + 12.0412) < 0.001,
                 "FFT check failed")
    finally:
        processor.close()


class _SafeParser(argparse.ArgumentParser):
    def error(self, _message):
        # A mistakenly supplied --token argument must not be echoed by argparse.
        self.print_usage(sys.stderr)
        self.exit(2, "invalid command-line options; use --help\n")


def main(argv=None):
    parser = _SafeParser(description=__doc__)
    parser.add_argument("--bind", default=os.environ.get("OBRII_DSP_BIND", "127.0.0.1"),
                        help="numeric local bind address (default: loopback)")
    parser.add_argument("--port", type=int, default=os.environ.get("OBRII_DSP_PORT"),
                        help="explicit TCP port, or OBRII_DSP_PORT")
    parser.add_argument("--engine", choices=("gnuradio", "numpy"), default="gnuradio")
    parser.add_argument("--plugin", help="trusted local Python file providing make_block(sample_rate)")
    parser.add_argument("--timeout", type=float, default=5.0, help="frame deadline, 0.05..30 seconds")
    parser.add_argument("--processing-timeout", type=float, default=5.0,
                        help="GNU startup/transaction deadline, 0.05..30 seconds")
    parser.add_argument("--check", action="store_true", help="software-only runtime check; no listener")
    args = parser.parse_args(argv)
    server = None
    try:
        _number(args.timeout, 0.05, 30)
        _number(args.processing_timeout, 0.05, 30)
        _require(not args.plugin or args.engine == "gnuradio", "plugin requires GNU Radio")
        plugin = str(Path(args.plugin).resolve(strict=True)) if args.plugin else None
        factory = ProcessorFactory(args.engine, plugin, args.processing_timeout)
        if args.check:
            check_runtime(factory)
            print("Obrii DSP bridge check: PASS (" + args.engine + ")", flush=True)
            return 0
        _integer(args.port, 1, 65535)
        server = BridgeServer(args.bind, args.port, os.environ.get(TOKEN_ENV, ""), factory, args.timeout)
        print("Obrii DSP bridge listening on " + str(server.address[0]) + ":" + str(server.address[1]),
              flush=True)
        server.serve_forever()
        return 0
    except KeyboardInterrupt:
        return 0
    except Exception:
        print("Obrii DSP bridge failed; check local configuration, token environment and runtime.",
              file=sys.stderr)
        return 1
    finally:
        if server is not None:
            server.close()


if __name__ == "__main__":
    multiprocessing.freeze_support()
    sys.exit(main())
