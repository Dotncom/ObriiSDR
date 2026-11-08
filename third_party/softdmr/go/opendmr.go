package opendmr

/*
#cgo CFLAGS: -I${SRCDIR}
#cgo LDFLAGS: ${SRCDIR}/libopendmr.a -lstdc++ -lm
#include "opendmr.h"
#include <stdlib.h>
*/
import "C"
import (
	"errors"
	"runtime"
	"unsafe"
)

// Constants from the C library
const (
	// AmbeFrameBytes is the size of an AMBE+2 frame in bytes (9 bytes = 72 bits)
	AmbeFrameBytes = C.OPENDMR_AMBE_FRAME_BYTES

	// AmbeFrameBits is the size of an AMBE+2 frame in bits (72 bits)
	AmbeFrameBits = C.OPENDMR_AMBE_FRAME_BITS

	// PcmSamples is the number of PCM samples per frame (160 samples = 20ms @ 8kHz)
	PcmSamples = C.OPENDMR_PCM_SAMPLES

	// SampleRate is the audio sample rate (8000 Hz)
	SampleRate = C.OPENDMR_SAMPLE_RATE
)

// Errors
var (
	ErrDecoderCreate = errors.New("opendmr: failed to create decoder")
	ErrEncoderCreate = errors.New("opendmr: failed to create encoder")
	ErrDecodeFailed  = errors.New("opendmr: decode operation failed")
	ErrEncodeFailed  = errors.New("opendmr: encode operation failed")
	ErrInvalidInput  = errors.New("opendmr: invalid input size")
)

// Decoder represents a DMR AMBE+2 decoder instance
type Decoder struct {
	dec *C.opendmr_decoder_t
}

// NewDecoder creates a new DMR decoder instance.
// The decoder must be closed with Close() when no longer needed.
func NewDecoder() (*Decoder, error) {
	dec := C.opendmr_decoder_create()
	if dec == nil {
		return nil, ErrDecoderCreate
	}

	d := &Decoder{dec: dec}
	runtime.SetFinalizer(d, (*Decoder).Close)
	return d, nil
}

// Decode decodes a DMR AMBE+2 frame to PCM audio.
//
// Parameters:
//   - ambe: Input AMBE+2 frame (must be exactly 9 bytes)
//   - pcm: Output PCM buffer (must be at least 160 samples)
//
// Returns the number of corrected bit errors and any error encountered.
// The decoder maintains state between frames for proper audio continuity.
func (d *Decoder) Decode(ambe []byte, pcm []int16) (int, error) {
	if len(ambe) != AmbeFrameBytes {
		return 0, ErrInvalidInput
	}
	if len(pcm) < PcmSamples {
		return 0, ErrInvalidInput
	}

	var errs C.int
	success := C.opendmr_decode(
		d.dec,
		(*C.uint8_t)(unsafe.Pointer(&ambe[0])),
		(*C.int16_t)(unsafe.Pointer(&pcm[0])),
		&errs,
	)

	if !success {
		return 0, ErrDecodeFailed
	}

	return int(errs), nil
}

// Reset resets the decoder state (e.g., at start of new transmission).
func (d *Decoder) Reset() {
	if d.dec != nil {
		C.opendmr_decoder_reset(d.dec)
	}
}

// Close destroys the decoder and frees resources.
// It's safe to call Close multiple times.
func (d *Decoder) Close() error {
	if d.dec != nil {
		C.opendmr_decoder_destroy(d.dec)
		d.dec = nil
		runtime.SetFinalizer(d, nil)
	}
	return nil
}

// Encoder represents a DMR AMBE+2 encoder instance
type Encoder struct {
	enc *C.opendmr_encoder_t
}

// NewEncoder creates a new DMR encoder instance.
// The encoder must be closed with Close() when no longer needed.
func NewEncoder() (*Encoder, error) {
	enc := C.opendmr_encoder_create()
	if enc == nil {
		return nil, ErrEncoderCreate
	}

	e := &Encoder{enc: enc}
	runtime.SetFinalizer(e, (*Encoder).Close)
	return e, nil
}

// Encode encodes PCM audio to a DMR AMBE+2 frame.
//
// Parameters:
//   - pcm: Input PCM buffer (must be exactly 160 samples, 16-bit signed, 8kHz)
//   - ambe: Output AMBE+2 frame (must be at least 9 bytes)
//
// The encoder maintains state between frames for proper voice analysis.
// For best results, encode frames in sequence without gaps.
func (e *Encoder) Encode(pcm []int16, ambe []byte) error {
	if len(pcm) != PcmSamples {
		return ErrInvalidInput
	}
	if len(ambe) < AmbeFrameBytes {
		return ErrInvalidInput
	}

	success := C.opendmr_encode(
		e.enc,
		(*C.int16_t)(unsafe.Pointer(&pcm[0])),
		(*C.uint8_t)(unsafe.Pointer(&ambe[0])),
	)

	if !success {
		return ErrEncodeFailed
	}

	return nil
}

// Reset resets the encoder state (e.g., at start of new transmission).
func (e *Encoder) Reset() {
	if e.enc != nil {
		C.opendmr_encoder_reset(e.enc)
	}
}

// SetGain sets the encoder gain adjustment in dB.
//
// Parameters:
//   - gainDB: Gain adjustment (-20 to +20 dB, default 0)
//
// Positive values increase output level, negative values decrease.
func (e *Encoder) SetGain(gainDB int) {
	if e.enc != nil {
		C.opendmr_encoder_set_gain(e.enc, C.int(gainDB))
	}
}

// Close destroys the encoder and frees resources.
// It's safe to call Close multiple times.
func (e *Encoder) Close() error {
	if e.enc != nil {
		C.opendmr_encoder_destroy(e.enc)
		e.enc = nil
		runtime.SetFinalizer(e, nil)
	}
	return nil
}

// Version returns the library version string.
func Version() string {
	return C.GoString(C.opendmr_version())
}

// ConvertFrameToBytes converts an AMBE+2 frame from bit array to byte array format.
//
// Parameters:
//   - bits: Bit array (72 bits, one bit per element)
//   - bytes: Byte array (9 bytes, MSB first per byte)
func ConvertFrameToBytes(bits []uint8, bytes []byte) error {
	if len(bits) != AmbeFrameBits {
		return ErrInvalidInput
	}
	if len(bytes) < AmbeFrameBytes {
		return ErrInvalidInput
	}

	C.opendmr_convert_frame(
		(*C.uint8_t)(unsafe.Pointer(&bytes[0])),
		(*C.uint8_t)(unsafe.Pointer(&bits[0])),
		C.bool(false),
	)

	return nil
}

// ConvertFrameToBits converts an AMBE+2 frame from byte array to bit array format.
//
// Parameters:
//   - bytes: Byte array (9 bytes, MSB first per byte)
//   - bits: Bit array (72 bits, one bit per element)
func ConvertFrameToBits(bytes []byte, bits []uint8) error {
	if len(bytes) != AmbeFrameBytes {
		return ErrInvalidInput
	}
	if len(bits) < AmbeFrameBits {
		return ErrInvalidInput
	}

	C.opendmr_convert_frame(
		(*C.uint8_t)(unsafe.Pointer(&bytes[0])),
		(*C.uint8_t)(unsafe.Pointer(&bits[0])),
		C.bool(true),
	)

	return nil
}
