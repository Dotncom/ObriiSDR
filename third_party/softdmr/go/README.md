# OpenDMR Go Bindings

Go wrapper for the OpenDMR library - a software implementation of the DMR AMBE+2 vocoder.

## Features

- ✅ **Static Linking**: Compiles OpenDMR directly into your Go binary
- ✅ **Complete API**: Full access to encoder, decoder, and utility functions
- ✅ **Idiomatic Go**: Proper error handling, resource management, and Go conventions
- ✅ **Memory Safe**: Automatic cleanup with finalizers and explicit Close() methods
- ✅ **Zero Dependencies**: Only requires the OpenDMR C++ library

## Installation

### Prerequisites

1. **C++ Compiler**: g++ or clang++
2. **CGO**: Must be enabled (`CGO_ENABLED=1`)
3. **OpenDMR Library**: Built static library

### Build OpenDMR Library

First, build the OpenDMR C++ library and prepare the Go package:

```bash
cd /path/to/OpenDMR
make                    # Build the C++ library
cd go
make prepare           # Copy header and library files to Go package
```

This creates `libopendmr.a` and copies it along with `opendmr.h` into the `go/` directory, making them available when the package is distributed via `go get`.

### Use in Your Go Project

```bash
go get github.com/hicaoc/OpenDMR/go
```

Or use it locally with a `replace` directive in your `go.mod`:

```go
require github.com/hicaoc/OpenDMR/go v0.0.0
replace github.com/hicaoc/OpenDMR/go => /path/to/OpenDMR/go
```

Then import in your code:

```go
import opendmr "github.com/hicaoc/OpenDMR/go"
```

## Quick Start

### Decoding DMR Audio

```go
package main

import (
    "log"
    "github.com/hicaoc/OpenDMR/go"
)

func main() {
    // Create decoder
    decoder, err := opendmr.NewDecoder()
    if err != nil {
        log.Fatal(err)
    }
    defer decoder.Close()

    // Prepare buffers
    ambeFrame := make([]byte, opendmr.AmbeFrameBytes)  // 9 bytes
    pcmSamples := make([]int16, opendmr.PcmSamples)    // 160 samples

    // Read AMBE frame from your DMR source
    // ... (fill ambeFrame with data)

    // Decode to PCM
    errs, err := decoder.Decode(ambeFrame, pcmSamples)
    if err != nil {
        log.Printf("Decode failed: %v", err)
        return
    }
    
    log.Printf("Decoded with %d corrected errors", errs)
    // Now pcmSamples contains 160 samples of 16-bit PCM audio @ 8kHz
}
```

### Encoding PCM Audio

```go
package main

import (
    "log"
    "github.com/hicaoc/OpenDMR/go"
)

func main() {
    // Create encoder
    encoder, err := opendmr.NewEncoder()
    if err != nil {
        log.Fatal(err)
    }
    defer encoder.Close()

    // Optional: Set gain adjustment
    encoder.SetGain(0)  // 0 dB (range: -20 to +20)

    // Prepare buffers
    pcmSamples := make([]int16, opendmr.PcmSamples)    // 160 samples @ 8kHz
    ambeFrame := make([]byte, opendmr.AmbeFrameBytes)  // 9 bytes

    // Fill pcmSamples with your audio data
    // ... (160 samples of 16-bit PCM @ 8kHz)

    // Encode to AMBE
    err = encoder.Encode(pcmSamples, ambeFrame)
    if err != nil {
        log.Printf("Encode failed: %v", err)
        return
    }
    
    // Now ambeFrame contains 9 bytes of AMBE+2 encoded audio
}
```

## API Reference

### Constants

```go
const (
    AmbeFrameBytes = 9      // AMBE+2 frame size in bytes
    AmbeFrameBits  = 72     // AMBE+2 frame size in bits
    PcmSamples     = 160    // PCM samples per frame (20ms @ 8kHz)
    SampleRate     = 8000   // Audio sample rate in Hz
)
```

### Decoder

```go
// Create a new decoder
decoder, err := opendmr.NewDecoder()

// Decode AMBE frame to PCM
errs, err := decoder.Decode(ambeFrame, pcmSamples)

// Reset decoder state (e.g., new transmission)
decoder.Reset()

// Clean up resources
decoder.Close()
```

### Encoder

```go
// Create a new encoder
encoder, err := opendmr.NewEncoder()

// Encode PCM to AMBE frame
err := encoder.Encode(pcmSamples, ambeFrame)

// Set gain adjustment (-20 to +20 dB)
encoder.SetGain(gainDB)

// Reset encoder state (e.g., new transmission)
encoder.Reset()

// Clean up resources
encoder.Close()
```

### Utility Functions

```go
// Get library version
version := opendmr.Version()

// Convert AMBE frame from bytes to bits
err := opendmr.ConvertFrameToBits(ambeBytes, ambeBits)

// Convert AMBE frame from bits to bytes
err := opendmr.ConvertFrameToBytes(ambeBits, ambeBytes)
```

## Example Program

A complete example is provided in the `example/` directory:

```bash
cd go/example
go build
./example
```

This demonstrates:
- Encoder/decoder round-trip
- Decoding sample frames
- Frame format conversion
- Proper resource management

## Building Your Application

### Standard Build

```bash
go build
```

### Cross-Compilation

When cross-compiling, ensure you have the appropriate C++ cross-compiler:

```bash
# For ARM64
CGO_ENABLED=1 GOOS=linux GOARCH=arm64 CC=aarch64-linux-gnu-g++ go build

# For ARM
CGO_ENABLED=1 GOOS=linux GOARCH=arm CC=arm-linux-gnueabihf-g++ go build
```

### Static Binary

The OpenDMR library is already statically linked. To create a fully static binary:

```bash
CGO_ENABLED=1 go build -ldflags '-extldflags "-static"'
```

## Technical Details

### Audio Format

- **Sample Rate**: 8000 Hz
- **Bit Depth**: 16-bit signed PCM
- **Frame Size**: 160 samples (20ms)
- **AMBE Frame**: 9 bytes (72 bits)

### Memory Management

- Decoders and encoders use finalizers for automatic cleanup
- Explicit `Close()` methods for deterministic resource release
- Safe to call `Close()` multiple times
- Resources are freed even if `Close()` is not called (via finalizer)

### Thread Safety

Each decoder/encoder instance maintains its own state and should not be shared between goroutines without synchronization. Create separate instances for concurrent use.

## Integration Example

Here's how to integrate OpenDMR into a DMR application:

```go
type DMRVoiceHandler struct {
    decoder *opendmr.Decoder
    encoder *opendmr.Encoder
}

func NewDMRVoiceHandler() (*DMRVoiceHandler, error) {
    decoder, err := opendmr.NewDecoder()
    if err != nil {
        return nil, err
    }
    
    encoder, err := opendmr.NewEncoder()
    if err != nil {
        decoder.Close()
        return nil, err
    }
    
    return &DMRVoiceHandler{
        decoder: decoder,
        encoder: encoder,
    }, nil
}

func (h *DMRVoiceHandler) ProcessIncomingVoice(ambeFrame []byte) ([]int16, error) {
    pcm := make([]int16, opendmr.PcmSamples)
    _, err := h.decoder.Decode(ambeFrame, pcm)
    return pcm, err
}

func (h *DMRVoiceHandler) ProcessOutgoingVoice(pcm []int16) ([]byte, error) {
    ambe := make([]byte, opendmr.AmbeFrameBytes)
    err := h.encoder.Encode(pcm, ambe)
    return ambe, err
}

func (h *DMRVoiceHandler) Close() {
    h.decoder.Close()
    h.encoder.Close()
}
```

## Troubleshooting

### "undefined reference" errors

Make sure `libopendmr.a` is built and copied to the Go package:
```bash
cd /path/to/OpenDMR
make clean
make
cd go
make prepare
```

### "No such file or directory" for opendmr.h

Run `make prepare` in the `go/` directory to copy the header and library files:
```bash
cd /path/to/OpenDMR/go
make prepare
```

### CGO errors

Ensure CGO is enabled:
```bash
export CGO_ENABLED=1
```

### Library not found

The CGO directives expect the library files in the Go package directory. Run `make prepare` to set up:
```
OpenDMR/
├── libopendmr.a          # Built by main Makefile
├── opendmr.h
└── go/
    ├── libopendmr.a      # Copied by 'make prepare'
    ├── opendmr.h         # Copied by 'make prepare'
    ├── opendmr.go
    └── ...
```

## License

GNU General Public License v2.0 (GPL-2.0)

This wrapper inherits the license from the OpenDMR library.

## Credits

- **OpenDMR**: hicaoc and contributors
- **mbelib-neo**: arancormonk (based on mbelib by Pavel Yazev)
- **OP25 MBEEncoder**: Max H. Parke KA1RBI
