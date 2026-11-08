package main

import (
	"strconv"

	"github.com/MattCheramie/GopherTrunk/internal/radio/dmr"
)

// Already-sliced input is not a continuous RF stream. Never close a missing
// A-F burst with a later burst or count a PI header as 132 voice dibits.
type burstAssembly struct {
	Dibits    []uint8
	Corrected []string
	First     inputPacket
	Base      int
	next      int
	lastRF    uint64
	hasRF     bool
}

func (a *burstAssembly) Active() bool { return a.next > 0 }

func (a *burstAssembly) Reset() {
	a.Dibits = a.Dibits[:0]
	a.Corrected = a.Corrected[:0]
	a.First = inputPacket{}
	a.next = 0
	a.hasRF = false
}

func (a *burstAssembly) Push(pkt inputPacket, dibits []uint8, base int) (bool, bool) {
	if pkt.Burst == nil || len(dibits) != dmr.BurstDibits || *pkt.Burst < 0 || *pkt.Burst > 5 {
		lost := a.Active()
		a.Reset()
		return false, lost
	}
	burst := *pkt.Burst
	rf, err := strconv.ParseUint(pkt.RFBaseIdx, 10, 64)
	hasRF := err == nil && pkt.RFBaseIdx != ""
	lost := false
	if a.Active() {
		lost = burst != a.next || (pkt.Timeslot != 0 && a.First.Timeslot != 0 && pkt.Timeslot != a.First.Timeslot)
		if hasRF && a.hasRF && pkt.Cadence > 0 {
			expected := a.lastRF + uint64(pkt.Cadence)
			lost = lost || rf+2 < expected || rf > expected+2
		}
		if lost {
			a.Reset()
		}
	}
	if burst == 0 {
		a.First, a.Base = pkt, base
	} else if !a.Active() {
		return false, lost
	}
	a.Dibits = append(a.Dibits, dibits...)
	for frame := 0; frame < 3; frame++ {
		payload := ""
		if len(pkt.AMBE49) == 3 {
			payload = pkt.AMBE49[frame]
		}
		a.Corrected = append(a.Corrected, payload)
	}
	a.next = burst + 1
	a.lastRF, a.hasRF = rf, hasRF
	return a.next == 6, lost
}

func (p *privacyState) LoseVoicePosition() {
	p.rc4MIValid = false
	p.rc4 = nil
	p.sync = false
	p.lateKnown = false
	p.lateCRCOk = false
	p.pendingLateHits = 0
}

func (p *privacyState) SkipVoiceFrame() {
	if p.mode == "arc4" && p.rc4MIValid {
		p.rc4FrameIndex++
	}
	p.MarkMutedFrame()
	p.MarkVoiceFrameSeen()
}
