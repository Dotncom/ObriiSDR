#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX="$ROOT/build/wifi-phy-native"
JOBS=${JOBS:-2}
if ! pkg-config --exists gnuradio-runtime; then
    echo 'Install: sudo apt-get install gnuradio gnuradio-dev python3-numpy python3-yaml python3-pybind11 pybind11-dev libboost-all-dev libspdlog-dev libfmt-dev liblog4cpp5-dev git cmake' >&2
    exit 1
fi
build_module() {
    local name=$1 revision=$2
    local source="$ROOT/build/deps/$name-$revision"
    if [ ! -d "$source/.git" ]; then
        mkdir -p "$source"
        git -C "$source" init
        git -C "$source" remote add origin "https://github.com/bastibl/$name.git"
        git -C "$source" fetch --depth 1 origin "$revision"
        git -C "$source" checkout --detach FETCH_HEAD
    fi
    cmake -S "$source" -B "$source/build-obrii" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" -DGR_PYTHON_DIR="$PREFIX/python" -DPYTHON_EXECUTABLE="$(command -v python3)"
    cmake --build "$source/build-obrii" -j "$JOBS"
    cmake --install "$source/build-obrii"
}
build_module gr-foo 9e0e002f5daf18e91ae428478785b74b2061a9f5
build_module gr-ieee802-11 ad0598e4a874f4b8e1f391a1e0323e80df2b34ff
mkdir -p "$ROOT/build/linux-release/wireless"
cp "$ROOT/tools/wifi_phy_decoder.py" "$ROOT/build/linux-release/wireless/"
cp "$ROOT/tools/wifi_phy_linux_launcher.sh" "$ROOT/build/linux-release/wireless/python-phy"
chmod +x "$ROOT/build/linux-release/wireless/python-phy"
"$ROOT/build/linux-release/wireless/python-phy" -I -B "$ROOT/build/linux-release/wireless/wifi_phy_decoder.py" --check
echo "Native PHY prefix: $PREFIX"
echo 'The application will use wireless/python-phy. No RF device was opened.'
