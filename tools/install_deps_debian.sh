#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This helper is for Debian/Raspberry Pi OS systems with apt-get." >&2
    exit 1
fi

sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    qtbase5-dev \
    libqt5serialport5-dev \
    qtmultimedia5-dev \
    libqt5multimedia5-plugins \
    libfftw3-dev \
    libusb-1.0-0-dev \
    librtlsdr-dev \
    rtl-sdr \
    mesa-common-dev \
    libgl1-mesa-dev

echo
echo "Base build dependencies installed."
echo "You still need Linux builds of libfobos and libfobos_sdr."
echo "Place them under /usr/local or pass FOBOS_STANDARD_ROOT/FOBOS_AGILE_ROOT to tools/build_linux.sh."
