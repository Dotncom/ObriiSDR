package main

import "math/bits"

// DMR uses the cyclic Golay generator x^11+x^10+x^6+x^5+x^4+x^2+1,
// followed by an even parity bit. Keep this wire convention local to DMR.
func dmrGolayEncode(data uint16) uint32 {
	d := uint32(data & 0xfff)
	remainder := d << 11
	for bit := 22; bit >= 11; bit-- {
		if remainder&(1<<uint(bit)) != 0 {
			remainder ^= 0xc75 << uint(bit-11)
		}
	}
	word := d<<11 | remainder
	return word<<1 | uint32(bits.OnesCount32(word)&1)
}

var dmrGolayWords = func() [4096]uint32 {
	var words [4096]uint32
	for i := range words {
		words[i] = dmrGolayEncode(uint16(i))
	}
	return words
}()

func dmrGolayDecode(word uint32) (uint16, int) {
	word &= 0xffffff
	for data, candidate := range dmrGolayWords {
		distance := bits.OnesCount32(word ^ candidate)
		if distance <= 3 {
			return uint16(data), distance
		}
	}
	return 0, -1
}
