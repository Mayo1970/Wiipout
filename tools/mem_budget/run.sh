#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MSYS_NO_PATHCONV=1 docker run --rm -e DEBIAN_FRONTEND=noninteractive -v "$ROOT:/src:ro" -w /src debian:bookworm sh -ec '
apt-get update -qq
apt-get install -y -qq --no-install-recommends gcc gcc-multilib libc6-dev
SOURCES=$(sed -n "/^COMMON_SRC =/,/RENDERER_SRC/p" Makefile | sed "1d; /RENDERER_SRC/d; s/\\\\//g")
gcc -m32 -std=gnu99 -O2 -D_FILE_OFFSET_BITS=64 -Isrc/libs tools/mem_budget/mem_budget.c $SOURCES -static -Wl,--wrap=malloc,--wrap=realloc,--wrap=free -lm -o /tmp/mem-budget
/tmp/mem-budget
'
"$ROOT/tools/docker-make.sh" PLATFORM=WII
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src:ro" -w /src devkitpro/devkitppc:20261102 sh -ec '
/opt/devkitpro/devkitPPC/bin/powerpc-eabi-size build/wii/boot.elf
/opt/devkitpro/devkitPPC/bin/powerpc-eabi-nm --size-sort -S build/wii/boot.elf | awk "\$3 ~ /^[bBdD]$/" | tail -n 15
'
