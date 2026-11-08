// Command fobos-dmr-virtual is a small lab bridge for feeding FobosAPP's
// already-sliced DMR dibit stream into GopherTrunk's DMR voice stack.
//
// It intentionally does not pretend to be a hardware SDR. FobosAPP owns the
// wideband IQ path, channel selection, discriminator, symbol timing, and
// 4-level slicer. This command starts at the boundary GopherTrunk's DMR code
// already exposes: []uint8 dibits plus a monotonic baseIdx.
//
// Input is newline-delimited JSON on stdin, or from one TCP client when
// -listen is set:
//
//	{"baseIdx":0,"dibits":"010310020223332030..."}
//	{"reset":true}
//
// The dibits field may be either a string of 0..3 characters or a JSON array
// of numbers. If baseIdx is omitted, the command continues from the previous
// packet. Output is NDJSON summaries on stdout. When -audio-udp is set, decoded
// 8 kHz PCM16LE from the pure-Go AMBE+2 DMR vocoder is also sent to that UDP
// target.
package main

import (
	"bufio"
	"bytes"
	"crypto/cipher"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	gtrc4 "github.com/MattCheramie/GopherTrunk/internal/crypto/rc4"
	"github.com/MattCheramie/GopherTrunk/internal/radio/dmr"
	dmrvoice "github.com/MattCheramie/GopherTrunk/internal/radio/dmr/voice"
	"github.com/MattCheramie/GopherTrunk/internal/radio/framing"
	"github.com/MattCheramie/GopherTrunk/internal/voice"

	_ "github.com/MattCheramie/GopherTrunk/internal/voice/ambe2"
)

type inputPacket struct {
	Burst       *int            `json:"burst,omitempty"`
	RFBaseIdx   string          `json:"rfBaseIdx,omitempty"`
	Cadence     int             `json:"cadenceSymbols,omitempty"`
	BaseIdx     *int            `json:"baseIdx,omitempty"`
	Dibits      json.RawMessage `json:"dibits,omitempty"`
	DibitString string          `json:"dibitString,omitempty"`
	AMBE49      []string        `json:"ambe49,omitempty"`
	BurstKind   string          `json:"burstKind,omitempty"`
	DataType    int             `json:"dataType,omitempty"`
	Privacy     *privacyConfig  `json:"privacy,omitempty"`
	Timeslot    int             `json:"timeslot,omitempty"`
	TSSource    string          `json:"timeslotSource,omitempty"`
	CACH        *cachSummary    `json:"cach,omitempty"`
	Reset       bool            `json:"reset,omitempty"`
}

type cachSummary struct {
	Decoded bool   `json:"decoded,omitempty"`
	Channel int    `json:"channel,omitempty"`
	LCSS    int    `json:"lcss,omitempty"`
	Map     string `json:"map,omitempty"`
	Payload string `json:"payload,omitempty"`
}

type privacyConfig struct {
	Mode        string `json:"mode,omitempty"`
	KeyID       int    `json:"keyId,omitempty"`
	KeyHex      string `json:"keyHex,omitempty"`
	Variant     string `json:"variant,omitempty"`
	Layout      string `json:"layout,omitempty"`
	FrameOffset *int   `json:"frameOffset,omitempty"`
}

type privacyOverride struct {
	path    string
	modTime time.Time
	size    int64
}

type privacySummary struct {
	Mode            string `json:"mode,omitempty"`
	Variant         string `json:"variant,omitempty"`
	Layout          string `json:"layout,omitempty"`
	FrameOffset     int    `json:"frameOffset,omitempty"`
	KeyID           int    `json:"keyId,omitempty"`
	KeySet          bool   `json:"keySet,omitempty"`
	Applied         bool   `json:"applied,omitempty"`
	Sync            bool   `json:"sync,omitempty"`
	Encrypted       bool   `json:"encrypted,omitempty"`
	Known           bool   `json:"known,omitempty"`
	Error           string `json:"error,omitempty"`
	FrameCount      int    `json:"frameCount,omitempty"`
	Changed         bool   `json:"changed,omitempty"`
	ChangedFrames   int    `json:"changedFrames,omitempty"`
	Muted           bool   `json:"muted,omitempty"`
	MutedFrames     int    `json:"mutedFrames,omitempty"`
	VoiceFrames     int    `json:"voiceFrames,omitempty"`
	SkippedFrames   int    `json:"skippedFrames,omitempty"`
	RC4StartFrame   int    `json:"rc4StartFrame,omitempty"`
	RC4DropBytes    int    `json:"rc4DropBytes,omitempty"`
	LateEntryMI     string `json:"leMi,omitempty"`
	VoiceMI         string `json:"voiceMi,omitempty"`
	NextMI          string `json:"nextMi,omitempty"`
	LateEntryCRCOk  bool   `json:"leCrcOk,omitempty"`
	LateEntryCRCExt int    `json:"leCrcExt,omitempty"`
	LateEntryCRCCmp int    `json:"leCrcCmp,omitempty"`
	LateEntryRaw    string `json:"leRaw,omitempty"`
	LateEntrySource string `json:"leSource,omitempty"`
	LateEntryCands  string `json:"leCandidates,omitempty"`
}

type lcSummary struct {
	FLCO      string `json:"flco"`
	FID       uint8  `json:"fid"`
	Service   uint8  `json:"service"`
	Dst       uint32 `json:"dst"`
	Src       uint32 `json:"src"`
	Group     uint32 `json:"group,omitempty"`
	Encrypted bool   `json:"encrypted,omitempty"`
	Emergency bool   `json:"emergency,omitempty"`
}

type outputPacket struct {
	Type        string          `json:"type"`
	StartDibit  int             `json:"startDibit"`
	Phase       uint8           `json:"phase"`
	Timeslot    int             `json:"timeslot,omitempty"`
	TSSource    string          `json:"timeslotSource,omitempty"`
	CACH        *cachSummary    `json:"cach,omitempty"`
	Sync        string          `json:"sync"`
	HasLC       bool            `json:"hasLC"`
	LC          *lcSummary      `json:"lc,omitempty"`
	HasRC       bool            `json:"hasRC"`
	RcInfo      uint16          `json:"rcInfo,omitempty"`
	Frames      int             `json:"frames"`
	FecErrors   []int           `json:"fecErrors"`
	BadFrames   int             `json:"badFrames"`
	AudioFrames int             `json:"audioFrames,omitempty"`
	PrivacyText string          `json:"privacyText,omitempty"`
	Privacy     *privacySummary `json:"privacy,omitempty"`
}

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "fobos-dmr-virtual: "+err.Error())
		os.Exit(1)
	}
}

func run() error {
	var (
		listen        = flag.String("listen", "", "optional TCP listen address, for example 127.0.0.1:7460")
		interleaved   = flag.Bool("interleaved", true, "use the real two-slot DMR voice decoder")
		audioUDP      = flag.String("audio-udp", "", "optional UDP target for decoded PCM16LE, for example 127.0.0.1:23456")
		vocoderName   = flag.String("vocoder", "ambe2-dmr", "GopherTrunk vocoder name")
		privacyMode   = flag.String("privacy-mode", os.Getenv("FOBOS_DMR_PRIVACY_MODE"), "known-key DMR privacy mode: none, arc4, aes256")
		privacyKeyID  = flag.Int("privacy-key-id", envInt("FOBOS_DMR_PRIVACY_KEY_ID", 0), "known-key DMR privacy key ID")
		privacyKeyHex = flag.String("privacy-key-hex", os.Getenv("FOBOS_DMR_PRIVACY_KEY_HEX"), "known DMR privacy key in hex")
		privacyVar    = flag.String("privacy-variant", os.Getenv("FOBOS_DMR_PRIVACY_VARIANT"), "DMR ARC4 test variant: dmra, key-reverse, mi-reverse, key-mi-reverse, fixed-mi, drop0")
		privacyLayout = flag.String("privacy-layout", os.Getenv("FOBOS_DMR_PRIVACY_LAYOUT"), "DMR ARC4 AMBE layout: normal, reverse49, bit-reverse-bytes, byte-reverse")
		frameOffset   = flag.Int("privacy-frame-offset", envInt("FOBOS_DMR_PRIVACY_FRAME_OFFSET", 0), "DMR ARC4 AMBE frame offset, 0..17")
		overrideFile  = flag.String("privacy-override-file", os.Getenv("FOBOS_DMR_PRIVACY_OVERRIDE_FILE"), "optional live privacy override file with variant=... and frameOffset=...")
	)
	flag.Parse()
	defaultPrivacy := privacyConfig{
		Mode:        *privacyMode,
		KeyID:       *privacyKeyID,
		KeyHex:      *privacyKeyHex,
		Variant:     *privacyVar,
		Layout:      *privacyLayout,
		FrameOffset: frameOffset,
	}
	if strings.TrimSpace(os.Getenv("FOBOS_DMR_PRIVACY_FORWARD")) == "0" {
		defaultPrivacy.Mode = "none"
		defaultPrivacy.KeyHex = ""
	}

	var audioConn net.Conn
	var voc voice.Vocoder
	if strings.TrimSpace(*audioUDP) != "" {
		conn, err := net.Dial("udp", *audioUDP)
		if err != nil {
			return err
		}
		defer conn.Close()
		audioConn = conn

		v, err := voice.DefaultRegistry.New(*vocoderName)
		if err != nil {
			return err
		}
		defer v.Close()
		voc = v
	}

	process := func(r io.Reader) error {
		return processStream(r, *interleaved, voc, audioConn, defaultPrivacy, *overrideFile)
	}

	if strings.TrimSpace(*listen) == "" {
		return process(os.Stdin)
	}

	ln, err := net.Listen("tcp", *listen)
	if err != nil {
		return err
	}
	defer ln.Close()
	fmt.Fprintf(os.Stderr, "fobos-dmr-virtual listening on %s\n", ln.Addr())

	for {
		conn, err := ln.Accept()
		if err != nil {
			return err
		}
		fmt.Fprintf(os.Stderr, "fobos-dmr-virtual client %s connected\n", conn.RemoteAddr())
		err = process(conn)
		_ = conn.Close()
		if err != nil {
			fmt.Fprintf(os.Stderr, "fobos-dmr-virtual client ended: %v\n", err)
		}
	}
}

func processStream(r io.Reader, interleaved bool, voc voice.Vocoder, audioConn net.Conn, defaultPrivacy privacyConfig, overrideFile string) error {
	dec := dmrvoice.NewDecoder()
	if interleaved {
		dec = dmrvoice.NewInterleavedDecoder()
	}
	privacy := newPrivacyState()
	privacy.Configure(defaultPrivacy)
	override := privacyOverride{path: strings.TrimSpace(overrideFile)}

	nextBase := 0
	assembly := burstAssembly{}
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 0, 64*1024), 8*1024*1024)
	enc := json.NewEncoder(os.Stdout)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" {
			continue
		}
		var pkt inputPacket
		if err := json.Unmarshal([]byte(line), &pkt); err != nil {
			fmt.Fprintf(os.Stderr, "bad input JSON: %v\n", err)
			continue
		}
		if pkt.Reset {
			dec.Reset()
			assembly.Reset()
			if pkt.Privacy != nil {
				privacy.Configure(*pkt.Privacy)
			} else {
				privacy.Configure(defaultPrivacy)
			}
			privacy.ResetCallState()
			nextBase = 0
			resetOutput := map[string]interface{}{"type": "reset"}
			if summary := privacy.Summary(0); summary != nil {
				resetOutput["privacy"] = summary
			}
			_ = enc.Encode(resetOutput)
			continue
		}
		if pkt.Privacy != nil {
			privacy.Configure(*pkt.Privacy)
		}
		override.Apply(privacy)
		dibits, err := normalizeDibits(pkt)
		if err != nil {
			fmt.Fprintf(os.Stderr, "bad dibits: %v\n", err)
			continue
		}
		base := nextBase
		if pkt.BaseIdx != nil {
			base = *pkt.BaseIdx
		}
		nextBase = base + len(dibits)

		if isDataPacket(pkt) {
			if assembly.Active() {
				privacy.LoseVoicePosition()
			}
			assembly.Reset()
			dec.Reset()
			out := processDataPacket(pkt, dibits, base, privacy)
			if err := enc.Encode(out); err != nil {
				return err
			}
			continue
		}

		if !interleaved && pkt.Burst != nil {
			complete, discontinuity := assembly.Push(pkt, dibits, base)
			if discontinuity {
				privacy.LoseVoicePosition()
				_ = enc.Encode(map[string]any{"type": "dmr.voice.discontinuity", "startDibit": base})
			}
			if complete {
				dec.Reset()
				for _, sf := range dec.Process(assembly.Dibits, 0) {
					sf.StartDibit = assembly.Base
					out := summarizeSuperframe(sf, voc, audioConn, privacy,
						assembly.First.Timeslot, assembly.First.TSSource,
						assembly.First.CACH, assembly.Corrected)
					if err := enc.Encode(out); err != nil {
						return err
					}
				}
				assembly.Reset()
			}
			continue
		}

		for _, sf := range dec.Process(dibits, base) {
			out := summarizeSuperframe(sf, voc, audioConn, privacy, pkt.Timeslot, pkt.TSSource, pkt.CACH, nil)
			if err := enc.Encode(out); err != nil {
				return err
			}
		}
	}
	return sc.Err()
}

func (o *privacyOverride) Apply(privacy *privacyState) {
	if o == nil || privacy == nil || o.path == "" {
		return
	}
	info, err := os.Stat(o.path)
	if err != nil {
		return
	}
	if info.ModTime().Equal(o.modTime) && info.Size() == o.size {
		return
	}
	o.modTime = info.ModTime()
	o.size = info.Size()

	raw, err := os.ReadFile(o.path)
	if err != nil {
		return
	}
	cfg := privacyConfig{
		Mode:    privacy.mode,
		KeyID:   privacy.keyID,
		KeyHex:  privacy.keyHex,
		Variant: privacy.variant,
		Layout:  privacy.layout,
	}
	offset := privacy.frameOffset
	cfg.FrameOffset = &offset

	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		key, value, ok := strings.Cut(line, "=")
		if !ok {
			continue
		}
		key = strings.ToLower(strings.TrimSpace(key))
		value = strings.TrimSpace(value)
		switch key {
		case "variant", "privacyvariant":
			cfg.Variant = value
		case "layout", "privacylayout":
			cfg.Layout = value
		case "frameoffset", "offset":
			if parsed, err := strconv.Atoi(value); err == nil {
				normalized := normalizeFrameOffset(parsed)
				cfg.FrameOffset = &normalized
			}
		}
	}
	privacy.Configure(cfg)
}

func normalizeDibits(pkt inputPacket) ([]uint8, error) {
	fromString := func(s string) ([]uint8, error) {
		out := make([]uint8, 0, len(s))
		for _, r := range s {
			if r < '0' || r > '3' {
				return nil, fmt.Errorf("dibit string contains %q", r)
			}
			out = append(out, uint8(r-'0'))
		}
		return out, nil
	}

	if pkt.DibitString != "" {
		return fromString(pkt.DibitString)
	}
	if len(pkt.Dibits) == 0 || string(pkt.Dibits) == "null" {
		return nil, fmt.Errorf("empty dibits")
	}
	if pkt.Dibits[0] == '"' {
		var s string
		if err := json.Unmarshal(pkt.Dibits, &s); err != nil {
			return nil, err
		}
		return fromString(s)
	}

	var raw []uint8
	if err := json.Unmarshal(pkt.Dibits, &raw); err != nil {
		return nil, err
	}
	out := make([]uint8, len(raw))
	for i, d := range raw {
		if d > 3 {
			return nil, fmt.Errorf("dibit[%d]=%d outside 0..3", i, d)
		}
		out[i] = d
	}
	return out, nil
}

func isDataPacket(pkt inputPacket) bool {
	kind := strings.ToLower(strings.TrimSpace(pkt.BurstKind))
	return kind == "data"
}

func processDataPacket(pkt inputPacket, dibits []uint8, base int, privacy *privacyState) map[string]any {
	out := map[string]any{
		"type":       "dmr.data.burst",
		"startDibit": base,
		"inputKind":  strings.TrimSpace(pkt.BurstKind),
		"inputDT":    pkt.DataType,
	}
	if pkt.CACH != nil {
		out["cach"] = pkt.CACH
	}
	if len(dibits) != dmr.BurstDibits {
		out["error"] = fmt.Sprintf("needs %d dibits, got %d", dmr.BurstDibits, len(dibits))
		return out
	}

	best := dataBurstCandidate{slotErrors: 9999, bptcErrors: 9999}
	for _, rotation := range dmr.CandidatePolarities {
		var burst dmr.Burst
		copy(burst.Dibits[:], dibits)
		dmr.RotateBurstDibits(&burst, rotation)
		slotType, slotErrs, err := dmr.ParseSlotType(burst.SlotTypeBitsAll())
		if err != nil {
			continue
		}
		cand := dataBurstCandidate{
			known:      true,
			rotation:   rotation,
			colorCode:  int(slotType.ColorCode),
			dataType:   slotType.DataType,
			slotErrors: slotErrs,
		}
		if slotType.DataType == dmr.DTPIHeader {
			info, bptcErrs := framing.DecodeBPTC196_96(burst.PayloadBits())
			cand.bptcErrors = bptcErrs
			if bptcErrs >= 0 {
				cand.info = info
				cand.pi, cand.piOK = parseDMRPiHeader(info)
			}
		}
		if betterDataBurstCandidate(cand, best) {
			best = cand
		}
	}
	if !best.known {
		out["error"] = "slot type decode failed"
		return out
	}

	out["polarityRotation"] = best.rotation
	out["colorCode"] = best.colorCode
	out["dataType"] = uint8(best.dataType)
	out["dataTypeName"] = best.dataType.String()
	out["slotErrors"] = best.slotErrors
	if best.bptcErrors != 9999 {
		out["bptcErrors"] = best.bptcErrors
	}
	if best.dataType == dmr.DTPIHeader {
		out["piCrcOk"] = best.piOK
		if !best.piOK {
			out["piReject"] = "PI header CRC, length or FEC check failed"
		}
	}
	if best.piOK {
		out["pi"] = best.pi
		out["piInfoHex"] = strings.ToUpper(hex.EncodeToString(packBits(best.info)))
		if privacy != nil {
			accepted, reason := privacy.UpdatePIHeader(best.pi, best.bptcErrors)
			out["piAccepted"] = accepted
			if reason != "" {
				out["piReject"] = reason
			}
			out["privacy"] = privacy.Summary(0)
		}
	}
	return out
}

type dataBurstCandidate struct {
	known      bool
	rotation   uint8
	colorCode  int
	dataType   dmr.DataType
	slotErrors int
	bptcErrors int
	info       []byte
	pi         piHeaderInfo
	piOK       bool
}

func betterDataBurstCandidate(candidate, current dataBurstCandidate) bool {
	if !candidate.known {
		return false
	}
	if !current.known {
		return true
	}
	candidatePI := 0
	if candidate.piOK {
		candidatePI = 1
	}
	currentPI := 0
	if current.piOK {
		currentPI = 1
	}
	if candidatePI != currentPI {
		return candidatePI > currentPI
	}
	if candidate.dataType == dmr.DTPIHeader && current.dataType != dmr.DTPIHeader {
		return true
	}
	if candidate.dataType != dmr.DTPIHeader && current.dataType == dmr.DTPIHeader {
		return false
	}
	if candidate.bptcErrors != current.bptcErrors {
		return candidate.bptcErrors >= 0 && (current.bptcErrors < 0 || candidate.bptcErrors < current.bptcErrors)
	}
	return candidate.slotErrors < current.slotErrors
}

type piHeaderInfo struct {
	MFID  uint8  `json:"mfid"`
	ENC   uint8  `json:"enc"`
	ALG   uint8  `json:"alg"`
	Alg   string `json:"algName"`
	KeyID uint8  `json:"keyId"`
	MI    string `json:"mi"`
}

func parseDMRPiHeader(info []byte) (piHeaderInfo, bool) {
	if len(info) != 96 {
		return piHeaderInfo{}, false
	}
	packed := packBits(info)
	if len(packed) != 12 {
		return piHeaderInfo{}, false
	}
	// DMR PI uses CCITT init 0, final complement and the 0x6969 mask.
	// A low BPTC correction count alone cannot validate the cipher MI.
	expected := framing.CRCCCITTWithInit(packed[:10], 0) ^ 0xffff
	received := (uint16(packed[10])<<8 | uint16(packed[11])) ^ 0x6969
	if received != expected {
		return piHeaderInfo{}, false
	}
	algRaw := packed[0]
	alg := algRaw & 0x07
	pi := piHeaderInfo{
		MFID:  packed[1],
		ALG:   alg,
		ENC:   algRaw >> 4,
		KeyID: packed[2],
		MI:    fmt.Sprintf("%08X", uint32(packed[3])<<24|uint32(packed[4])<<16|uint32(packed[5])<<8|uint32(packed[6])),
	}
	pi.Alg = dmrPrivacyAlgName(pi.ALG)
	return pi, pi.MFID != 0 || pi.ALG != 0 || pi.ENC != 0 || pi.KeyID != 0 || pi.MI != "00000000"
}

func dmrPrivacyAlgName(alg uint8) string {
	switch alg {
	case 0:
		return "BP"
	case 1:
		return "RC4"
	case 2:
		return "DES56"
	case 4:
		return "AES128"
	case 5:
		return "AES256"
	default:
		return fmt.Sprintf("ALG%d", alg)
	}
}

func readBits(bits []byte, start, count int) uint64 {
	var value uint64
	for i := 0; i < count; i++ {
		value <<= 1
		idx := start + i
		if idx >= 0 && idx < len(bits) {
			value |= uint64(bits[idx] & 1)
		}
	}
	return value
}

func summarizeSuperframe(sf dmrvoice.VoiceSuperframe, voc voice.Vocoder, audioConn net.Conn, privacy *privacyState, timeslot int, timeslotSource string, cach *cachSummary, corrected []string) outputPacket {
	out := outputPacket{
		Type:       "dmr.voice.superframe",
		StartDibit: sf.StartDibit,
		Phase:      sf.Phase,
		Timeslot:   normalizeTimeslot(timeslot),
		TSSource:   strings.TrimSpace(timeslotSource),
		Sync:       sf.SyncName,
		HasLC:      sf.HasLC,
		HasRC:      sf.HasRC,
		Frames:     len(sf.Frames),
		FecErrors:  make([]int, 0, len(sf.Frames)),
	}
	if out.TSSource == "" && out.Timeslot > 0 {
		out.TSSource = "input"
	}
	if cach != nil {
		copied := *cach
		out.CACH = &copied
	}
	appliedPrivacyFrames := 0
	if sf.HasLC {
		lc := lcSummary{
			FLCO:    sf.LC.FLCO.String(),
			FID:     sf.LC.FID,
			Service: sf.LC.ServiceOptions,
			Dst:     sf.LC.DstAddr,
			Src:     sf.LC.SrcAddr,
		}
		if gv, ok := sf.LC.AsGroupVoiceUser(); ok {
			lc.Group = gv.GroupAddress
			lc.Encrypted = gv.Encrypted
			lc.Emergency = gv.Emergency
			if privacy != nil {
				privacy.UpdateEncryptionState(gv.Encrypted)
			}
		}
		out.LC = &lc
	}
	if sf.HasRC {
		out.RcInfo = sf.RC.Info
	}

	decodedInfos := make([][]byte, len(sf.Frames))
	for idx, frame := range sf.Frames {
		info, fecErrs, err := dmrvoice.DecodeAMBEFrame(frame)
		if idx < len(corrected) && corrected[idx] != "" {
			if nativeInfo, nativeErr := bitsFromHexMSB(corrected[idx], 49); nativeErr == nil {
				info, fecErrs, err = nativeInfo, 0, nil
			}
		}
		out.FecErrors = append(out.FecErrors, fecErrs)
		if err != nil {
			out.BadFrames++
			continue
		}
		decodedInfos[idx] = append([]byte(nil), info...)
	}
	rawVoc := voc
	rawAudioConn := audioConn
	for _, rawInfo := range decodedInfos {
		if len(rawInfo) == 0 {
			if privacy != nil {
				privacy.SkipVoiceFrame()
			}
			continue
		}
		info := append([]byte(nil), rawInfo...)
		countPrivacyFrame := privacy != nil
		if privacy != nil {
			if privacy.ShouldMuteUnsupportedAudio() {
				privacy.MarkMutedFrame()
				privacy.MarkVoiceFrameSeen()
				continue
			}
			if privacy.ApplyBits(info) {
				appliedPrivacyFrames++
			} else if privacy.ShouldMuteEncryptedAudio() {
				privacy.MarkMutedFrame()
				privacy.MarkVoiceFrameSeen()
				continue
			}
		}
		if rawVoc == nil || rawAudioConn == nil {
			if countPrivacyFrame {
				privacy.MarkVoiceFrameSeen()
			}
			continue
		}
		pcm, err := rawVoc.Decode(packBits(info))
		if err != nil {
			out.BadFrames++
			if countPrivacyFrame {
				privacy.MarkVoiceFrameSeen()
			}
			continue
		}
		if len(pcm) > 0 {
			_, _ = rawAudioConn.Write(pcm16le(pcm))
			out.AudioFrames++
		}
		if countPrivacyFrame {
			privacy.MarkVoiceFrameSeen()
		}
	}
	if privacy != nil {
		out.Privacy = privacy.Summary(appliedPrivacyFrames)
		// The MI carried by A-F becomes valid after F, for the next superframe.
		// Never overwrite the current PI/LFSR context before decoding its voice.
		privacy.UpdateLateEntry(sf.Frames)
		if out.Privacy != nil {
			if privacy.lateKnown && privacy.lateCRCOk {
				out.Privacy.NextMI = fmt.Sprintf("%08X", privacy.lateMI)
			}
			out.PrivacyText = out.Privacy.Compact()
		}
	}
	return out
}

func summarizeCorrectedAMBEPacket(pkt inputPacket, base int, voc voice.Vocoder, audioConn net.Conn, privacy *privacyState) outputPacket {
	out := outputPacket{
		Type:       "dmr.voice.corrected",
		StartDibit: base,
		Timeslot:   normalizeTimeslot(pkt.Timeslot),
		TSSource:   strings.TrimSpace(pkt.TSSource),
		Sync:       "Fobos-corrected",
		Frames:     len(pkt.AMBE49),
		FecErrors:  make([]int, 0, len(pkt.AMBE49)),
	}
	if out.TSSource == "" && out.Timeslot > 0 {
		out.TSSource = "input"
	}
	if pkt.CACH != nil {
		copied := *pkt.CACH
		out.CACH = &copied
	}

	appliedPrivacyFrames := 0
	for _, payloadHex := range pkt.AMBE49 {
		info, err := bitsFromHexMSB(payloadHex, 49)
		if err != nil {
			out.BadFrames++
			continue
		}
		out.FecErrors = append(out.FecErrors, 0)
		countPrivacyFrame := privacy != nil
		if privacy != nil {
			if privacy.ShouldMuteUnsupportedAudio() {
				privacy.MarkMutedFrame()
				privacy.MarkVoiceFrameSeen()
				continue
			}
			if privacy.ApplyBits(info) {
				appliedPrivacyFrames++
			} else if privacy.ShouldMuteEncryptedAudio() {
				privacy.MarkMutedFrame()
				privacy.MarkVoiceFrameSeen()
				continue
			}
		}
		if voc == nil || audioConn == nil {
			if countPrivacyFrame {
				privacy.MarkVoiceFrameSeen()
			}
			continue
		}
		pcm, err := voc.Decode(packBits(info))
		if err != nil {
			out.BadFrames++
			if countPrivacyFrame {
				privacy.MarkVoiceFrameSeen()
			}
			continue
		}
		if len(pcm) > 0 {
			_, _ = audioConn.Write(pcm16le(pcm))
			out.AudioFrames++
		}
		if countPrivacyFrame {
			privacy.MarkVoiceFrameSeen()
		}
	}
	if privacy != nil {
		out.Privacy = privacy.Summary(appliedPrivacyFrames)
		if out.Privacy != nil {
			out.PrivacyText = out.Privacy.Compact()
		}
	}
	return out
}

func envInt(name string, fallback int) int {
	value := strings.TrimSpace(os.Getenv(name))
	if value == "" {
		return fallback
	}
	var parsed int
	if _, err := fmt.Sscanf(value, "%d", &parsed); err != nil {
		return fallback
	}
	return parsed
}

func normalizeTimeslot(timeslot int) int {
	if timeslot == 1 || timeslot == 2 {
		return timeslot
	}
	return 0
}

type privacyState struct {
	mode            string
	variant         string
	layout          string
	frameOffset     int
	keyID           int
	keyHex          string
	key             []byte
	err             string
	rc4             *gtrc4.Cipher
	aesStream       cipher.Stream
	sync            bool
	knownEncrypted  bool
	encrypted       bool
	totalFrames     int
	changedFrames   int
	mutedFrames     int
	voiceFrames     int
	skippedFrames   int
	rc4StartFrame   int
	rc4DropBytes    int
	rc4FrameIndex   int
	rc4BaseMI       uint32
	lateMI          uint32
	lateCRCOk       bool
	lateCRCExt      uint8
	lateCRCCmp      uint8
	lateRaw         string
	lateSource      string
	lateCandidates  string
	lateKnown       bool
	rc4MI           uint32
	rc4MIValid      bool
	pendingLateMI   uint32
	pendingLateHits int
}

func newPrivacyState() *privacyState {
	return &privacyState{mode: "none"}
}

func (p *privacyState) Configure(cfg privacyConfig) {
	mode := normalizePrivacyMode(cfg.Mode)
	variant := normalizePrivacyVariant(cfg.Variant)
	if strings.TrimSpace(cfg.Variant) == "" && p.variant != "" {
		variant = p.variant
	}
	layout := normalizePrivacyLayout(cfg.Layout)
	if strings.TrimSpace(cfg.Layout) == "" && p.layout != "" {
		layout = p.layout
	}
	frameOffset := p.frameOffset
	if cfg.FrameOffset != nil {
		frameOffset = normalizeFrameOffset(*cfg.FrameOffset)
	}
	keyHex := cleanHex(cfg.KeyHex)
	keyID := cfg.KeyID
	if mode == p.mode && variant == p.variant && layout == p.layout && frameOffset == p.frameOffset && keyID == p.keyID && keyHex == p.keyHex {
		return
	}
	preservePI := mode == p.mode && keyID == p.keyID && keyHex == p.keyHex && p.lateKnown && p.lateCRCOk
	preservedKnownEncrypted := p.knownEncrypted
	preservedEncrypted := p.encrypted
	preservedLateMI := p.lateMI
	preservedLateCRCExt := p.lateCRCExt
	preservedLateCRCCmp := p.lateCRCCmp
	preservedLateRaw := p.lateRaw
	preservedLateSource := p.lateSource
	preservedLateCandidates := p.lateCandidates

	p.mode = mode
	p.variant = variant
	p.layout = layout
	p.frameOffset = frameOffset
	p.keyID = keyID
	p.keyHex = keyHex
	p.key = nil
	p.err = ""
	p.rc4 = nil
	p.aesStream = nil
	p.sync = false
	p.knownEncrypted = false
	p.encrypted = false
	p.totalFrames = 0
	p.changedFrames = 0
	p.mutedFrames = 0
	p.voiceFrames = 0
	p.skippedFrames = 0
	p.rc4StartFrame = 0
	p.rc4DropBytes = 0
	p.rc4FrameIndex = 0
	p.rc4BaseMI = 0
	p.lateKnown = false
	p.lateMI = 0
	p.lateCRCOk = false
	p.lateCRCExt = 0
	p.lateCRCCmp = 0
	p.lateRaw = ""
	p.lateSource = ""
	p.lateCandidates = ""
	p.rc4MI = 0
	p.rc4MIValid = false
	p.pendingLateMI = 0
	p.pendingLateHits = 0
	if mode == "none" {
		return
	}
	if keyHex == "" {
		p.err = "empty key"
		return
	}
	key, err := hex.DecodeString(keyHex)
	if err != nil {
		p.err = "bad hex key"
		return
	}
	p.key = key
	p.resetCipher()
	if preservePI && p.err == "" {
		p.knownEncrypted = preservedKnownEncrypted
		p.encrypted = preservedEncrypted
		p.lateKnown = true
		p.lateMI = preservedLateMI
		p.lateCRCOk = true
		p.lateCRCExt = preservedLateCRCExt
		p.lateCRCCmp = preservedLateCRCCmp
		p.lateRaw = preservedLateRaw
		p.lateSource = preservedLateSource + "/retune"
		p.lateCandidates = preservedLateCandidates
		p.resetCipherPositionForPI()
	}
}

func (p *privacyState) resetCipher() {
	p.err = ""
	p.rc4 = nil
	p.aesStream = nil
	switch p.mode {
	case "arc4":
		if len(p.key) != 5 {
			p.err = "DMR ARC4/RC4 key must be 5 bytes"
			return
		}
	case "aes256":
		if len(p.key) != 32 {
			p.err = "AES-256 key must be 32 bytes"
			return
		}
		p.err = "AES-256 DMR privacy sync is not implemented"
		return
	case "hytera-bp":
		p.err = "Hytera Basic beta requires DSD-neo; this adapter does not implement it"
		return
	default:
		p.err = "unsupported privacy mode"
	}
}

func (p *privacyState) resetCipherPositionForPI() {
	p.rc4 = nil
	p.aesStream = nil
	p.sync = false
	p.rc4MI = 0
	p.rc4MIValid = false
	p.totalFrames = 0
	p.changedFrames = 0
	p.mutedFrames = 0
	p.voiceFrames = 0
	p.skippedFrames = 0
	p.rc4StartFrame = 0
	p.rc4DropBytes = 0
	p.rc4FrameIndex = 0
	p.rc4BaseMI = 0
}

func (p *privacyState) ResetCallState() {
	if p == nil {
		return
	}
	p.knownEncrypted = false
	p.encrypted = false
	if p.mode != "none" && len(p.key) > 0 {
		p.resetCipher()
	}
	p.sync = false
	p.totalFrames = 0
	p.changedFrames = 0
	p.lateKnown = false
	p.lateMI = 0
	p.lateCRCOk = false
	p.lateCRCExt = 0
	p.lateCRCCmp = 0
	p.lateRaw = ""
	p.lateSource = ""
	p.lateCandidates = ""
	p.rc4MI = 0
	p.rc4MIValid = false
	p.mutedFrames = 0
	p.voiceFrames = 0
	p.skippedFrames = 0
	p.rc4StartFrame = 0
	p.rc4DropBytes = 0
	p.rc4FrameIndex = 0
	p.rc4BaseMI = 0
	p.pendingLateMI = 0
	p.pendingLateHits = 0
}

func (p *privacyState) UpdateEncryptionState(encrypted bool) {
	if p == nil {
		return
	}
	p.knownEncrypted = true
	p.encrypted = encrypted
}

func (p *privacyState) UpdatePIHeader(pi piHeaderInfo, bptcErrors int) (bool, string) {
	if p == nil {
		return false, "no privacy state"
	}
	if bptcErrors < 0 {
		return false, "bptc decode failed"
	}
	if bptcErrors > 2 {
		return false, fmt.Sprintf("bptc errors %d > 2", bptcErrors)
	}
	var mi uint32
	if _, err := fmt.Sscanf(pi.MI, "%X", &mi); err != nil {
		return false, "bad MI"
	}

	expectedAlg := uint8(0)
	switch p.mode {
	case "arc4":
		expectedAlg = 1
	case "aes256":
		expectedAlg = 5
	default:
		return false, "privacy mode disabled"
	}
	if pi.ALG != expectedAlg {
		return false, fmt.Sprintf("alg %s != selected %s", dmrPrivacyAlgName(pi.ALG), p.mode)
	}
	if p.keyID != 0 && p.keyID != int(pi.KeyID) {
		return false, fmt.Sprintf("key %02X != selected %02X", pi.KeyID, p.keyID)
	}

	resetPosition := p.lateSource != "pi-header" || p.lateMI != mi || p.voiceFrames > 0 || p.rc4 != nil

	p.knownEncrypted = true
	p.encrypted = pi.ALG != 0 || pi.ENC != 0
	p.lateKnown = true
	p.lateMI = mi
	p.lateCRCExt = 0
	p.lateCRCCmp = 0
	p.lateRaw = fmt.Sprintf("mfid=%02X alg=%02X enc=%X key=%02X mi=%08X",
		pi.MFID, pi.ALG, pi.ENC, pi.KeyID, mi)
	p.lateSource = "pi-header"
	p.lateCandidates = p.lateRaw
	p.pendingLateMI = 0
	p.pendingLateHits = 0
	if p.mode == "arc4" {
		p.lateCRCOk = true
		if resetPosition {
			p.resetCipherPositionForPI()
		}
		return true, ""
	}
	return false, "selected privacy mode not implemented"
}

func (p *privacyState) ApplyBits(bits []byte) bool {
	if p == nil || p.mode == "none" || p.err != "" || len(p.key) == 0 {
		return false
	}
	if !p.knownEncrypted || !p.encrypted {
		return false
	}
	cipherBits := privacyLayoutToCipherBits(bits, p.layout)
	payload := packBits(cipherBits)
	before := append([]byte(nil), payload...)
	switch p.mode {
	case "arc4":
		if !p.applyDMRARC4Frame(payload) {
			return false
		}
	case "aes256":
		if p.aesStream == nil {
			return false
		}
		p.aesStream.XORKeyStream(payload, payload)
	default:
		return false
	}
	plainCipherBits := make([]byte, len(cipherBits))
	unpackBitsInto(payload, plainCipherBits)
	plainBits := privacyLayoutFromCipherBits(plainCipherBits, p.layout)
	copy(bits, plainBits)
	p.totalFrames++
	if !bytes.Equal(before, payload) {
		p.changedFrames++
	}
	return true
}

func (p *privacyState) ShouldMuteEncryptedAudio() bool {
	if p == nil || !p.knownEncrypted || !p.encrypted {
		return false
	}
	if p.mode == "none" || p.err != "" || len(p.key) == 0 {
		return true
	}
	switch p.mode {
	case "arc4":
		return !p.lateKnown || !p.lateCRCOk
	case "aes256":
		return p.aesStream == nil
	default:
		return true
	}
}

func (p *privacyState) ShouldSuppressRawEncryptedAudio() bool {
	if p == nil || p.mode == "none" || p.err != "" || len(p.key) == 0 {
		return false
	}
	return p.knownEncrypted && p.encrypted
}

func (p *privacyState) ShouldUseCorrectedEncryptedAudio() bool {
	if p == nil || p.mode == "none" || p.err != "" || len(p.key) == 0 {
		return false
	}
	if !p.knownEncrypted || !p.encrypted {
		return false
	}
	switch p.mode {
	case "arc4":
		return p.lateKnown && p.lateCRCOk
	case "aes256":
		return p.aesStream != nil
	default:
		return false
	}
}

func (p *privacyState) ShouldMuteUnsupportedAudio() bool {
	if p == nil || p.mode == "none" {
		return false
	}
	if p.err != "" {
		return true
	}
	if len(p.key) == 0 {
		return false
	}
	switch p.mode {
	case "arc4":
		return false
	case "aes256":
		return p.aesStream == nil
	default:
		return true
	}
}

func (p *privacyState) MarkMutedFrame() {
	if p != nil {
		p.mutedFrames++
	}
}

func (p *privacyState) MarkVoiceFrameSeen() {
	if p != nil {
		p.voiceFrames++
	}
}

func (p *privacyState) ensureRC4ForLateMI() bool {
	if p == nil || p.mode != "arc4" || p.err != "" || len(p.key) != 5 {
		return false
	}
	if !p.lateKnown || !p.lateCRCOk {
		return false
	}
	if p.rc4MIValid {
		return true
	}
	p.rc4MI = p.lateMI
	p.rc4BaseMI = p.lateMI
	p.rc4MIValid = true
	p.rc4FrameIndex = 0
	p.rc4StartFrame = p.voiceFrames
	p.rc4DropBytes = 256
	p.sync = true
	return true
}

func (p *privacyState) applyDMRARC4Frame(payload []byte) bool {
	if !p.ensureRC4ForLateMI() {
		return false
	}
	absoluteFrame := p.rc4FrameIndex + p.frameOffset
	frameInSuperframe := absoluteFrame % 18
	superframeIndex := absoluteFrame / 18
	mi := p.rc4BaseMI
	if p.variant != "fixed-mi" {
		mi = dmrArc4AdvanceMI(mi, superframeIndex)
	}
	advanceFrame := func() {
		p.rc4FrameIndex++
		p.rc4MI = mi
		nextSuperframe := (p.rc4FrameIndex + p.frameOffset) / 18
		if p.variant != "fixed-mi" {
			p.lateMI = dmrArc4AdvanceMI(p.rc4BaseMI, nextSuperframe)
			p.lateSource = "dmra-lfsr"
		}
	}
	effectiveKey := make([]byte, 9)
	key := append([]byte(nil), p.key...)
	if p.variant == "key-reverse" || p.variant == "key-mi-reverse" {
		reverseBytes(key)
	}
	copy(effectiveKey[:5], key)
	miBytes := []byte{
		byte(mi >> 24),
		byte(mi >> 16),
		byte(mi >> 8),
		byte(mi),
	}
	if p.variant == "mi-reverse" || p.variant == "key-mi-reverse" {
		reverseBytes(miBytes)
	}
	copy(effectiveKey[5:], miBytes)
	if shouldSkipDMRARC4Payload(payload) {
		dropBytes := 256 + frameInSuperframe*7
		if p.variant == "drop0" {
			dropBytes = frameInSuperframe * 7
		}
		p.rc4DropBytes = dropBytes
		p.skippedFrames++
		advanceFrame()
		return true
	}
	c, err := gtrc4.NewCipher(effectiveKey)
	if err != nil {
		p.err = err.Error()
		return false
	}
	// DSD-FME treats DMRA ARC4 voice as 18 packed AMBE frames per
	// superframe: drop 256 bytes, consume 7 bytes per 49-bit AMBE frame,
	// then advance the 32-bit MI with the DMRA LFSR for the next
	// superframe.
	dropBytes := 256 + frameInSuperframe*7
	if p.variant == "drop0" {
		dropBytes = frameInSuperframe * 7
	}
	c.KeyStream(dropBytes)
	c.XORKeyStream(payload, payload)
	p.rc4DropBytes = dropBytes
	advanceFrame()
	return true
}

func dmrArc4AdvanceMI(mi uint32, count int) uint32 {
	for i := 0; i < count; i++ {
		mi = dmrArc4NextMI(mi)
	}
	return mi
}

func dmrArc4NextMI(mi uint32) uint32 {
	// One superframe advances the x^32+x^4+x^2+1 recurrence by 32 bits.
	for bit := 0; bit < 32; bit++ {
		feedback := ((mi >> 31) ^ (mi >> 3) ^ (mi >> 1)) & 1
		mi = (mi << 1) | feedback
	}
	return mi
}

func shouldSkipDMRARC4Payload(payload []byte) bool {
	bits := make([]byte, 49)
	unpackBitsInto(payload, bits)
	if bytes.Equal(bits, dmrAmbeSilenceBits()) {
		return true
	}
	for i := 24; i < 44 && i < len(bits); i++ {
		if bits[i] != 0 {
			return false
		}
	}
	return len(bits) >= 44
}

func dmrAmbeSilenceBits() []byte {
	const silence uint64 = 0xF801A99F8CE080
	out := make([]byte, 49)
	for i := 0; i < 49; i++ {
		out[i] = byte((silence >> uint(55-i)) & 1)
	}
	return out
}

func (p *privacyState) UpdateLateEntry(frames [dmrvoice.FramesPerSuperframe][]byte) {
	if p == nil {
		return
	}
	best, candidates := bestLateEntryCandidate(frames)
	if !best.known {
		return
	}
	if !trustedLateEntryCandidate(best) {
		if p.lateCRCOk {
			p.lateCandidates = candidates
			return
		}
		p.lateKnown = true
		p.lateMI = best.mi
		p.lateCRCOk = false
		p.lateCRCExt = best.crcExt
		p.lateCRCCmp = best.crcCmp
		p.lateRaw = best.raw
		p.lateSource = best.source
		p.lateCandidates = candidates
		p.rc4 = nil
		p.rc4MIValid = false
		return
	}

	if p.lateCRCOk {
		if best.mi == p.lateMI {
			p.lateCandidates = candidates
			return
		}
	}

	// A-F ordering is verified before entry here. The CRC/Golay-verified MI
	// describes the next superframe; it changes each time, so waiting for two
	// identical values prevents late entry from ever acquiring normal calls.
	p.resetCipherPositionForPI()

	p.lateKnown = true
	p.lateMI = best.mi
	p.lateCRCOk = best.crcOK
	p.lateCRCExt = best.crcExt
	p.lateCRCCmp = best.crcCmp
	p.lateRaw = best.raw
	p.lateSource = best.source
	p.lateCandidates = candidates
	if p.mode == "arc4" && !p.knownEncrypted && len(p.key) == 5 {
		p.UpdateEncryptionState(true)
	}
}

type lateNibbleMap struct {
	name string
	idx  [4]int
}

type lateEntryOrder struct {
	name   string
	burst  [3]int
	frame  [3]int
	column bool
}

type lateEntryCandidate struct {
	source      string
	raw         string
	mi          uint32
	crcExt      uint8
	crcCmp      uint8
	crcOK       bool
	known       bool
	golayErrors int
}

var lateNibbleMaps = []lateNibbleMap{
	{name: "48-45", idx: [4]int{48, 47, 46, 45}},
	{name: "45-48", idx: [4]int{45, 46, 47, 48}},
	{name: "35-38", idx: [4]int{35, 36, 37, 38}},
	{name: "38-35", idx: [4]int{38, 37, 36, 35}},
	{name: "0-3", idx: [4]int{0, 1, 2, 3}},
	{name: "3-0", idx: [4]int{3, 2, 1, 0}},
}

var lateEntryOrders = []lateEntryOrder{
	{name: "dsd", burst: [3]int{1, 2, 3}, frame: [3]int{0, 1, 2}, column: true},
	{name: "burst", burst: [3]int{1, 2, 3}, frame: [3]int{0, 1, 2}, column: false},
	{name: "dsd-rb", burst: [3]int{3, 2, 1}, frame: [3]int{0, 1, 2}, column: true},
	{name: "burst-rb", burst: [3]int{3, 2, 1}, frame: [3]int{0, 1, 2}, column: false},
	{name: "dsd-rf", burst: [3]int{1, 2, 3}, frame: [3]int{2, 1, 0}, column: true},
	{name: "burst-rf", burst: [3]int{1, 2, 3}, frame: [3]int{2, 1, 0}, column: false},
}

const (
	maxTrustedLateEntryGolayErrors = 6
	minConfirmedLateEntryHits      = 2
)

func bestLateEntryCandidate(frames [dmrvoice.FramesPerSuperframe][]byte) (lateEntryCandidate, string) {
	cand, candidates := lateEntryCandidateFromRawFrames(frames)
	if !cand.known {
		return cand, ""
	}
	if candidates != "" {
		return cand, candidates
	}
	ok := "-"
	if cand.crcOK {
		ok = "ok"
	}
	return cand, fmt.Sprintf("%s=%08X/%X-%X/%s,golay=%d",
		cand.source, cand.mi, cand.crcExt, cand.crcCmp, ok, cand.golayErrors)
}

var (
	ambeRW = [36]int{
		0, 1, 0, 1, 0, 1,
		0, 1, 0, 1, 0, 1,
		0, 1, 0, 1, 0, 1,
		0, 1, 0, 1, 0, 2,
		0, 2, 0, 2, 0, 2,
		0, 2, 0, 2, 0, 2,
	}
	ambeRX = [36]int{
		23, 10, 22, 9, 21, 8,
		20, 7, 19, 6, 18, 5,
		17, 4, 16, 3, 15, 2,
		14, 1, 13, 0, 12, 10,
		11, 9, 10, 8, 9, 7,
		8, 6, 7, 5, 6, 4,
	}
	ambeRY = [36]int{
		0, 2, 0, 2, 0, 2,
		0, 2, 0, 3, 0, 3,
		1, 3, 1, 3, 1, 3,
		1, 3, 1, 3, 1, 3,
		1, 3, 1, 3, 1, 3,
		1, 3, 1, 3, 1, 3,
	}
	ambeRZ = [36]int{
		5, 3, 4, 2, 3, 1,
		2, 0, 1, 13, 0, 12,
		22, 11, 21, 10, 20, 9,
		19, 8, 18, 7, 17, 6,
		16, 5, 15, 4, 14, 3,
		13, 2, 12, 1, 11, 0,
	}
)

func lateEntryCandidateFromRawFrames(frames [dmrvoice.FramesPerSuperframe][]byte) (lateEntryCandidate, string) {
	var frag [7][3]uint64
	for burst := 0; burst < 6; burst++ {
		for frame := 0; frame < 3; frame++ {
			raw := frames[burst*3+frame]
			if len(raw) != 72 {
				return lateEntryCandidate{source: "raw72"}, ""
			}
			frag[burst+1][frame] = uint64(lateEntryNibbleFromRawFrame(raw, 0))
		}
	}
	return decodeRawLateEntryCandidate(frag, "raw72/c3-0/s0"), ""
}

func betterLateEntryCandidate(cand, best lateEntryCandidate) bool {
	if !cand.known {
		return false
	}
	if !best.known {
		return true
	}
	candTrusted := trustedLateEntryCandidate(cand)
	bestTrusted := trustedLateEntryCandidate(best)
	if candTrusted != bestTrusted {
		return candTrusted
	}
	if cand.crcOK != best.crcOK {
		return cand.crcOK
	}
	if cand.golayErrors != best.golayErrors {
		return cand.golayErrors < best.golayErrors
	}
	return cand.source < best.source
}

func trustedLateEntryCandidate(cand lateEntryCandidate) bool {
	return cand.known && cand.crcOK && cand.golayErrors <= maxTrustedLateEntryGolayErrors
}

func decodeRawLateEntryCandidate(frag [7][3]uint64, source string) lateEntryCandidate {
	miTest := (frag[1][0] << 32) | (frag[2][0] << 28) | (frag[3][0] << 24) |
		(frag[1][1] << 20) | (frag[2][1] << 16) | (frag[3][1] << 12) |
		(frag[1][2] << 8) | (frag[2][2] << 4) | frag[3][2]
	goTest := (frag[4][0] << 32) | (frag[5][0] << 28) | (frag[6][0] << 24) |
		(frag[4][1] << 20) | (frag[5][1] << 16) | (frag[6][1] << 12) |
		(frag[4][2] << 8) | (frag[5][2] << 4) | frag[6][2]

	var miCorrected uint64
	totalGolayErrors := 0
	for block := 0; block < 3; block++ {
		var cw uint32
		for i := 0; i < 12; i++ {
			cw = (cw << 1) | uint32(bitFrom36(miTest, block*12+i))
		}
		for i := 0; i < 12; i++ {
			cw = (cw << 1) | uint32(bitFrom36(goTest, block*12+i))
		}
		data, errs := dmrGolayDecode(cw)
		if errs < 0 {
			return lateEntryCandidate{
				source: source,
				raw:    lateEntryRawString(frag),
			}
		}
		totalGolayErrors += errs
		miCorrected = (miCorrected << 12) | uint64(data&0x0fff)
	}

	var miBits [36]uint8
	for i := 0; i < 36; i++ {
		miBits[i] = bitFrom36(miCorrected, i)
	}
	crcExt := uint8(miCorrected & 0xf)
	crcCmp := dmrCRC4(miBits[:32])
	return lateEntryCandidate{
		source:      source,
		raw:         lateEntryRawString(frag),
		mi:          uint32((miCorrected >> 4) & 0xffffffff),
		crcExt:      crcExt,
		crcCmp:      crcCmp,
		crcOK:       crcExt == crcCmp,
		known:       true,
		golayErrors: totalGolayErrors,
	}
}

func bitFrom36(value uint64, index int) uint8 {
	if index < 0 || index >= 36 {
		return 0
	}
	return uint8((value >> uint(35-index)) & 1)
}

func lateEntryNibbleFromRawFrame(frame []byte, mode int) uint8 {
	var fr [4][24]uint8
	for i := 0; i < 36; i++ {
		fr[ambeRW[i]][ambeRX[i]] = frame[2*i] & 1
		fr[ambeRY[i]][ambeRZ[i]] = frame[2*i+1] & 1
	}
	var value uint8
	for _, bitIdx := range lateEntryNibbleModeIndexes(mode) {
		value = (value << 1) | (fr[3][bitIdx] & 1)
	}
	return value
}

func lateEntryNibbleModeName(mode int) string {
	switch mode {
	case 0:
		return "c3-0"
	case 1:
		return "c3-0r"
	case 2:
		return "c3-13"
	case 3:
		return "c3-13r"
	default:
		return "c3"
	}
}

func lateEntryNibbleModeIndexes(mode int) [4]int {
	switch mode {
	case 1:
		return [4]int{3, 2, 1, 0}
	case 2:
		return [4]int{13, 12, 11, 10}
	case 3:
		return [4]int{10, 11, 12, 13}
	default:
		return [4]int{0, 1, 2, 3}
	}
}

func lateEntryRawString(frag [7][3]uint64) string {
	return fmt.Sprintf("%X%X%X:%X%X%X:%X%X%X:%X%X%X:%X%X%X:%X%X%X",
		frag[1][0], frag[1][1], frag[1][2],
		frag[2][0], frag[2][1], frag[2][2],
		frag[3][0], frag[3][1], frag[3][2],
		frag[4][0], frag[4][1], frag[4][2],
		frag[5][0], frag[5][1], frag[5][2],
		frag[6][0], frag[6][1], frag[6][2])
}

func buildLateEntryCandidate(frag [7][3]uint64, source string, order lateEntryOrder) lateEntryCandidate {
	var nibbles [9]uint64
	pos := 0
	if order.column {
		for _, frame := range order.frame {
			for _, burst := range order.burst {
				nibbles[pos] = frag[burst][frame]
				pos++
			}
		}
	} else {
		for _, burst := range order.burst {
			for _, frame := range order.frame {
				nibbles[pos] = frag[burst][frame]
				pos++
			}
		}
	}
	var miTest uint64
	for _, nibble := range nibbles {
		miTest = (miTest << 4) | (nibble & 0xf)
	}
	mi := uint32((miTest >> 4) & 0xffffffff)
	var miBits [36]uint8
	for i := 0; i < 36; i++ {
		miBits[i] = uint8((miTest >> uint(35-i)) & 1)
	}
	crcExt := uint8(miTest & 0xf)
	crcCmp := dmrCRC4(miBits[:32])
	return lateEntryCandidate{
		source: source,
		raw: fmt.Sprintf("%X%X%X:%X%X%X:%X%X%X:%X%X%X:%X%X%X:%X%X%X",
			frag[1][0], frag[1][1], frag[1][2],
			frag[2][0], frag[2][1], frag[2][2],
			frag[3][0], frag[3][1], frag[3][2],
			frag[4][0], frag[4][1], frag[4][2],
			frag[5][0], frag[5][1], frag[5][2],
			frag[6][0], frag[6][1], frag[6][2]),
		mi:     mi,
		crcExt: crcExt,
		crcCmp: crcCmp,
		crcOK:  crcExt == crcCmp,
		known:  true,
	}
}

func lateEntryNibble(info []byte, idx [4]int) uint8 {
	var value uint8
	for _, bitIdx := range idx {
		value <<= 1
		if bitIdx >= 0 && bitIdx < len(info) {
			value |= info[bitIdx] & 1
		}
	}
	return value
}

func dmrCRC4(bits []uint8) uint8 {
	const k = 4
	poly := [5]uint8{1, 0, 0, 1, 1}
	buf := make([]uint8, len(bits)+k)
	copy(buf, bits)
	for i := range bits {
		if buf[i] == 0 {
			continue
		}
		for j := 0; j <= k; j++ {
			buf[i+j] ^= poly[j]
		}
	}
	var crc uint8
	for i := 0; i < k; i++ {
		crc = (crc << 1) | (buf[len(bits)+i] & 1)
	}
	return crc ^ 0xf
}

func (p *privacyState) Summary(appliedFrames int) *privacySummary {
	if p == nil || (p.mode == "none" && !p.knownEncrypted) {
		return nil
	}
	summary := &privacySummary{
		Mode:          p.mode,
		Variant:       p.variant,
		Layout:        p.layout,
		FrameOffset:   p.frameOffset,
		KeyID:         p.keyID,
		KeySet:        len(p.key) > 0 && p.err == "",
		Applied:       appliedFrames > 0,
		Sync:          p.sync,
		Encrypted:     p.encrypted,
		Known:         p.knownEncrypted,
		Error:         p.err,
		FrameCount:    p.totalFrames,
		Changed:       p.changedFrames > 0,
		ChangedFrames: p.changedFrames,
		Muted:         p.mutedFrames > 0,
		MutedFrames:   p.mutedFrames,
		VoiceFrames:   p.voiceFrames,
		SkippedFrames: p.skippedFrames,
		RC4StartFrame: p.rc4StartFrame,
		RC4DropBytes:  p.rc4DropBytes,
	}
	if p.lateKnown {
		summary.LateEntryMI = fmt.Sprintf("%08X", p.lateMI)
		summary.LateEntryCRCOk = p.lateCRCOk
		summary.LateEntryCRCExt = int(p.lateCRCExt)
		summary.LateEntryCRCCmp = int(p.lateCRCCmp)
		summary.LateEntryRaw = p.lateRaw
		summary.LateEntrySource = p.lateSource
		summary.LateEntryCands = p.lateCandidates
	}
	if p.rc4MIValid {
		summary.VoiceMI = fmt.Sprintf("%08X", p.rc4MI)
	}
	return summary
}

func (p *privacySummary) Compact() string {
	if p == nil {
		return ""
	}
	return fmt.Sprintf("mode=%s var=%s layout=%s off=%d applied=%t sync=%t enc=%t known=%t muted=%t frames=%d changed=%d skipped=%d drop=%d mi=%s src=%s err=%s",
		p.Mode,
		p.Variant,
		p.Layout,
		p.FrameOffset,
		p.Applied,
		p.Sync,
		p.Encrypted,
		p.Known,
		p.Muted,
		p.FrameCount,
		p.ChangedFrames,
		p.SkippedFrames,
		p.RC4DropBytes,
		p.LateEntryMI,
		p.LateEntrySource,
		p.Error)
}

func normalizePrivacyMode(mode string) string {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "arc4", "rc4":
		return "arc4"
	case "aes", "aes256", "aes-256":
		return "aes256"
	case "hytera-bp", "hytera-basic":
		return "hytera-bp"
	default:
		return "none"
	}
}

func normalizePrivacyVariant(variant string) string {
	switch strings.ToLower(strings.TrimSpace(variant)) {
	case "key-reverse", "keyrev", "reverse-key":
		return "key-reverse"
	case "mi-reverse", "mirev", "reverse-mi":
		return "mi-reverse"
	case "key-mi-reverse", "mi-key-reverse", "both-reverse", "reverse-both":
		return "key-mi-reverse"
	case "fixed-mi", "no-lfsr", "nolfsr":
		return "fixed-mi"
	case "drop0", "drop-zero", "no-drop":
		return "drop0"
	default:
		return "dmra"
	}
}

func normalizePrivacyLayout(layout string) string {
	switch strings.ToLower(strings.TrimSpace(layout)) {
	case "reverse49", "reverse-49", "bit-reverse-all", "reverse-all":
		return "reverse49"
	case "bit-reverse-bytes", "reverse-byte-bits", "lsb-bytes":
		return "bit-reverse-bytes"
	case "byte-reverse", "reverse-bytes":
		return "byte-reverse"
	default:
		return "normal"
	}
}

func normalizeFrameOffset(offset int) int {
	offset %= 18
	if offset < 0 {
		offset += 18
	}
	return offset
}

func privacyLayoutToCipherBits(bits []byte, layout string) []byte {
	out := append([]byte(nil), bits...)
	switch layout {
	case "reverse49":
		reverseBytes(out)
	case "bit-reverse-bytes":
		reverseBitGroups(out, 8)
	case "byte-reverse":
		reverseByteGroups(out)
	}
	return out
}

func privacyLayoutFromCipherBits(bits []byte, layout string) []byte {
	// The test layouts are involutions: applying the same transform again
	// maps decrypted cipher-order bits back to the vocoder bit order.
	return privacyLayoutToCipherBits(bits, layout)
}

func reverseBitGroups(bits []byte, group int) {
	if group <= 1 {
		return
	}
	for start := 0; start < len(bits); start += group {
		end := start + group
		if end > len(bits) {
			end = len(bits)
		}
		for i, j := start, end-1; i < j; i, j = i+1, j-1 {
			bits[i], bits[j] = bits[j], bits[i]
		}
	}
}

func reverseByteGroups(bits []byte) {
	const group = 8
	groups := (len(bits) + group - 1) / group
	tmp := append([]byte(nil), bits...)
	for g := 0; g < groups; g++ {
		srcGroup := groups - 1 - g
		srcStart := srcGroup * group
		srcEnd := srcStart + group
		if srcEnd > len(bits) {
			srcEnd = len(bits)
		}
		dstStart := g * group
		dstEnd := dstStart + (srcEnd - srcStart)
		if dstStart >= len(bits) {
			break
		}
		copy(bits[dstStart:dstEnd], tmp[srcStart:srcEnd])
	}
}

func reverseBytes(v []byte) {
	for i, j := 0, len(v)-1; i < j; i, j = i+1, j-1 {
		v[i], v[j] = v[j], v[i]
	}
}

func cleanHex(value string) string {
	s := strings.ToLower(strings.TrimSpace(value))
	s = strings.ReplaceAll(s, "0x", "")
	replacer := strings.NewReplacer(" ", "", "\t", "", "\r", "", "\n", "", ":", "", "-", "", "_", "", ",", "")
	return replacer.Replace(s)
}

func bitsFromHexMSB(value string, bitCount int) ([]byte, error) {
	cleaned := cleanHex(value)
	if bitCount <= 0 {
		return nil, fmt.Errorf("bad bit count %d", bitCount)
	}
	if len(cleaned)*4 < bitCount {
		return nil, fmt.Errorf("hex payload has %d bits, need %d", len(cleaned)*4, bitCount)
	}
	out := make([]byte, bitCount)
	for i := 0; i < bitCount; i++ {
		nibble := cleaned[i/4]
		var v byte
		switch {
		case nibble >= '0' && nibble <= '9':
			v = nibble - '0'
		case nibble >= 'a' && nibble <= 'f':
			v = 10 + nibble - 'a'
		default:
			return nil, fmt.Errorf("bad hex digit %q", nibble)
		}
		out[i] = (v >> uint(3-(i&3))) & 1
	}
	return out, nil
}

func packBits(bits []byte) []byte {
	out := make([]byte, (len(bits)+7)/8)
	for i, bit := range bits {
		if bit&1 != 0 {
			out[i>>3] |= 1 << uint(7-(i&7))
		}
	}
	return out
}

func unpackBitsInto(packed []byte, bits []byte) {
	for i := range bits {
		if (packed[i>>3] & (1 << uint(7-(i&7)))) != 0 {
			bits[i] = 1
		} else {
			bits[i] = 0
		}
	}
}

func pcm16le(samples []int16) []byte {
	out := make([]byte, len(samples)*2)
	for i, s := range samples {
		binary.LittleEndian.PutUint16(out[i*2:], uint16(s))
	}
	return out
}
