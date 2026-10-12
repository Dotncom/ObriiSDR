#!/usr/bin/env python3
"""Software-only OFDM loopback using upstream modulation/training symbols.
No receiver or transmitter is opened. Training data: gr-ieee802-11 ad0598e, GPLv3.
"""
import ast
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import time
import zlib
import wave
sys.path.insert(0, str(Path(__file__).resolve().parent))
import wifi_phy_decoder as adapter
import numpy as np
import pmt
import yaml
from gnuradio import gr, blocks, digital, fft, filter
import ieee802_11 as wifi


def synthesize(frame, encoding, upstream):
    graph = gr.top_block("Software-only Wi-Fi fixture")
    source = wifi.mapper(encoding, False)
    formatter = wifi.signal_field()
    header = digital.packet_headergenerator_bb(formatter.formatter(), "packet_len")
    header_symbols = digital.chunks_to_symbols_bc([-1, 1], 1)
    symbols = wifi.chunks_to_symbols()
    mux = blocks.tagged_stream_mux(gr.sizeof_gr_complex, "packet_len", 1)
    definition = yaml.safe_load(Path(upstream).read_text(encoding="utf-8"))
    parameters = next(b["parameters"] for b in definition["blocks"] if b["id"] == "digital_ofdm_carrier_allocator_cvc")
    occupied = (list(range(-26, -21)) + list(range(-20, -7)) + list(range(-6, 0)) + list(range(1, 7)) + list(range(8, 21)) + list(range(22, 27)),)
    carriers = digital.ofdm_carrier_allocator_cvc(64, occupied, ((-21, -7, 7, 21),), ast.literal_eval(parameters["pilot_symbols"]), ast.literal_eval(parameters["sync_words"]), "packet_len", True)
    transform = fft.fft_vcc(64, False, [1 / np.sqrt(52)] * 64, True, 1)
    prefix = digital.ofdm_cyclic_prefixer(64, 80, 2, "packet_len")
    sink = blocks.vector_sink_c()
    graph.connect(source, header, header_symbols, (mux, 0))
    graph.connect(source, symbols, (mux, 1))
    graph.connect(mux, carriers, transform, prefix, sink)
    payload = frame + struct.pack("<I", zlib.crc32(frame))
    graph.start()
    source.to_basic_block()._post(pmt.intern("in"), pmt.cons(pmt.make_dict(), pmt.init_u8vector(len(payload), list(payload))))
    deadline = time.monotonic() + 5
    previous = 0
    while time.monotonic() < deadline:
        time.sleep(0.05)
        count = len(sink.data())
        if count and count == previous:
            break
        previous = count
    graph.stop(); graph.wait()
    data = np.asarray(sink.data(), dtype=np.complex64)
    if not len(data):
        raise AssertionError("Upstream transmitter did not produce a software fixture")
    signal = np.concatenate((np.zeros(4096, np.complex64), data * 0.1, np.zeros(16384, np.complex64)))
    # RF captures have a noise floor. Exact-zero padding can expose upstream moving-sum roundoff.
    random = np.random.default_rng(42)
    noise = (random.normal(0, 1e-4, len(signal)) + 1j * random.normal(0, 1e-4, len(signal))).astype(np.complex64)
    return signal + noise


def read_pcap(filename):
    raw = Path(filename).read_bytes()
    assert len(raw) >= 24 and struct.unpack_from("<I", raw)[0] == 0xA1B2C3D4
    result = []
    pos = 24
    while pos < len(raw):
        length = struct.unpack_from("<IIII", raw, pos)[2]
        packet = raw[pos + 16:pos + 16 + length]
        radiotap_length = struct.unpack_from("<H", packet, 2)[0]
        result.append(packet[radiotap_length:])
        pos += 16 + length
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--upstream", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    root = Path(args.output); root.mkdir(parents=True, exist_ok=True)
    frame = bytes.fromhex("80000000ffffffffffff0200000000010200000000010000000000000000000064000100") + b"\x00\x0aObrii test\x01\x01\x82\x03\x01\x01"
    encodings = [wifi.BPSK_1_2, wifi.BPSK_3_4, wifi.QPSK_1_2, wifi.QPSK_3_4, wifi.QAM16_1_2, wifi.QAM16_3_4, wifi.QAM64_2_3, wifi.QAM64_3_4]
    for index, encoding in enumerate(encodings):
        iq = synthesize(frame, encoding, args.upstream)
        filename = root / (str(index) + ".cf32")
        iq.tofile(filename)
        output = str(root / (str(index) + ".pcap"))
        adapter.decode(argparse.Namespace(input=str(filename), output=output, format="cf32", sample_rate=20e6, center=2412e6, channel=2412e6, threshold=0.56))
        assert read_pcap(output) == [frame], "PHY/FEC/CRC round trip failed for encoding " + str(index)
        stats = json.loads(Path(output + ".json").read_text(encoding="utf-8"))["phy"]
        assert stats["short"] >= 1 and stats["long"] >= 1 and stats["signal"] >= 1
        assert stats["crc_valid"] == 1 and stats["rates_mbps"].get(str(adapter.PhyStats.rates_mbps[index])) == 1
    print("All 8 legacy OFDM rates: exact CRC-validated packet round trip PASS")
    for fmt, scale, dtype in [("cs16", 32768, "<i2"), ("cs8", 128, "i1")]:
        filename = root / ("format." + fmt)
        interleaved = np.empty(len(iq)*2, np.float32)
        interleaved[0::2] = iq.real; interleaved[1::2] = iq.imag
        np.rint(interleaved * scale).astype(dtype).tofile(filename)
        output = str(root / (fmt + ".pcap"))
        adapter.decode(argparse.Namespace(input=str(filename), output=output, format=fmt, sample_rate=20e6, center=2412e6, channel=2412e6, threshold=0.56))
        assert read_pcap(output) == [frame], "Integer IQ format " + fmt
    filename = root / "format.wav"
    with wave.open(str(filename), "wb") as wav:
        wav.setnchannels(2); wav.setsampwidth(2); wav.setframerate(20_000_000)
        wav.writeframes(np.rint(interleaved * 32768).astype("<i2").tobytes())
    output = str(root / "wav.pcap")
    adapter.decode(argparse.Namespace(input=str(filename), output=output, format="cf32", sample_rate=50e6, center=2412e6, channel=2412e6, threshold=0.56))
    assert read_pcap(output) == [frame], "WAV actual rate and format override manual raw settings"
    graph = gr.top_block("Software-only rate/center fixture")
    source = blocks.vector_source_c(iq.tolist(), False)
    resample = filter.rational_resampler_ccc(5, 2, fractional_bw=0.45)
    shift = blocks.rotator_cc(2*np.pi*12e6/50e6)
    sink = blocks.vector_sink_c()
    graph.connect(source,resample,shift,sink); graph.run()
    filename = root / "wide.cf32"; np.asarray(sink.data(),np.complex64).tofile(filename)
    output = str(root / "wide.pcap")
    adapter.decode(argparse.Namespace(input=str(filename), output=output, format="cf32", sample_rate=50e6, center=2400e6, channel=2412e6, threshold=0.56))
    assert read_pcap(output) == [frame], "Wideband NCO and 50 -> 20 MS/s conversion"
    filename = root / "zero.cf32"; np.zeros(25000,np.complex64).tofile(filename)
    output = str(root / "zero.pcap")
    adapter.decode(argparse.Namespace(input=str(filename), output=output, format="cf32", sample_rate=20e6, center=2412e6, channel=2412e6, threshold=0.56))
    assert read_pcap(output) == [], "Pure zero IQ cannot produce valid packets"
    assert json.loads(Path(output + ".json").read_text(encoding="utf-8"))["phy"]["crc_valid"] == 0
    interleaved = np.empty(len(iq) * 2, np.float32)
    interleaved[0::2], interleaved[1::2] = iq.real, iq.imag
    live_output = str(root / "live.pcap")
    live = subprocess.run([sys.executable, "-I", "-B", str(Path(adapter.__file__).resolve()),
        "--live", "--output", live_output, "--sample-rate", "20000000",
        "--center", "2412000000", "--channel", "2412000000"],
        input=np.rint(interleaved * 128).clip(-128, 127).astype(np.int8).tobytes(),
        capture_output=True, timeout=30)
    assert live.returncode == 0, live.stderr.decode(errors="replace")
    assert read_pcap(live_output) == [frame], "Live stdin IQ to PCAP"
    events = [json.loads(line) for line in live.stdout.splitlines() if line.startswith(b"{")]
    assert any("row" in event for event in events), "Live decoded packets must be visible before PCAP inspection"
    assert any(event.get("phy", {}).get("crc_valid") == 1 for event in events), "Live PHY status must be emitted"
    print("CS16/CS8/WAV, wideband shift/resample and no-signal tests: PASS")
    print("Live CS8 stdin to CRC-validated packet and PCAP: PASS")


if __name__ == "__main__":
    main()
