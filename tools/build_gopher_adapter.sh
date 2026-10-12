#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
workspace="$(cd -- "${script_dir}/.." && pwd)"
source_root="${1:-${GOPHERTRUNK_ROOT:-}}"
if [[ -z "$source_root" || ! -f "$source_root/internal/radio/dmr/voice/superframe.go" ]]; then
    echo 'Usage: bash tools/build_gopher_adapter.sh /path/to/GopherTrunk [output-directory]' >&2
    exit 2
fi
source_root="$(cd -- "$source_root" && pwd)"
output="${2:-${BUILD_DIR:-${workspace}/build/linux-release}/gophertrunk}"
mkdir -p -- "$source_root/cmd/fobos-dmr-virtual" "$output"
output="$(cd -- "$output" && pwd)"
cp -- "$script_dir"/gophertrunk_adapter/*.go "$source_root/cmd/fobos-dmr-virtual/"
cd -- "$source_root"
go test ./cmd/fobos-dmr-virtual ./internal/radio/dmr/voice ./internal/crypto/rc4
go build -trimpath -o "$output/fobos-dmr-virtual" ./cmd/fobos-dmr-virtual
cp -- LICENSE "$output/LICENSE.txt"
cp -- THIRD_PARTY_LICENSES.md "$output/"
printf 'Built: %s/fobos-dmr-virtual\n' "$output"
