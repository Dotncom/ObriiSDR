#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REV=03787da53c65a8eb291eec571b6412ec470aeb17
SOURCE="$ROOT/build/deps/PocketSDR"
DEST="$ROOT/build/linux-release/pocketsdr"
JOBS=${JOBS:-2}
if ! pkg-config --exists libusb-1.0; then
    echo 'Install build-essential git pkg-config libusb-1.0-0-dev first.' >&2
    exit 1
fi
mkdir -p "$SOURCE" "$DEST"
if [ ! -d "$SOURCE/.git" ]; then
    git -C "$SOURCE" init
    git -C "$SOURCE" remote add origin https://github.com/tomojitakasu/PocketSDR.git
fi
git -C "$SOURCE" sparse-checkout init --cone
git -C "$SOURCE" sparse-checkout set src app/pocket_trk lib/build lib/RTKLIB lib/pocketfft
git -C "$SOURCE" fetch --depth 1 --filter=blob:none origin "$REV"
git -C "$SOURCE" checkout --detach FETCH_HEAD
mkdir -p "$SOURCE/lib/linux"
# IQ is supplied by Obrii. No Soapy hardware access is needed.
make -C "$SOURCE/lib/build" -j "$JOBS" USE_SOAPY=0 USE_AVX2=0 USE_FFTW=0
make -C "$SOURCE/lib/build" install USE_SOAPY=0 USE_AVX2=0 USE_FFTW=0
make -C "$SOURCE/app/pocket_trk" -j "$JOBS" pocket_trk USE_SOAPY=0 USE_FFTW=0
cp "$SOURCE/app/pocket_trk/pocket_trk" "$DEST/"
cp "$SOURCE/LICENSE.txt" "$DEST/LICENSE-PocketSDR.txt"
cp "$SOURCE/lib/RTKLIB/readme.txt" "$DEST/LICENSE-RTKLIB.txt"
cp "$SOURCE/lib/pocketfft/LICENSE.md" "$DEST/LICENSE-PocketFFT.md"
"$DEST/pocket_trk" -v
