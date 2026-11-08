package main

import (
	"encoding/hex"
	"testing"

	"github.com/MattCheramie/GopherTrunk/internal/radio/framing"
)

func TestPIHeaderCRCRejectsCorruption(t *testing.T) {
	raw, err := hex.DecodeString("21100112345678000000")
	if err != nil {
		t.Fatal(err)
	}
	crc := framing.CRCCCITTWithInit(raw, 0) ^ 0xffff ^ 0x6969
	raw = append(raw, byte(crc>>8), byte(crc))
	bits := framing.UnpackBitsMSB(raw, 96)
	if _, ok := parseDMRPiHeader(bits); !ok {
		t.Fatal("valid synthetic PI rejected")
	}
	for bit := range bits {
		bad := append([]byte(nil), bits...)
		bad[bit] ^= 1
		if _, ok := parseDMRPiHeader(bad); ok {
			t.Fatalf("corrupt PI bit %d accepted", bit)
		}
	}
	for _, count := range []int{0, 56, 80, 95, 97} {
		if _, ok := parseDMRPiHeader(make([]byte, count)); ok {
			t.Fatalf("PI length %d accepted", count)
		}
	}
}

func TestCorruptPIDoesNotReplaceValidMI(t *testing.T) {
	p := newPrivacyState()
	p.Configure(privacyConfig{Mode: "arc4", KeyID: 1, KeyHex: "0102030405"})
	raw, _ := hex.DecodeString("21100112345678000000")
	crc := framing.CRCCCITTWithInit(raw, 0) ^ 0xffff ^ 0x6969
	raw = append(raw, byte(crc>>8), byte(crc))
	bits := framing.UnpackBitsMSB(raw, 96)
	pi, ok := parseDMRPiHeader(bits)
	if !ok {
		t.Fatal("valid PI rejected")
	}
	if accepted, reason := p.UpdatePIHeader(pi, 0); !accepted {
		t.Fatalf("valid PI update failed: %s", reason)
	}
	p.voiceFrames = 9
	p.rc4MI = 0x12345678
	p.rc4MIValid = true
	bits[24] ^= 1
	if corrupt, valid := parseDMRPiHeader(bits); valid {
		p.UpdatePIHeader(corrupt, 0)
	}
	if p.lateMI != 0x12345678 || p.rc4MI != 0x12345678 || p.voiceFrames != 9 {
		t.Fatal("corrupt PI changed cipher state")
	}
}
