package main

import (
	"encoding/hex"
	"fmt"
	"log"
	"math"

	opendmr "github.com/hicaoc/OpenDMR/go"
)

func main() {
	fmt.Printf("OpenDMR Go Wrapper Example\n")
	fmt.Printf("Library Version: %s\n\n", opendmr.Version())

	// Example 1: Encoder/Decoder Round-trip
	fmt.Println("=== Example 1: Encode/Decode Round-trip ===")
	roundTripExample()

	// Example 2: Decoder with sample AMBE frame
	fmt.Println("\n=== Example 2: Decode Sample Frame ===")
	decoderExample()

	// Example 3: Frame format conversion
	fmt.Println("\n=== Example 3: Frame Format Conversion ===")
	conversionExample()
}

// roundTripExample demonstrates encoding PCM to AMBE and decoding back
func roundTripExample() {
	// Create encoder
	encoder, err := opendmr.NewEncoder()
	if err != nil {
		log.Fatalf("Failed to create encoder: %v", err)
	}
	defer encoder.Close()

	// Set encoder gain (optional)
	encoder.SetGain(0) // 0 dB (no adjustment)

	// Create decoder
	decoder, err := opendmr.NewDecoder()
	if err != nil {
		log.Fatalf("Failed to create decoder: %v", err)
	}
	defer decoder.Close()

	// Generate a simple test tone (1000 Hz sine wave at 8kHz sample rate)
	pcmInput := make([]int16, opendmr.PcmSamples)
	for i := range opendmr.PcmSamples {
		// Generate sine wave: amplitude * sin(2π * frequency * time)
		t := float64(i) / float64(opendmr.SampleRate)
		sample := 8000.0 * math.Sin(2*math.Pi*1000.0*t)
		pcmInput[i] = int16(sample)
	}

	// Encode PCM to AMBE
	ambeFrame := make([]byte, opendmr.AmbeFrameBytes)
	err = encoder.Encode(pcmInput, ambeFrame)
	if err != nil {
		log.Fatalf("Encode failed: %v", err)
	}
	fmt.Printf("Encoded AMBE frame (%d bytes): %s\n", len(ambeFrame), hex.EncodeToString(ambeFrame))

	// Decode AMBE back to PCM
	pcmOutput := make([]int16, opendmr.PcmSamples)
	errs, err := decoder.Decode(ambeFrame, pcmOutput)
	if err != nil {
		log.Fatalf("Decode failed: %v", err)
	}
	fmt.Printf("Decoded successfully with %d corrected bit errors\n", errs)

	// Show first few samples
	fmt.Printf("First 10 PCM samples (input):  %v\n", pcmInput[:10])
	fmt.Printf("First 10 PCM samples (output): %v\n", pcmOutput[:10])
}

// decoderExample demonstrates decoding a sample AMBE frame
func decoderExample() {
	decoder, err := opendmr.NewDecoder()
	if err != nil {
		log.Fatalf("Failed to create decoder: %v", err)
	}
	defer decoder.Close()

	// Sample AMBE frame (9 bytes) - this is just example data
	// In real usage, you would receive this from a DMR radio or network
	ambeFrame := []byte{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}

	pcmSamples := make([]int16, opendmr.PcmSamples)
	errs, err := decoder.Decode(ambeFrame, pcmSamples)
	if err != nil {
		log.Fatalf("Decode failed: %v", err)
	}

	fmt.Printf("Decoded %d PCM samples with %d corrected errors\n", len(pcmSamples), errs)
	fmt.Printf("Sample rate: %d Hz\n", opendmr.SampleRate)
	fmt.Printf("Frame duration: %.1f ms\n", float64(opendmr.PcmSamples)*1000.0/float64(opendmr.SampleRate))

	// Reset decoder state (useful when starting a new transmission)
	decoder.Reset()
	fmt.Println("Decoder state reset")
}

// conversionExample demonstrates converting between byte and bit array formats
func conversionExample() {
	// Sample AMBE frame in byte format
	ambeBytes := []byte{0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC}
	fmt.Printf("AMBE frame (bytes): %s\n", hex.EncodeToString(ambeBytes))

	// Convert to bit array
	ambeBits := make([]uint8, opendmr.AmbeFrameBits)
	err := opendmr.ConvertFrameToBits(ambeBytes, ambeBits)
	if err != nil {
		log.Fatalf("Conversion to bits failed: %v", err)
	}
	fmt.Printf("AMBE frame (first 16 bits): %v...\n", ambeBits[:16])

	// Convert back to bytes
	ambeBytes2 := make([]byte, opendmr.AmbeFrameBytes)
	err = opendmr.ConvertFrameToBytes(ambeBits, ambeBytes2)
	if err != nil {
		log.Fatalf("Conversion to bytes failed: %v", err)
	}
	fmt.Printf("AMBE frame (bytes): %s\n", hex.EncodeToString(ambeBytes2))

	// Verify round-trip conversion
	match := true
	for i := range ambeBytes {
		if ambeBytes[i] != ambeBytes2[i] {
			match = false
			break
		}
	}
	if match {
		fmt.Println("✓ Round-trip conversion successful!")
	} else {
		fmt.Println("✗ Round-trip conversion failed!")
	}
}
