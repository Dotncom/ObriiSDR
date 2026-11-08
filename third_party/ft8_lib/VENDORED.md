# Vendored ft8_lib encoder and receive-decoder subset

Source: https://github.com/kgoba/ft8_lib

Upstream commit: `9fec6ca39886edbf96f4f5e71edc76da5074e871`

Files required for FT8/FT4 message/tone encoding and the FT4 receiver's
candidate search, soft LDPC and CRC validation are retained. `decode.[ch]` and
`ldpc.[ch]` use the same pinned upstream revision. The decoder's three-symbol
FT8 scratch VLA has an equivalent fixed maximum (512 floats) for MSVC C.
Demos, test recordings, Python utilities and upstream FFT support are omitted;
the receiver builds its waterfall using the project's FFTW dependency.
The original MIT license is preserved in `LICENSE` and in
`licenses/ft8_lib/LICENSE`.
