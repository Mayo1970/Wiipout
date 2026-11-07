#!/bin/sh
# Run make in the pinned devkitPPC container from the repo root. Args go to make.
set -e
IMAGE="${WIIPEOUT_IMAGE:-devkitpro/devkitppc:20261102}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLPATH=/opt/devkitpro/devkitPPC/bin:/opt/devkitpro/tools/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
# MSYS_NO_PATHCONV stops Git Bash from rewriting /src on Windows.
MSYS_NO_PATHCONV=1 exec docker run --rm -e "PATH=$TOOLPATH" -v "$ROOT:/src" -w /src "$IMAGE" make "$@"
