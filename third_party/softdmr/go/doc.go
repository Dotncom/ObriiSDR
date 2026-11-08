// Package opendmr provides Go bindings for the OpenDMR library,
// a software implementation of the DMR AMBE+2 vocoder for encoding
// and decoding digital voice.
//
// OpenDMR is an open-source DMR vocoder that requires no proprietary
// hardware. It integrates vocoder implementations from mbelib-neo
// (decoder) and OP25 MBEEncoder (encoder).
//
// # Installation
//
// To use this package, you need to have the OpenDMR C++ library built:
//
//	cd /path/to/OpenDMR
//	make
//
// This will create libopendmr.a which is statically linked by this Go package.
//
// # Usage
//
// Decoding DMR audio:
//
//	decoder, err := opendmr.NewDecoder()
//	if err != nil {
//	    log.Fatal(err)
//	}
//	defer decoder.Close()
//
//	ambeFrame := make([]byte, opendmr.AmbeFrameBytes)
//	pcmSamples := make([]int16, opendmr.PcmSamples)
//
//	// Read AMBE frame from your source
//	// ...
//
//	errs, err := decoder.Decode(ambeFrame, pcmSamples)
//	if err != nil {
//	    log.Printf("Decode failed: %v", err)
//	}
//	log.Printf("Decoded with %d corrected errors", errs)
//
// Encoding PCM audio:
//
//	encoder, err := opendmr.NewEncoder()
//	if err != nil {
//	    log.Fatal(err)
//	}
//	defer encoder.Close()
//
//	pcmSamples := make([]int16, opendmr.PcmSamples)
//	ambeFrame := make([]byte, opendmr.AmbeFrameBytes)
//
//	// Fill pcmSamples with audio data (160 samples @ 8kHz)
//	// ...
//
//	err = encoder.Encode(pcmSamples, ambeFrame)
//	if err != nil {
//	    log.Printf("Encode failed: %v", err)
//	}
//
// # Build Requirements
//
// This package uses CGO and requires:
//   - C++ compiler (g++ or clang++)
//   - OpenDMR library built (libopendmr.a)
//   - CGO enabled (CGO_ENABLED=1)
//
// # License
//
// GNU General Public License v2.0 (GPL-2.0)
package opendmr
