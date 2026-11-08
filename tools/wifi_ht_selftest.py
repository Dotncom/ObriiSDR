#!/usr/bin/env python3
"""Software-only HT20 SISO regression using the separately pinned sdr2wifi generator."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wifi_phy_decoder as adapter


def packet_count(path):
    raw = Path(path).read_bytes()
    assert len(raw) >= 24 and struct.unpack_from('<I', raw)[0] == 0xA1B2C3D4
    count = 0
    offset = 24
    while offset < len(raw):
        assert offset + 16 <= len(raw)
        length = struct.unpack_from('<I', raw, offset + 8)[0]
        offset += 16 + length
        assert offset <= len(raw)
        count += 1
    return count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generator', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    root = Path(args.output)
    root.mkdir(parents=True, exist_ok=True)
    for mcs in (0, 1):
        iq = root / f'ht-mcs{mcs}.cf32'
        pcap = root / f'ht-mcs{mcs}.pcap'
        subprocess.run([sys.executable, '-I', '-B', args.generator,
                        '--mode', 'ht', '--mcs', str(mcs), '--reps', '4',
                        '--gap', '2000', '--out', str(iq)], check=True)
        adapter.decode(argparse.Namespace(input=str(iq), output=str(pcap), format='cf32',
            sample_rate=20e6, center=2412e6, channel=2412e6, threshold=0.56,
            extended_phy=True))
        stats = json.loads(Path(str(pcap) + '.json').read_text(encoding='utf-8'))
        assert packet_count(pcap) >= 1
        assert stats['phy']['extended_encodings'].get(f'HT MCS {mcs}', 0) >= 1
    print('HT20 SISO MCS0/1: CRC-validated PCAP round trip PASS')


if __name__ == '__main__':
    main()
