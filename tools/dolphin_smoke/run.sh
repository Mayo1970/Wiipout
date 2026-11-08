#!/bin/sh
# Boot the smoke build in Dolphin with an SD image that holds wipeout/, then check the SD log.
# Usage: tools/dolphin_smoke/run.sh [path/to/Dolphin.exe]. SMOKE_TIMEOUT sets the limit in seconds.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DOLPHIN="${1:-${DOLPHIN:-Dolphin.exe}}"
TIMEOUT="${SMOKE_TIMEOUT:-300}"
OUT="$ROOT/build/wii-smoke/dolphin"
IMG=build/wii-smoke/dolphin/sd.raw
CTR=wpo-smoke-mtools

"$ROOT/tools/docker-make.sh" PLATFORM=WII WII_SMOKE=1
rm -rf "$OUT"
mkdir -p "$OUT/user/Config"
printf '[Analytics]\nPermissionAsked = True\nEnabled = False\n' > "$OUT/user/Config/Dolphin.ini"

docker rm -f "$CTR" >/dev/null 2>&1 || true
MSYS_NO_PATHCONV=1 docker run -d --name "$CTR" -e DEBIAN_FRONTEND=noninteractive -v "$ROOT:/src" -w /src debian:bookworm sleep infinity >/dev/null
trap 'docker rm -f "$CTR" >/dev/null 2>&1 || true' EXIT
# Superfloppy FAT32, the same layout Dolphin uses for its own SD image.
MSYS_NO_PATHCONV=1 docker exec "$CTR" sh -ec "
apt-get update -qq
apt-get install -y -qq --no-install-recommends mtools dosfstools >/dev/null 2>&1
truncate -s 512M $IMG
mkfs.vfat -F 32 $IMG >/dev/null
mmd -i $IMG ::/apps ::/apps/wiipeout
mcopy -s -i $IMG wipeout ::/apps/wiipeout/
"

# Dolphin locks the image while it runs; the smoke build powers off when done, which ends batch mode.
WIN_OUT="$(cygpath -w "$OUT")"
"$DOLPHIN" -u "$WIN_OUT\\user" -b \
	-C "Dolphin.Core.WiiSDCard=True" \
	-C "Dolphin.Core.WiiSDCardEnableFolderSync=False" \
	-C "Dolphin.General.WiiSDCardPath=$WIN_OUT\\sd.raw" \
	-e "$(cygpath -w "$ROOT/build/wii-smoke/boot.dol")" >/dev/null 2>&1 &
PID=$!
elapsed=0
while kill -0 "$PID" 2>/dev/null && [ "$elapsed" -lt "$TIMEOUT" ]; do
	sleep 5
	elapsed=$((elapsed + 5))
done
if kill -0 "$PID" 2>/dev/null; then
	echo "smoke: Dolphin still running after ${TIMEOUT}s, killing it"
	WINPID="$(cat /proc/$PID/winpid 2>/dev/null || true)"
	if [ -n "$WINPID" ]; then taskkill //F //PID "$WINPID" >/dev/null 2>&1 || true; else kill "$PID" 2>/dev/null || true; fi
	sleep 2
fi

MSYS_NO_PATHCONV=1 docker exec "$CTR" mtype -i $IMG ::/apps/wiipeout/wiipeout.log > "$OUT/wiipeout.log" || true
cat "$OUT/wiipeout.log"
if grep -qi "error" "$OUT/wiipeout.log"; then
	echo "smoke: FAIL (log has errors)"
	exit 1
fi
if ! grep -q "SMOKE PASS" "$OUT/wiipeout.log"; then
	echo "smoke: FAIL (no PASS line)"
	exit 1
fi
echo "smoke: PASS"
