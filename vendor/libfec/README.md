# Integer Reed-Solomon codec

Source: https://github.com/quiet/libfec
Revision: 9750ca0a6d0a786b506e44692776b541f90daa91

The C sources and their included headers are retained unchanged. Compile only
init_rs_int.c, encode_rs_int.c, and decode_rs_int.c with this directory on the
include path and C99 support. Cargo's build.rs performs that build; no network
access or complete libfec installation is needed.

The original copyright notices and GNU LGPL version 2.1 text are in the source
headers and lesser.txt. Distributions must retain these materials and the build
sources needed to rebuild/relink the codec.

The Rust wrapper fixes symbol width to 16, polynomial to 0x1100b, first root to
0, and root step to 1. It validates lengths and erasure positions before entering
C. Encode/decode read the context's tables and use per-call data and scratch;
native pointers remain in a bounded thread-local cache and never cross threads.
The decoder can report success after damage beyond its correction capacity;
the stored pre-existing XXH3-128 checksum remains the acceptance condition.

SHA256SUMS records the exact upstream file bytes. Tests compare parity against
independent polynomial division and cover unknown errors, known erasures, and
post-decoding checksum rejection.
