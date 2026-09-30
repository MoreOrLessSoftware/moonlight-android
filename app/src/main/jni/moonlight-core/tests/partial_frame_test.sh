#!/bin/sh
# Builds and runs partial_frame_test.c on the host. CC defaults to gcc.
set -e
cd "$(dirname "$0")"
C=../moonlight-common-c
OUT=${TMPDIR:-/tmp}/partial_frame_test
"${CC:-gcc}" -std=c11 -O1 -Wall -Wextra -Wno-unused-parameter -DHAS_SOCKLEN_T -DNDEBUG \
    -I$C/src -I$C/enet/include -I$C/nanors -I$C/nanors/deps -I$C/nanors/deps/obl \
    partial_frame_test.c $C/src/RtpVideoQueue.c $C/src/VideoDepacketizer.c $C/src/ByteBuffer.c \
    $C/nanors/rs.c $C/nanors/deps/obl/oblas_common.c $C/nanors/deps/obl/oblas_lite.c \
    -o "$OUT"
"$OUT"
