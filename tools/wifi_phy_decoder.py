#!/usr/bin/env python3
"""802.11 20 MHz OFDM adapter. GNU Radio/gr-ieee802-11 do PHY/FEC/CRC.

Receiver graph follows bastibl/gr-ieee802-11 ad0598e wifi_phy_hier.grc.
Copyright Bastian Bloessl and gr-ieee802-11 contributors; GPL-3.0-or-later.
Obrii adapter: bounded file or live stdin input, rate conversion, PCAP and provenance.
No hardware source, transmitter, network upload or automatic key handling.
"""
import argparse
from fractions import Fraction
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
import threading
import wave
from collections import Counter

_print_lock = threading.Lock()


def report(event):
    with _print_lock:
        print(json.dumps(event, separators=(",", ":")), flush=True)

_dll_handles = []
if os.name == "nt":
    for directory in (Path(sys.executable).parent / "Library" / "bin", Path(sys.executable).parent / "DLLs"):
        if directory.is_dir():
            _dll_handles.append(os.add_dll_directory(str(directory)))
else:
    native_modules = Path(__file__).resolve().parents[2] / "wifi-phy-native" / "python"
    if native_modules.is_dir():
        sys.path.insert(0, str(native_modules))

import numpy as np
import pmt
from gnuradio import gr, blocks, fft, filter
import ieee802_11 as wifi


class IqSource(gr.sync_block):
    def __init__(self, filename, fmt, sample_rate):
        super().__init__(name="Obrii IQ file", in_sig=None, out_sig=[np.complex64])
        self.count = 0
        self.error = ""
        self.last_report = 0.0
        self.wave = None
        self.dtype, self.scale = {"cf32": ("<f4", 1.0), "cs16": ("<i2", 1 / 32768), "cs8": ("i1", 1 / 128)}[fmt]
        self.sample_rate = sample_rate
        if Path(filename).suffix.lower() == ".wav":
            self.wave = wave.open(filename, "rb")
            if self.wave.getnchannels() != 2 or self.wave.getsampwidth() != 2 or self.wave.getcomptype() != "NONE":
                raise ValueError("WAV must contain stereo PCM16 I+jQ, not audio or float WAV")
            self.dtype, self.scale = "<i2", 1 / 32768
            self.sample_rate = self.wave.getframerate()
            self.file = None
        else:
            self.file = open(filename, "rb")
            if os.fstat(self.file.fileno()).st_size % (2 * np.dtype(self.dtype).itemsize):
                raise ValueError("IQ file ends with an incomplete sample")

    def work(self, inputs, outputs):
        count = min(len(outputs[0]), 65536)
        raw = self.wave.readframes(count) if self.wave else self.file.read(count * 2 * np.dtype(self.dtype).itemsize)
        if not raw:
            return -1
        values = np.frombuffer(raw, dtype=self.dtype).astype(np.float32, copy=False)
        if not np.isfinite(values).all():
            self.error = "Non-finite IQ samples in recording; decoding stopped"
            return -1
        count = len(values) // 2
        outputs[0][:count].real = values[0::2] * self.scale
        outputs[0][:count].imag = values[1::2] * self.scale
        self.count += count
        now = time.monotonic()
        if now - self.last_report >= 1:
            report({"iq_samples": self.count, "input_rate_hz": self.sample_rate})
            self.last_report = now
        return count

    def close(self):
        if self.wave:
            self.wave.close()
        if self.file:
            self.file.close()


class IqStdinSource(gr.sync_block):
    def __init__(self, sample_rate):
        super().__init__(name="Obrii live IQ", in_sig=None, out_sig=[np.complex64])
        if os.name == "nt":
            import msvcrt
            msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
        self.sample_rate = sample_rate
        self.pending = b""
        self.count = 0
        self.error = ""
        self.last_report = 0.0

    def work(self, inputs, outputs):
        wanted = min(len(outputs[0]), 65536) * 2
        raw = self.pending + os.read(sys.stdin.fileno(), max(1, wanted - len(self.pending)))
        if len(raw) < 2:
            if not raw:
                return -1
            more = os.read(sys.stdin.fileno(), 1)
            if not more:
                self.error = "Live IQ stream ended on a half-sample"
                return -1
            raw += more
        even = len(raw) & ~1
        self.pending = raw[even:]
        values = np.frombuffer(raw[:even], dtype=np.int8).astype(np.float32)
        count = len(values) // 2
        outputs[0][:count].real = values[0::2] / 128.0
        outputs[0][:count].imag = values[1::2] / 128.0
        self.count += count
        now = time.monotonic()
        if now - self.last_report >= 1:
            report({"iq_samples": self.count, "input_rate_hz": self.sample_rate})
            self.last_report = now
        return count

    def close(self):
        pass


def wifi_frame_type(frame):
    if len(frame) < 2:
        return None
    fc = int.from_bytes(frame[:2], "little")
    kind, subtype = (fc >> 2) & 3, (fc >> 4) & 15
    if kind == 0:
        return "Management"
    if kind == 1:
        return {8: "Block Ack Request", 9: "Block Ack", 10: "PS-Poll",
                11: "RTS", 12: "CTS", 13: "ACK", 14: "CF-End",
                15: "CF-End + CF-Ack"}.get(subtype, f"Control {subtype}")
    if kind == 2:
        return "Data"
    return "Reserved"


def live_wifi_row(frame, frequency, number, timestamp):
    if len(frame) < 10:
        return None
    fc = int.from_bytes(frame[:2], "little")
    kind, subtype = (fc >> 2) & 3, (fc >> 4) & 15
    to_ds, from_ds = (fc >> 8) & 1, (fc >> 9) & 1
    def mac(offset):
        return ":".join(f"{v:02x}" for v in frame[offset:offset + 6])
    if kind == 1:
        destination = mac(4)
        source = mac(10) if len(frame) >= 16 and subtype in (8, 9, 10, 11, 14, 15) else ""
        bssid = ""
        sequence = ""
    elif len(frame) >= 24:
        destination, source, bssid = mac(4), mac(10), mac(16)
        sequence = str(int.from_bytes(frame[22:24], "little") >> 4)
    else:
        return None
    if kind == 2 and to_ds != from_ds:
        if to_ds:
            bssid, destination = destination, mac(16)
        else:
            bssid, source = source, mac(16)
    ssid = ""
    if kind == 0 and subtype in (4, 5, 8):
        offset = 24 if subtype == 4 else 36
        while offset + 2 <= len(frame):
            tag, length = frame[offset], frame[offset + 1]
            offset += 2
            if offset + length > len(frame):
                break
            if tag == 0:
                ssid = frame[offset:offset + length].hex()
                break
            offset += length
    type_name = wifi_frame_type(frame)
    return [str(number), str(timestamp), "802.11", source, destination,
            str(len(frame) + 12), bssid, ssid, str(round(frequency / 1e6)), "",
            type_name, str((fc >> 11) & 1), str((fc >> 14) & 1), "", sequence,
            type_name, "radiotap:wlan"]


class PacketSink(gr.basic_block):
    def __init__(self, filename, frequency, live=False):
        super().__init__(name="Obrii PCAP", in_sig=None, out_sig=None)
        self.file = open(filename, "xb")
        self.file.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 127))
        self.frequency = frequency
        self.count = 0
        self.limited = False
        self.live = live
        self.last_report = 0.0
        self.row_window = 0
        self.rows_this_second = 0
        self.ui_drops = 0
        self.extended_encodings = Counter()
        self.frame_types = Counter()
        self.frame_classes = Counter()
        self.packet_stats_lock = threading.Lock()
        self.message_port_register_in(pmt.intern("in"))
        self.set_msg_handler(pmt.intern("in"), self.receive)

    def receive(self, message):
        if self.limited or not pmt.is_pair(message):
            return
        metadata = pmt.car(message)
        encoding = None
        if pmt.is_dict(metadata):
            crc_ok = pmt.dict_ref(metadata, pmt.intern("crc_ok"), pmt.PMT_T)
            if pmt.is_bool(crc_ok) and not pmt.to_bool(crc_ok):
                return
            encoded = pmt.dict_ref(metadata, pmt.intern("encoding"), pmt.PMT_NIL)
            if pmt.is_uint64(encoded):
                encoding = pmt.to_uint64(encoded)
        payload = pmt.cdr(message)
        if pmt.is_blob(payload):
            frame = bytes(pmt.to_python(payload))
        elif pmt.is_u8vector(payload):
            frame = bytes(pmt.u8vector_elements(payload))
        else:
            return
        if not frame or len(frame) > 65000:
            return
        if self.file.tell() + len(frame) + 28 > 100 * 1024 * 1024 or self.count >= 500000:
            self.limited = True
            if self.live:
                report({"packets": self.count, "limit_reached": True, "ui_drops": self.ui_drops})
            return
        flags = 0xC0 if self.frequency < 3e9 else 0x140
        radiotap = struct.pack("<BBHIHH", 0, 0, 12, 1 << 3, round(self.frequency / 1e6), flags)
        packet = radiotap + frame
        # Upstream does not expose the original RF sample counter. Never fabricate RF timestamps.
        now = time.time()
        seconds = int(now)
        self.file.write(struct.pack("<IIII", seconds, int((now - seconds) * 1e6), len(packet), len(packet)))
        self.file.write(packet)
        self.file.flush()
        self.count += 1
        with self.packet_stats_lock:
            frame_type = wifi_frame_type(frame)
            if frame_type:
                self.frame_types[frame_type] += 1
                self.frame_classes[("Management", "Control", "Data", "Reserved")[(frame[0] >> 2) & 3]] += 1
            if encoding is not None and 8 <= encoding <= 49:
                name = f"HT MCS {encoding - 8}" if encoding <= 39 else f"VHT MCS {encoding - 40}"
                self.extended_encodings[name] += 1
        if self.live:
            second = int(now)
            if second != self.row_window:
                self.row_window = second
                self.rows_this_second = 0
            if self.rows_this_second < 200:
                row = live_wifi_row(frame, self.frequency, self.count, now)
                if row:
                    report({"row": row})
                    self.rows_this_second += 1
            else:
                self.ui_drops += 1
            if now - self.last_report >= 1:
                report({"packets": self.count, "ui_drops": self.ui_drops})
                self.last_report = now

    def close(self):
        self.file.close()

    def packet_snapshot(self):
        with self.packet_stats_lock:
            return {"extended_encodings": dict(self.extended_encodings),
                    "frame_types": dict(self.frame_types),
                    "frame_classes": dict(self.frame_classes)}


class PhyStats:
    rates_mbps = (6, 9, 12, 18, 24, 36, 48, 54)

    def __init__(self):
        self.lock = threading.Lock()
        self.short = 0
        self.long = 0
        self.signal = 0
        self.rates = Counter()
        self.snr_sum = 0.0
        self.snr_count = 0

    def observe(self, stage, tags):
        if not tags:
            return
        with self.lock:
            if stage == "short":
                self.short += len(tags)
            elif stage == "long":
                self.long += len(tags)
            else:
                self.signal += len(tags)

    def observe_header(self, rates, snrs):
        if not rates and not snrs:
            return
        with self.lock:
            for tag in rates:
                encoding = pmt.to_uint64(tag.value)
                if 0 <= encoding < len(self.rates_mbps):
                    self.rates[str(self.rates_mbps[encoding])] += 1
            for tag in snrs:
                value = pmt.to_double(tag.value)
                if math.isfinite(value):
                    self.snr_sum += value
                    self.snr_count += 1

    def snapshot(self, valid_packets):
        with self.lock:
            return {"short": self.short, "long": self.long,
                    "signal": self.signal, "crc_valid": valid_packets,
                    "rates_mbps": dict(self.rates),
                    "avg_snr_db": round(self.snr_sum / self.snr_count, 1) if self.snr_count else None}


class TagCounter(gr.sync_block):
    def __init__(self, stats, stage, signature):
        super().__init__(name="Obrii " + stage + " counter", in_sig=[signature], out_sig=None)
        self.stats = stats
        self.stage = stage
        self.key = pmt.intern("frame bytes" if stage == "signal" else "wifi_start")

    def work(self, inputs, outputs):
        count = len(inputs[0])
        tags = self.get_tags_in_window(0, 0, count, self.key)
        self.stats.observe(self.stage, tags)
        if self.stage == "signal":
            rates = self.get_tags_in_window(0, 0, count, pmt.intern("encoding"))
            snrs = self.get_tags_in_window(0, 0, count, pmt.intern("snr"))
            self.stats.observe_header(rates, snrs)
        return count


class Receiver(gr.hier_block2):
    def __init__(self, frequency, stats, threshold=0.56, debug=False, extended_phy=False):
        super().__init__("802.11 OFDM RX", gr.io_signature(1, 1, gr.sizeof_gr_complex), gr.io_signature(0, 0, 0))
        self.message_port_register_hier_out("out")
        delay16 = blocks.delay(gr.sizeof_gr_complex, 16)
        conjugate = blocks.conjugate_cc()
        multiply = blocks.multiply_cc()
        correlation = blocks.moving_average_cc(48, 1, 4000)
        magnitude = blocks.complex_to_mag()
        power = blocks.complex_to_mag_squared()
        average = blocks.moving_average_ff(64, 1, 4000)
        divide = blocks.divide_ff()
        short = wifi.sync_short(threshold, 2, False, debug)
        delay320 = blocks.delay(gr.sizeof_gr_complex, 320)
        long = wifi.sync_long(320, False, debug)
        vectors = blocks.stream_to_vector(gr.sizeof_gr_complex, 64)
        transform = fft.fft_vcc(64, True, [1] * 64, True, 1)
        equalizer = wifi.frame_equalizer(wifi.LS, frequency, 20e6, False, debug)
        decoder = wifi.decode_mac(False, debug)
        self.short_counter = TagCounter(stats, "short", np.complex64)
        self.long_counter = TagCounter(stats, "long", np.complex64)
        self.signal_counter = TagCounter(stats, "signal", (np.uint8, 48))
        self.connect(self, delay16, conjugate, (multiply, 1))
        self.connect(self, (multiply, 0))
        self.connect(multiply, correlation, magnitude, (divide, 0))
        self.connect(correlation, (short, 1))
        self.connect(self, power, average, (divide, 1))
        self.connect(divide, (short, 2))
        self.connect(delay16, (short, 0))
        self.connect(short, (long, 0))
        self.connect(short, self.short_counter)
        self.connect(short, delay320, (long, 1))
        self.connect(long, self.long_counter)
        self.connect(long, vectors, transform, equalizer, decoder)
        if extended_phy:
            self.connect((equalizer, 1), (decoder, 1))
        self.connect(equalizer, self.signal_counter)
        self.msg_connect(decoder, "out", self, "out")
        if extended_phy:
            self.msg_connect(equalizer, "pdu", self, "out")


def decode(args):
    if not all(math.isfinite(v) for v in (args.sample_rate, args.center, args.channel, args.threshold)):
        raise ValueError("Non-finite radio parameters")
    live = getattr(args, "live", False)
    extended_phy = getattr(args, "extended_phy", False)
    source = IqStdinSource(args.sample_rate) if live else IqSource(args.input, args.format, args.sample_rate)
    sink = None
    graph = None
    try:
        if Path(args.output + ".json").exists():
            raise ValueError("Output metadata already exists; select a new output name")
        rate = source.sample_rate
        if not 20e6 <= rate <= 1e9 or abs(args.center - args.channel) + 10e6 > rate / 2:
            raise ValueError("A complete 20 MHz channel must fit in the recording; upsampling cannot recover missing RF bandwidth")
        if not 1e6 <= args.channel <= 60e9 or not 0.1 <= args.threshold <= 0.95:
            raise ValueError("Channel frequency or synchronization threshold out of range")
        graph = gr.top_block("Obrii Wi-Fi decoder", catch_exceptions=True)
        sink = PacketSink(args.output, args.channel, live)
        rotate = blocks.rotator_cc(-2 * math.pi * (args.channel - args.center) / rate)
        graph.connect(source, rotate)
        last = rotate
        ratio = Fraction(20_000_000, round(rate))
        if ratio.denominator > 4096 or ratio.numerator > 4096:
            raise ValueError("Rate conversion ratio is too complex; record a conventional sample rate")
        if ratio != 1:
            resample = filter.rational_resampler_ccc(ratio.numerator, ratio.denominator, fractional_bw=0.45)
            graph.connect(last, resample)
            last = resample
        stats = PhyStats()
        receiver = Receiver(args.channel, stats, args.threshold, getattr(args, "debug", False), extended_phy)
        graph.connect(last, receiver)
        graph.msg_connect(receiver, "out", sink, "in")
        finished = threading.Event()
        cancelled = threading.Event()
        def read_control():
            data = b""
            while len(data) < 32:
                part = os.read(sys.stdin.fileno(), 32 - len(data))
                if not part:
                    return
                data += part
                if b"\n" in data:
                    if data.split(b"\n", 1)[0].strip() == b"stop":
                        cancelled.set()
                    return
        def wait_graph():
            try:
                graph.wait()
            finally:
                finished.set()
        graph.start()
        started_at = time.monotonic()
        if not live:
            threading.Thread(target=read_control, daemon=True).start()
        waiter = threading.Thread(target=wait_graph, daemon=True)
        waiter.start()
        last_stats_report = time.monotonic()
        while not finished.wait(0.1):
            if (sink.limited and not live) or cancelled.is_set():
                graph.stop()
            now = time.monotonic()
            if now - last_stats_report >= 1:
                report({"phy": {**stats.snapshot(sink.count), **sink.packet_snapshot()}})
                last_stats_report = now
        waiter.join()
        if source.error:
            raise ValueError(source.error)
        elapsed = max(time.monotonic() - started_at, 1e-6)
        report({"iq_samples": source.count, "input_rate_hz": rate})
        phy = {**stats.snapshot(sink.count), **sink.packet_snapshot()}
        report({"phy": phy})
        report({"packets": sink.count, "limit_reached": sink.limited, "ui_drops": sink.ui_drops,
                "timestamp_origin": "decoder-wall-clock-not-rf"})
        Path(args.output + ".json").write_text(json.dumps({"schema": "obrii-wifi-phy-v1", "input": "live:stdin" if live else str(Path(args.input).resolve()), "input_sample_rate_hz": rate,
            "record_center_hz": args.center, "channel_hz": args.channel, "phy_sample_rate_hz": 20e6, "packets": sink.count,
            "timestamp_origin": "decoder-wall-clock-not-rf", "rssi_dbm": None, "crc_policy": "upstream-valid-only", "fcs_removed": True, "cancelled": cancelled.is_set(),
            "live": live, "ui_drops": sink.ui_drops, "input_samples": source.count,
            "phy_mode": "ht20-experimental" if extended_phy else "legacy",
            "decoder_elapsed_s": round(elapsed, 3),
            "decoder_input_rate_hz": round(source.count / elapsed),
            "phy": phy,
            "upstream": "josephnef/gr-ieee802-11/b3dbd2d476407db92f77056df23c166e4274c507" if extended_phy else "bastibl/gr-ieee802-11/ad0598e",
            "gnu_radio": gr.version()}, indent=2), encoding="utf-8")
    finally:
        if graph:
            graph.stop()
            graph.wait()
        if sink:
            sink.close()
        source.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--extended-phy", action="store_true", help="enable experimental HT/VHT PDU output from the extended GNU Radio module")
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--input")
    parser.add_argument("--output")
    parser.add_argument("--format", choices=("cf32", "cs16", "cs8"), default="cf32")
    parser.add_argument("--sample-rate", type=float, default=20e6)
    parser.add_argument("--center", type=float, default=2412e6)
    parser.add_argument("--channel", type=float, default=2412e6)
    parser.add_argument("--threshold", type=float, default=0.56)
    args = parser.parse_args()
    if args.check:
        print("GNU Radio " + gr.version() + "; gr-ieee802-11 OFDM RX available", flush=True)
        return 0
    if (not args.live and not args.input) or not args.output:
        parser.error("--output and either --input or --live are required")
    decode(args)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
