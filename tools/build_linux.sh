#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${BUILD_DIR:-${WORKSPACE}/build/linux-release}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
FFTW_ROOT="${FFTW_ROOT:-/usr}"
LOCAL_FOBOS_PREFIX="${LOCAL_FOBOS_PREFIX:-${WORKSPACE}/third_party/fobos-linux}"

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"

choose_root() {
    local env_name="$1"
    local header="$2"
    local explicit="${!env_name:-}"

    if [ -n "${explicit}" ]; then
        printf '%s\n' "${explicit}"
    elif [ -e "${LOCAL_FOBOS_PREFIX}/include/${header}" ]; then
        printf '%s\n' "${LOCAL_FOBOS_PREFIX}"
    elif [ -e "/usr/local/include/${header}" ]; then
        printf '%s\n' "/usr/local"
    elif [ -e "/usr/include/${header}" ]; then
        printf '%s\n' "/usr"
    else
        printf '%s\n' "/usr/local"
    fi
}

FOBOS_STANDARD_ROOT="$(choose_root FOBOS_STANDARD_ROOT fobos.h)"
FOBOS_AGILE_ROOT="$(choose_root FOBOS_AGILE_ROOT fobos_sdr.h)"

cmake_options=()
if [ "${BUILD_LITE:-0}" = 1 ]; then
    cmake_options+=(
        -DFOBOS_ENABLE_MBELIB_NEO=OFF
        -DFOBOS_ENABLE_HF_PREDICTION=OFF
        -DFOBOS_ENABLE_VKFFT=OFF
    )
    BUILD_HF_HELPER=0
fi

missing=0
need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Missing command: $1" >&2
        missing=1
    fi
}

need_path() {
    if [ ! -e "$1" ]; then
        echo "Missing: $1" >&2
        missing=1
    fi
}

need_cmd cmake
need_cmd c++
need_path "${FOBOS_STANDARD_ROOT}/include/fobos.h"
need_path "${FOBOS_AGILE_ROOT}/include/fobos_sdr.h"

if [ "$missing" -ne 0 ]; then
    cat >&2 <<EOF

Install base packages with:
  sudo ./tools/install_deps_debian.sh

Then install or build Linux libfobos/libfobos_sdr. Expected default layout:
  ${LOCAL_FOBOS_PREFIX}/include/fobos.h
  ${LOCAL_FOBOS_PREFIX}/lib/libfobos.so
  ${LOCAL_FOBOS_PREFIX}/include/fobos_sdr.h
  ${LOCAL_FOBOS_PREFIX}/lib/libfobos_sdr.so

You can build that private copy with:
  ./tools/prepare_fobos_linux.sh

System-wide fallback layout:
  ${FOBOS_STANDARD_ROOT}/include/fobos.h
  ${FOBOS_STANDARD_ROOT}/lib/libfobos.so
  ${FOBOS_AGILE_ROOT}/include/fobos_sdr.h
  ${FOBOS_AGILE_ROOT}/lib/libfobos_sdr.so

If your libraries live elsewhere, run for example:
  FOBOS_STANDARD_ROOT=/path/to/standard FOBOS_AGILE_ROOT=/path/to/agile ./tools/build_linux.sh
EOF
    exit 2
fi

if [ "${BUILD_TARGET:-}" = ObriiCli ]; then
    # Full packages use the same project/cache/output as the GUI. The reduced
    # developer diagnostic archive retains a fallback entry point, not a fork.
    CMAKE_SOURCE="${WORKSPACE}"
    if [ ! -f "${CMAKE_SOURCE}/CMakeLists.txt" ]; then
        CMAKE_SOURCE="${WORKSPACE}/src/cli"
    fi
    cmake -S "${CMAKE_SOURCE}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
        -DFOBOS_STANDARD_ROOT="${FOBOS_STANDARD_ROOT}" \
        -DFOBOS_AGILE_ROOT="${FOBOS_AGILE_ROOT}" \
        -DFFTW_ROOT="${FFTW_ROOT}" \
        "${cmake_options[@]}"
    if [ "${BUILD_CLI_VIEW:-0}" = 1 ]; then
        cmake --build "${BUILD_DIR}" --target ObriiCli ObriiCliModule ObriiCliGnssModule ObriiCliPacketModule ObriiCliImageModule ObriiCliSpectrumModule ObriiCliM17Module ObriiCliTxModule ObriiCliView -j "${JOBS}"
        echo "Visual companion: ${BUILD_DIR}/ObriiCliView"
    else
        cmake --build "${BUILD_DIR}" --target ObriiCli ObriiCliModule ObriiCliGnssModule ObriiCliPacketModule ObriiCliImageModule ObriiCliSpectrumModule ObriiCliM17Module ObriiCliTxModule -j "${JOBS}"
    fi
    echo "Build complete: ${BUILD_DIR}/ObriiCli and on-demand modules"
    exit 0
fi

cmake -S "${WORKSPACE}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
    -DFOBOS_STANDARD_ROOT="${FOBOS_STANDARD_ROOT}" \
    -DFOBOS_AGILE_ROOT="${FOBOS_AGILE_ROOT}" \
    -DFFTW_ROOT="${FFTW_ROOT}" \
    "${cmake_options[@]}"

if [ -n "${BUILD_TARGET:-}" ]; then
    cmake --build "${BUILD_DIR}" --target "${BUILD_TARGET}" -j "${JOBS}"
    echo
    echo "Build complete: ${BUILD_DIR}/${BUILD_TARGET}"
    exit 0
fi

cmake --build "${BUILD_DIR}" -j "${JOBS}"

# The small opt-in HF engine is a separate process, never part of RX/audio.
if [ "${BUILD_HF_HELPER:-1}" = 1 ] && [ -f "${WORKSPACE}/third_party/itu_hf/P533/Src/P533/P533.c" ]; then
    HF_BUILD="${WORKSPACE}/build/itu-hf-native"
    cmake -S "${WORKSPACE}/tools/itu_hf" -B "${HF_BUILD}" \
        -DITU_HF_BUILD_QT=OFF -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    cmake --build "${HF_BUILD}" -j 2
    cmake --install "${HF_BUILD}" --prefix "${BUILD_DIR}"
    if [ -d "${WORKSPACE}/third_party/runtime/itu_hf/data" ]; then
        mkdir -p "${BUILD_DIR}/itu_hf/data"
        cp "${WORKSPACE}/third_party/runtime/itu_hf/data/"* "${BUILD_DIR}/itu_hf/data/"
    fi
fi

echo
echo "Build complete:"
echo "  ${BUILD_DIR}/ObriiSDR"
echo "  ${BUILD_DIR}/ObriiAnalyzer"
echo "  ${BUILD_DIR}/ObriiIqHub"
echo "  ${BUILD_DIR}/ObriiCli"
mkdir -p "${BUILD_DIR}/wireless"
cp "${WORKSPACE}/tools/wifi_phy_decoder.py" "${BUILD_DIR}/wireless/"
