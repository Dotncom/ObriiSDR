# ITU HF Source Provenance

Upstream: https://github.com/ITU-R-Study-Group-3/ITU-R-HF
Commit: `82017594a1c6cacfaa7e86954c4ae7b3a5825a3d`

Only the C/header files under P533/Src/P533, P372/Src/P372,
ITURHFProp/Src/ITURHFProp, the upstream README, and two small published
input/output fixture pairs are vendored. Their bytes are unmodified.
`tools/itu_hf_fetch.ps1` verifies sizes and Git blob SHA-1 against the pinned
public tree. No upstream executables, DLLs, antenna collections, redundant
data trees, or Git checkout are downloaded.

The complete official free-use/no-warranty notice is preserved at the end of
README.md and in ITURHFProp.c/P533.c. It is not an SPDX/MIT/BSD license; do not
relabel it. Some upstream headers also contain older 2018 all-rights-reserved
notices, preserved verbatim. Distributors should retain both and seek legal
review if those historical notices require clarification.

Model data is fetched from this same commit's P533/Data into a private cache
by default: ionos01..12.bin, COEFF01..12W.txt, and
P1239-3 Decile Factors.txt. Each month costs about 11.43 MB; all 12 cost
about 137.27 MB (130.91 MiB). Data may instead be placed in a portable
bundle by passing -DataRoot. Neither data nor private requests are included
in this source directory by default.

The isolated CMake build compiles fresh x64 P533.dll and P372.dll plus
itu_hf_helper.exe on Windows, or native libp533.so, libp372.so and itu_hf_helper
on Linux. A separate launcher restricts DLL lookup/preloads exact sibling
SONAMEs and lowers process priority; CMake renames the upstream main at compile
time. Linux compiler options supply the missing errno.h declaration and old
common-global linkage. There are no changes to propagation equations or to
vendored source files. See docs/hf_prediction.md for verified test status;
Linux ELF cross-compilation passed, native Linux execution remains unverified.
