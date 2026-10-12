#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

PATCHED_SOURCE_ROOT="${PATCHED_SOURCE_ROOT:-${WORKSPACE}/third_party/patched}"
SOURCE_ROOT="${SOURCE_ROOT:-${PATCHED_SOURCE_ROOT}}"
UPSTREAM_SOURCE_ROOT="${UPSTREAM_SOURCE_ROOT:-${WORKSPACE}/third_party/src}"
INSTALL_PREFIX="${INSTALL_PREFIX:-${WORKSPACE}/third_party/fobos-linux}"
BUILD_ROOT="${BUILD_ROOT:-${WORKSPACE}/build/fobos-linux}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
ALLOW_UPSTREAM_FOBOS_CLONE="${ALLOW_UPSTREAM_FOBOS_CLONE:-0}"

LIBFOBOS_REPO="${LIBFOBOS_REPO:-https://github.com/rigexpert/libfobos.git}"
LIBFOBOS_AGILE_REPO="${LIBFOBOS_AGILE_REPO:-https://github.com/rigexpert/libfobos-sdr-agile.git}"

log() {
    echo "$@" >&2
}

resolve_source_dir() {
    local name="$1"
    local repo="$2"
    local dir="${SOURCE_ROOT}/${name}"

    if [ -d "${dir}/.git" ]; then
        log "Using existing ${dir}"
        git -C "${dir}" fetch --tags --quiet || true
        printf '%s\n' "${dir}"
        return
    fi

    if [ -d "${dir}" ]; then
        log "Using existing non-git source directory ${dir}"
        printf '%s\n' "${dir}"
        return
    fi

    if [ "${ALLOW_UPSTREAM_FOBOS_CLONE}" != "1" ]; then
        cat >&2 <<EOF
Missing Fobos source directory:
  ${dir}

This project expects the patched vendored Fobos sources, not a silent clone
of the official upstream libraries. Make sure the source package contains:
  third_party/patched/libfobos
  third_party/patched/libfobos-sdr-agile

For an explicit upstream experiment only, rerun with:
  ALLOW_UPSTREAM_FOBOS_CLONE=1 ./tools/prepare_fobos_linux.sh
EOF
        exit 2
    fi

    dir="${UPSTREAM_SOURCE_ROOT}/${name}"
    if [ -d "${dir}" ]; then
        log "Using existing upstream source directory ${dir}"
        printf '%s\n' "${dir}"
        return
    fi

    if ! command -v git >/dev/null 2>&1; then
        echo "Missing git and source directory does not exist: ${dir}" >&2
        exit 2
    fi

    mkdir -p "${UPSTREAM_SOURCE_ROOT}"
    log "WARNING: cloning upstream ${repo}; this may not include Obrii SDR patches."
    log "Cloning ${repo} -> ${dir}"
    git clone --depth 1 "${repo}" "${dir}"
    printf '%s\n' "${dir}"
}

build_and_install() {
    local name="$1"
    local source_dir="$2"
    local build_dir="${BUILD_ROOT}/${name}"

    echo
    echo "Building ${name}"
    cmake -S "${source_dir}" -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${INSTALL_PREFIX}" \
        -DFOBOS_INSTALL_UDEV_RULES=OFF
    cmake --build "${build_dir}" -j "${JOBS}"
    cmake --install "${build_dir}"
}

LIBFOBOS_SOURCE_DIR="$(resolve_source_dir libfobos "${LIBFOBOS_REPO}")"
LIBFOBOS_AGILE_SOURCE_DIR="$(resolve_source_dir libfobos-sdr-agile "${LIBFOBOS_AGILE_REPO}")"

build_and_install libfobos "${LIBFOBOS_SOURCE_DIR}"
build_and_install libfobos-sdr-agile "${LIBFOBOS_AGILE_SOURCE_DIR}"

echo
echo "Fobos libraries installed into:"
echo "  ${INSTALL_PREFIX}"
echo
echo "Now build Obrii SDR with:"
echo "  ./tools/build_linux.sh"
