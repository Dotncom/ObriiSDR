package main

import (
	"crypto/aes"
	"crypto/cipher"
	stdrc4 "crypto/rc4"
	"encoding/hex"
	"encoding/json"
	"os"
	"testing"

	"github.com/MattCheramie/GopherTrunk/internal/radio/framing"
)

// Independent standard-library ciphers and the upstream BPTC encoder check
// the native C++ path. All keys here are public synthetic test values.
func TestExportNativePrivacyVectors(t *testing.T) {
	path := os.Getenv("OBRII_NATIVE_VECTORS")
	if path == "" {
		t.Skip("optional native cross-language regression")
	}
	type superframe struct {
		Cipher []string
		Raw    []string
	}
	type vector struct {
		Mode   int
		Key    string
		PI     string
		Frames []superframe
	}
	var vectors []vector
	for _, mode := range []int{1, 2} {
		key := []byte{1, 2, 3, 4, 5}
		alg := byte(1)
		if mode == 2 {
			key = make([]byte, 32)
			for i := range key {
				key[i] = byte(i)
			}
			alg = 5
		}
		const initialMI = uint32(0x12345678)
		pi := []byte{0x20 | alg, 0x10, 7, 0x12, 0x34, 0x56, 0x78, 0, 0, 0, 0, 0}
		crc := framing.CRCCCITTWithInit(pi[:10], 0) ^ 0xffff ^ 0x6969
		pi[10], pi[11] = byte(crc>>8), byte(crc)
		bits := make([]byte, 96)
		for i := range bits {
			bits[i] = (pi[i/8] >> uint(7-i%8)) & 1
		}
		coded := framing.EncodeBPTC196_96(bits)
		burst := make([]byte, 264)
		copy(burst[:98], coded[:98])
		copy(burst[166:], coded[98:])
		dibits := make([]byte, 132)
		for i := range dibits {
			dibits[i] = '0' + (burst[2*i]<<1 | burst[2*i+1])
		}
		v := vector{Mode: mode, Key: hex.EncodeToString(key), PI: string(dibits)}
		mi := initialMI
		for sf := 0; sf < 3; sf++ {
			var next uint32
			stream := make([]byte, 126)
			if mode == 1 {
				seed := append(append([]byte{}, key...), byte(mi>>24), byte(mi>>16), byte(mi>>8), byte(mi))
				c, _ := stdrc4.NewCipher(seed)
				discard := make([]byte, 256)
				c.XORKeyStream(discard, discard)
				c.XORKeyStream(stream, stream)
				next = dmrArc4NextMI(mi)
			} else {
				iv := make([]byte, 16)
				iv[0], iv[1], iv[2], iv[3] = byte(mi>>24), byte(mi>>16), byte(mi>>8), byte(mi)
				state := mi
				for bit := 32; bit < 128; bit++ {
					f := ((state >> 31) ^ (state >> 21) ^ (state >> 1) ^ state) & 1
					state = state<<1 | f
					iv[bit/8] |= byte(f << uint(7-bit%8))
				}
				next = uint32(iv[4])<<24 | uint32(iv[5])<<16 | uint32(iv[6])<<8 | uint32(iv[7])
				block, _ := aes.NewCipher(key)
				ofb := cipher.NewOFB(block, iv)
				discard := make([]byte, 16)
				ofb.XORKeyStream(discard, discard)
				ofb.XORKeyStream(stream, stream)
			}
			raw := lateEntryFrames(next)
			record := superframe{}
			for i := 0; i < 18; i++ {
				plain := []byte{0x82, 0x7e, 0x93, 0xd4, 0x85, 0x76, 0x80}
				for b := range plain {
					plain[b] ^= stream[i*7+b]
				}
				plain[6] &= 0x80
				record.Cipher = append(record.Cipher, hex.EncodeToString(plain))
				record.Raw = append(record.Raw, hex.EncodeToString(packBits(raw[i])))
			}
			v.Frames = append(v.Frames, record)
			mi = next
		}
		vectors = append(vectors, v)
	}
	data, err := json.MarshalIndent(vectors, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
}
