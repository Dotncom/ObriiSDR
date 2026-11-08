# Native BPSK31 receive

Digital Audio -> BPSK31 decodes the selected demodulated audio channel. Use USB
on a conventional PSK31 band, put the carrier at the configured audio tone,
enable digital decoding, and adjust PSK selectivity. AFC follows small offsets
(up to +/-7.5 Hz); it is not a wide-band carrier search. Latin-1 preserves the
traditional 8-bit Varicode alphabet; UTF-8 is available for compatible peers.
This is BPSK31, not QPSK31 or PSK63.

The receiver adapts the mature gMFSK implementation by Tomi Manninen OH2BNS,
including Peter Martinez G3PLX's filters, differential decisions, symbol timing
and Varicode. Original source: https://github.com/rubund/gmfsk/tree/master/src/psk31
License: GPL-2.0-or-later; attribution is retained in the source.

Processing preserves fractional resampling and odd PCM-byte boundaries. State
resets when the sample rate or tone changes, not on every audio block. AFC and
selectivity can change live. No process, network service or extra installation
is required. Status is limited to two updates per second.

ObriiPsk31SelfTest checks the complete unique Varicode table, synthesized text
at 8/44.1/48 kHz, small frequency errors, weak signals, random byte boundaries,
silence and reset. Real RF interoperability still needs user testing.
