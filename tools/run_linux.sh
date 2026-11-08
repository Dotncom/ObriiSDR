#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${WORKSPACE}/build/linux-release}"
APP="${APP:-${BUILD_DIR}/ObriiSDR}"
LOCAL_FOBOS_PREFIX="${LOCAL_FOBOS_PREFIX:-${WORKSPACE}/third_party/fobos-linux}"

if [ ! -x "${APP}" ]; then
    echo "Obrii SDR binary not found or not executable: ${APP}" >&2
    echo "Build first with ./tools/build_linux.sh" >&2
    exit 2
fi

RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ -d "${RUNTIME_DIR}" ]; then
    chmod 700 "${RUNTIME_DIR}" 2>/dev/null || true
fi

export LD_LIBRARY_PATH="${LOCAL_FOBOS_PREFIX}/lib:${FOBOS_STANDARD_ROOT:-/usr/local}/lib:${FOBOS_AGILE_ROOT:-/usr/local}/lib:${LD_LIBRARY_PATH:-}"
exec "${APP}" "$@"
