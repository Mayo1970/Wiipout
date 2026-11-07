#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

MSYS_NO_PATHCONV=1 docker run --rm \
	-e DEBIAN_FRONTEND=noninteractive \
	-v "$ROOT:/src:ro" \
	-w /src gcc:14-bookworm sh -ec '
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends gcc-powerpc-linux-gnu libc6-dev-powerpc-cross qemu-user-static

	SOURCES="tools/endian_test/endian_test.c src/wipeout/image.c src/wipeout/track.c src/wipeout/object.c src/types.c src/utils.c"
	CFLAGS="-std=gnu99 -O2 -ffunction-sections -fdata-sections -Isrc/libs"
	gcc $CFLAGS $SOURCES -Wl,--gc-sections -lm -o /tmp/endian-native
	/tmp/endian-native > /tmp/endian-native.out

	powerpc-linux-gnu-gcc $CFLAGS $SOURCES -static -Wl,--gc-sections -lm -o /tmp/endian-powerpc
	qemu-ppc-static -L /usr/powerpc-linux-gnu /tmp/endian-powerpc > /tmp/endian-powerpc.out

	diff -u /tmp/endian-native.out /tmp/endian-powerpc.out
	printf "Native x86-64 and big-endian PowerPC dumps match:\n"
	cat /tmp/endian-native.out
'
