package main

import (
	"encoding/hex"
	"testing"

	"github.com/MattCheramie/GopherTrunk/internal/radio/framing"
)

func TestParseDMRPiHeaderDMRALayout(t *testing.T) {
	raw, err := hex.DecodeString("2110CF3930D1B100017B")
	if err != nil {
		t.Fatal(err)
	}
	crc := framing.CRCCCITTWithInit(raw, 0) ^ 0xffff ^ 0x6969
	raw = append(raw, byte(crc>>8), byte(crc))
	pi, ok := parseDMRPiHeader(framing.UnpackBitsMSB(raw, len(raw)*8))
	if !ok {
		t.Fatal("PI header was not recognized")
	}
	if pi.MFID != 0x10 {
		t.Fatalf("MFID = %02X, want 10", pi.MFID)
	}
	if pi.ALG != 1 || pi.Alg != "RC4" {
		t.Fatalf("ALG = %d/%s, want 1/RC4", pi.ALG, pi.Alg)
	}
	if pi.KeyID != 0xCF {
		t.Fatalf("KeyID = %02X, want CF", pi.KeyID)
	}
	if pi.MI != "3930D1B1" {
		t.Fatalf("MI = %s, want 3930D1B1", pi.MI)
	}
}

func TestPIHeaderResetsRC4FramePosition(t *testing.T) {
	p := newPrivacyState()
	p.Configure(privacyConfig{Mode: "arc4", KeyID: 0xCF, KeyHex: "0102030405"})
	p.voiceFrames = 18
	p.totalFrames = 18
	p.changedFrames = 18
	p.rc4StartFrame = 18
	p.rc4DropBytes = 382
	p.sync = true

	accepted, reason := p.UpdatePIHeader(piHeaderInfo{
		MFID:  0x10,
		ENC:   2,
		ALG:   1,
		Alg:   "RC4",
		KeyID: 0xCF,
		MI:    "3930D1B1",
	}, 0)
	if !accepted {
		t.Fatalf("PI header rejected: %s", reason)
	}
	if p.voiceFrames != 0 || p.totalFrames != 0 || p.changedFrames != 0 {
		t.Fatalf("frame counters were not reset: voice=%d total=%d changed=%d", p.voiceFrames, p.totalFrames, p.changedFrames)
	}
	if p.rc4StartFrame != 0 || p.rc4DropBytes != 0 || p.sync {
		t.Fatalf("RC4 position was not reset: start=%d drop=%d sync=%v", p.rc4StartFrame, p.rc4DropBytes, p.sync)
	}
	if !p.lateKnown || !p.lateCRCOk || p.lateMI != 0x3930D1B1 || p.lateSource != "pi-header" {
		t.Fatalf("PI metadata was not preserved: known=%v ok=%v mi=%08X source=%q", p.lateKnown, p.lateCRCOk, p.lateMI, p.lateSource)
	}
}
