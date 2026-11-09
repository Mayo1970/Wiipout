#!/bin/sh
# Build and profile a Wii smoke run in headless Dolphin. Set SMOKE_FULL=1, CPU_CORE=interpreter|cached|jit, TIMEOUT=seconds, PROFILE_FRAMES=count, or PROFILE_WINDOW=count to override defaults.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
IMAGE=${DOLPHIN_IMAGE:-wiipeout-dolphin:2506}
TIMEOUT=${TIMEOUT:-14400}
PROFILE_FRAMES=${PROFILE_FRAMES:-0}
PROFILE_WINDOW=${PROFILE_WINDOW:-300}
case ${CPU_CORE:-interpreter} in
	interpreter) CORE=0 ;;
	cached) CORE=5 ;;
	jit) CORE=1 ;;
	*) echo "Unknown CPU_CORE: $CPU_CORE" >&2; exit 2 ;;
esac
case $TIMEOUT in *[!0-9]*|'') echo "Invalid TIMEOUT" >&2; exit 2 ;; esac
if [ "${SMOKE_FULL:-0}" = 1 ]; then
	BUILD_DIR=build/wii-smoke-full-profile
else
	BUILD_DIR=build/wii-smoke-profile
fi
rm -f "$ROOT/$BUILD_DIR/obj/src/platform_wii.o"
"$ROOT/tools/docker-make.sh" PLATFORM=WII WII_SMOKE=1 WII_SMOKE_FULL="${SMOKE_FULL:-0}" WII_PROFILE=1 WII_PROFILE_FRAMES="$PROFILE_FRAMES" WII_PROFILE_WINDOW="$PROFILE_WINDOW"
OUT=$ROOT/$BUILD_DIR/dolphin-docker
mkdir -p "$OUT/user/Config"
printf '[Analytics]\nPermissionAsked = True\nEnabled = False\n' > "$OUT/user/Config/Dolphin.ini"
MSYS_NO_PATHCONV=1 docker build -q -t "$IMAGE" "$(cygpath -w "$ROOT/tools/dolphin_docker" 2>/dev/null || echo "$ROOT/tools/dolphin_docker")"
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -w /src "$IMAGE" sh -ec '
	truncate -s 0 "$1/sd.raw"
	truncate -s 512M "$1/sd.raw"
	mkfs.vfat -F 32 "$1/sd.raw" >/dev/null
	mmd -i "$1/sd.raw" ::/apps ::/apps/wiipeout
	mcopy -s -i "$1/sd.raw" wipeout ::/apps/wiipeout/
' sh "$BUILD_DIR/dolphin-docker"
status=0
# Dolphin's memory arena lives in /dev/shm (64 MB by default), and it ignores SIGTERM.
MSYS_NO_PATHCONV=1 docker run --rm --shm-size=4g -v "$ROOT:/src" -w /src "$IMAGE" \
	timeout -s KILL "$TIMEOUT" dolphin-emu-nogui -p headless -u "/src/$BUILD_DIR/dolphin-docker/user" \
	-C "Dolphin.Core.GFXBackend=Null" -C "Dolphin.Core.CPUCore=$CORE" \
	-C "Dolphin.DSP.Backend=No Audio Output" -C "Dolphin.Core.WiiSDCard=True" \
	-C "Dolphin.Core.WiiSDCardEnableFolderSync=False" \
	-C "Dolphin.General.WiiSDCardPath=/src/$BUILD_DIR/dolphin-docker/sd.raw" \
	-e "/src/$BUILD_DIR/boot.dol" || status=$?
MSYS_NO_PATHCONV=1 docker run --rm -v "$ROOT:/src" -w /src "$IMAGE" \
	mtype -i "$BUILD_DIR/dolphin-docker/sd.raw" ::/apps/wiipeout/wiipeout.log > "$OUT/wiipeout.log" || {
	echo "Could not extract wiipeout.log" >&2; exit 1;
}
grep '^PROF ' "$OUT/wiipeout.log" || true
if [ "$status" -ne 0 ]; then echo "Dolphin exited with status $status" >&2; exit "$status"; fi
if ! grep -q 'SMOKE PASS' "$OUT/wiipeout.log"; then echo "Smoke run did not pass" >&2; exit 1; fi
