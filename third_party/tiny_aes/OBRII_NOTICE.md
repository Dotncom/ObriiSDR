# Tiny AES

Upstream: https://github.com/kokke/tiny-AES-c
Revision: 23856752fbd139da0b8ca6e471a13d5bcc99a08d
License: Unlicense (public domain); see unlicense.txt.

Unmodified aes.c/aes.h are built as C++ with AES256=1, ECB=1, CBC=0,
CTR=0. Obrii uses only the block cipher to implement DMR AES-256-OFB in
src/digital/dmrprivacycodec.cpp. No additional runtime library is needed.
