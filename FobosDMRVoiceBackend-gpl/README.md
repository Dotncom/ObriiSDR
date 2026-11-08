# Fobos DMR Voice Backend GPL

This project builds optional DMR voice backend modules for FobosAPP.

The base FobosAPP application can decode DMR metadata and extract AMBE frames
without linking to any specific AMBE implementation. Voice synthesis and future
voice encoding are provided by small loadable backend libraries with a stable C
ABI.

## Current Backend

- `fobos_dmr_voice_mbelib`: GPL-2.0-or-later decode backend using mbelib-neo.
- `fobos_dmr_voice_opendmr`: GPL-2.0 encode/decode backend using OpenDMR,
  mbelib-derived decode code, and OP25 MBEEncoder-derived encode code.

The mbelib backend accepts the DMR over-the-air AMBE frame layout used by the
current FobosAPP DMR burst extractor. The OpenDMR backend exposes
canonical/DVSI AMBE3600x2450 frame encode/decode; those 9-byte frames still need
DMR burst placement/interleaving before RF transmission.

## Build

The backend dependencies are optional FobosAPP Git submodules:

- mbelib-neo: https://github.com/arancormonk/mbelib-neo
- OpenDMR/softdmr: https://github.com/hicaoc/softdmr

From a Git checkout, fetch them with:

```bash
git submodule update --init --recursive third_party/mbelib-neo third_party/softdmr
```

If you are working from a source archive, clone them into the same paths before
building:

```bash
git clone https://github.com/arancormonk/mbelib-neo.git third_party/mbelib-neo
git clone https://github.com/hicaoc/softdmr.git third_party/softdmr
```

From the FobosAPP repository root:

```powershell
cmake -S FobosDMRVoiceBackend-gpl -B build\fobos-dmr-voice-backend-gpl -G "Visual Studio 17 2022" -A x64
cmake --build build\fobos-dmr-voice-backend-gpl --config Release
```

The runtime DLL is:

```text
build\fobos-dmr-voice-backend-gpl\Release\fobos_dmr_voice_mbelib.dll
build\fobos-dmr-voice-backend-gpl\Release\fobos_dmr_voice_opendmr.dll
```

For FobosAPP runtime testing, place backend DLLs under:

```text
release\bin\dmr_voice_backends\
```

## Licensing And Patent Notice

This backend project is intended to be distributed under GPL-2.0-or-later
because it links to mbelib-neo. See `../third_party/mbelib-neo/LICENSE` and
`../third_party/mbelib-neo/README.md`.

The OpenDMR backend links to `../third_party/softdmr`, which is GPL-2.0 and
integrates code from mbelib-derived decoder sources and OP25 MBEEncoder-derived
encoder sources. Distribute the OpenDMR backend under GPL-2.0-compatible terms
and include the upstream license and attribution files.

AMBE/AMBE+2 technologies may be patent-encumbered in some jurisdictions. This
backend is optional and is provided for research, interoperability, and amateur
experimentation. Users and redistributors should verify local legal and patent
requirements before compiling, distributing, or using AMBE-capable binaries.

## ABI Policy

The public interface is plain C and lives in:

```text
include/fobos_dmr_voice_backend.h
```

FobosAPP should only depend on this ABI, not on mbelib-neo, OpenDMR, OP25, or
DVSI-specific headers.
