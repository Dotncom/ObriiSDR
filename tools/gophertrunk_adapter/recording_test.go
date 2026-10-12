package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"os"
	"testing"

	dmrvoice "github.com/MattCheramie/GopherTrunk/internal/radio/dmr/voice"
)

// The optional fixture is a private bridge JSONL capture, never part of a release.
func TestPrivateRecordingLateEntry(t *testing.T) {
	path := os.Getenv("OBRII_GOPHER_FIXTURE")
	if path == "" {
		t.Skip("no private recording supplied")
	}
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var assembly burstAssembly
	decoder := dmrvoice.NewDecoder()
	scanner := bufio.NewScanner(f)
	valid, complete, compared, mismatched := 0, 0, 0, 0
	for scanner.Scan() {
		var packet inputPacket
		if err := json.Unmarshal(scanner.Bytes(), &packet); err != nil {
			t.Fatal(err)
		}
		if packet.Reset || isDataPacket(packet) {
			assembly.Reset()
			continue
		}
		dibits, err := normalizeDibits(packet)
		if err != nil {
			t.Fatal(err)
		}
		ready, _ := assembly.Push(packet, dibits, 0)
		if !ready {
			continue
		}
		decoder.Reset()
		for _, sf := range decoder.Process(assembly.Dibits, 0) {
			complete++
			candidate, _ := lateEntryCandidateFromRawFrames(sf.Frames)
			if trustedLateEntryCandidate(candidate) {
				valid++
			}
			for index, raw := range sf.Frames {
				if assembly.Corrected[index] == "" {
					continue
				}
				info, errors, err := dmrvoice.DecodeAMBEFrame(raw)
				if err == nil && errors == 0 && len(info) == 49 {
					compared++
					native, err := bitsFromHexMSB(assembly.Corrected[index], 49)
					if err != nil || !bytes.Equal(info, native) {
						mismatched++
					}
				}
			}
		}
		assembly.Reset()
	}
	if scanner.Err() != nil {
		t.Fatal(scanner.Err())
	}
	t.Logf("superframes=%d valid late entry=%d payload comparisons=%d mismatched=%d", complete, valid, compared, mismatched)
	if complete < 2 || valid*10 < complete*9 || compared == 0 || mismatched != 0 {
		t.Fatal("recording failed protocol/payload checks")
	}
}
