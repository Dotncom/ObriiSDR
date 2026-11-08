"""Verify against compiled, unmodified RNode firmware TX/RX and save boundary vectors.

Run with build/reticulum-venv/Scripts/python.exe tools/generate_rnode_vectors.py
Add --download to fetch the pinned, hash-checked GPL firmware into ignored build/.
The separate reference executable is never linked into Obrii or distributed by
the module's CMake install. No firmware implementation is copied into src/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request
from unittest.mock import patch

COMMIT = "56775c51b60a6190242e324c210997dcc7637e53"
HASHES = {
    "RNode_Firmware.ino": "e29f4c22086ac8d9204555433c460add27638866b9c4e41817e06a40911fae3e",
    "Utilities.h": "d3957f85ee5d8ea5980c5fd64ccdb6a7a6ddbd73bc7d7b61ba9fd5d7d34113e4",
    "Framing.h": "2cd09e17dfc3b8029fd97347a004639743db4f887f33399e0a2a1d5e1da381a9",
    "Config.h": "600d00a1fd7f7828bfe857a826c0a5b4d10eb9727b713dc45ad4e6bdea012227",
    "LICENSE": "3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986",
}

PRELUDE = r'''
// Generated test harness for upstream RNode firmware (GPL-3.0-or-later).
// Upstream copyright and GPL license are retained alongside this executable.
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "../Framing.h"
#define MCU_VARIANT 1
#define MCU_ESP32 2
#define MCU_NRF52 3
#define ISR_VECT
#define ERROR_MODEM_TIMEOUT 6
bool radio_online=true, promisc=false, implicit=false;
uint8_t seq=SEQ_UNSET;
uint16_t read_len=0, host_write_len=0;
int last_rssi=0, last_snr_raw=0;
int random_byte=0;
uint8_t pbuf[508]={}, tbuf[508]={};
std::vector<uint8_t> serial_data;
struct Radio {
    std::vector<uint8_t> input, output;
    std::vector<std::vector<uint8_t>> frames;
    size_t offset=0;
    void beginPacket(int=0) { output.clear(); }
    void write(uint8_t b) { output.push_back(b); }
    bool endPacket() { frames.push_back(output); return true; }
    uint8_t read() { return input.at(offset++); }
    int packetRssi() { return -70; }
    int packetSnrRaw() { return 24; }
} radio;
Radio *LoRa=&radio;
long random(long) { return random_byte; }
void serial_write(uint8_t b) { serial_data.push_back(b); }
void kiss_indicate_stat_rssi() {}
void kiss_indicate_stat_snr() {}
void kiss_indicate_error(int) {}
void led_tx_on() {}
void led_indicate_error(int) {}
void add_airtime(uint16_t) {}
void hard_reset() { throw std::runtime_error("Unexpected firmware reset"); }
'''

MAIN = r'''
std::vector<uint8_t> unhex(const std::string &s) {
    if(s.size()%2) throw std::runtime_error("Odd hex");
    std::vector<uint8_t> out;
    for(size_t i=0; i<s.size(); i+=2) out.push_back(uint8_t(std::stoul(s.substr(i,2),nullptr,16)));
    return out;
}
void printhex(const std::vector<uint8_t> &bytes) {
    constexpr char hex[]="0123456789abcdef";
    for(auto b:bytes) std::cout<<hex[b>>4]<<hex[b&15];
}
int main() {
    try {
        std::string line;
        while(std::getline(std::cin,line)) {
            std::istringstream stream(line);
            std::string op, data; stream>>op;
            if(op=="tx") {
                stream>>random_byte>>data;
                auto payload=unhex(data);
                if(payload.empty() || payload.size()>508) throw std::runtime_error("TX length");
                std::copy(payload.begin(),payload.end(),tbuf);
                radio.frames.clear(); transmit(uint16_t(payload.size()));
                for(size_t i=0; i<radio.frames.size(); ++i) {
                    if(i) std::cout<<'|'; printhex(radio.frames[i]);
                }
            } else if(op=="rx") {
                stream>>data; seq=SEQ_UNSET; read_len=host_write_len=0; serial_data.clear();
                std::istringstream parts(data);
                while(std::getline(parts,data,'|')) {
                    radio.input=unhex(data); radio.offset=0;
                    if(radio.input.empty() || radio.input.size()>255) throw std::runtime_error("RX length");
                    receive_callback(int(radio.input.size()));
                }
                printhex(serial_data);
            } else throw std::runtime_error("Unknown operation");
            std::cout<<std::endl;
        }
    } catch(const std::exception &e) { std::cerr<<e.what()<<std::endl; return 1; }
}
'''


def function(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 0
    for end in range(body, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1] + "\n"
    raise ValueError("Could not extract complete firmware function")


def run(args):
    result = subprocess.run(args, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=120, creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    if result.returncode:
        raise RuntimeError(result.stdout[-4000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--download", action="store_true")
    args = parser.parse_args()
    workspace = Path(__file__).resolve().parents[1]
    cache = workspace / "build/rnode-reference"
    cache.mkdir(parents=True, exist_ok=True)
    for name, digest in HASHES.items():
        path = cache / name
        if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            if not args.download:
                raise RuntimeError(f"Missing or changed {name}; use --download for the pinned reference")
            with urllib.request.urlopen(f"https://raw.githubusercontent.com/markqvist/RNode_Firmware/{COMMIT}/{name}", timeout=30) as response:
                contents = response.read(200000)
            if hashlib.sha256(contents).hexdigest() != digest:
                raise RuntimeError(f"Reference hash mismatch: {name}")
            path.write_bytes(contents)
    ino = (cache / "RNode_Firmware.ino").read_text(encoding="utf-8")
    utilities = (cache / "Utilities.h").read_text(encoding="utf-8")
    config = (cache / "Config.h").read_text(encoding="utf-8")
    constants = "".join(re.search(r"^\s*#define " + name + r"\s+\d+.*$", config, re.M)[0] + "\n"
                        for name in ("MTU", "SINGLE_MTU", "HEADER_L"))
    functions = "\n".join(function(source, signature) for source, signature in (
        (utilities, "inline bool isSplitPacket("), (utilities, "inline uint8_t packetSequence("),
        (ino, "inline void kiss_write_packet("), (ino, "inline void getPacketData("),
        (ino, "void ISR_VECT receive_callback("), (ino, "void transmit(uint16_t size)")))
    generated = cache / "runtime"
    generated.mkdir(exist_ok=True)
    (generated / "reference.cpp").write_text(
        ino[:ino.index("#include <Arduino.h>")] + PRELUDE + constants + functions + MAIN, encoding="utf-8")
    (generated / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.16)\nproject(RNodeReference LANGUAGES CXX)\n'
        'add_executable(RNodeFramingReference reference.cpp)\n'
        'target_compile_features(RNodeFramingReference PRIVATE cxx_std_17)\n', encoding="utf-8")
    cmake = shutil.which("cmake")
    if not cmake:
        raise RuntimeError("CMake not found")
    build = generated / "build"
    run([cmake, "-S", str(generated), "-B", str(build)])
    run([cmake, "--build", str(build), "--config", "Release"])
    executable = build / ("Release/RNodeFramingReference.exe" if os.name == "nt" else "RNodeFramingReference")
    sys.path.insert(0, str(workspace / "src/reticulum"))
    from obrii_reticulum.rnode import Fragmenter, Reassembler
    from RNS.Interfaces.RNodeInterface import KISS
    vectors = []
    lengths = {1, 2, 19, 253, 254, 255, 256, 499, 500, 507, 508}
    with subprocess.Popen([str(executable)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            text=True, encoding="ascii", creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0) as process:
        def oracle(line):
            process.stdin.write(line + "\n")
            process.stdin.flush()
            answer = process.stdout.readline(4096)
            if not answer.endswith("\n"):
                raise RuntimeError("Reference firmware process failed")
            return answer.strip()
        for size in range(1, 509):
            payload = bytes((i * 17 + 3) % 256 for i in range(size))
            for sequence in range(16):
                frames = [bytes.fromhex(p) for p in oracle(f"tx {sequence << 4} {payload.hex()}").split("|")]
                with patch("obrii_reticulum.rnode.secrets.randbits", return_value=sequence):
                    own = Fragmenter().split(payload)
                expected = frames[:-1] if size == 508 else frames
                if own != expected:
                    raise AssertionError(f"TX mismatch at length {size}, sequence {sequence}")
                kiss = oracle("rx " + "|".join(p.hex() for p in own))
                if kiss != (b"\xc0\x00" + KISS.escape(payload) + b"\xc0").hex():
                    raise AssertionError(f"Firmware did not receive our TX at {size}/{sequence}")
                receiver = Reassembler()
                output = [receiver.receive(p) for p in frames]
                if [p for p in output if p is not None] != [payload] or receiver.pending:
                    raise AssertionError(f"Could not receive firmware TX at {size}/{sequence}")
                if size in lengths and sequence in (0, 5, 10, 15):
                    vectors.append({"length": size, "sequence": sequence,
                                    "frames": [p.hex() for p in frames], "kiss": kiss})
        process.stdin.close()
        if process.wait(timeout=5):
            raise RuntimeError("Reference exited unsuccessfully")
    destination = workspace / "src/reticulum/tests/vectors/rnode_firmware_1_86.json"
    destination.parent.mkdir(exist_ok=True)
    destination.write_text(json.dumps({"upstream": "https://github.com/markqvist/RNode_Firmware", "commit": COMMIT,
        "firmware_version": "1.86", "source_sha256": HASHES, "verified_cases": 508 * 16,
        "pattern": "payload[i] = (17*i+3) mod 256", "vectors": vectors}, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {508 * 16} cases against compiled upstream TX/RX; saved {len(vectors)} boundary vectors")


if __name__ == "__main__":
    main()
