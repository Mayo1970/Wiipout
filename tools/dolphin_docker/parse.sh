#!/bin/sh
set -eu
awk '
/^PROF (window|total) / {
	for (key in v) delete v[key]
	for (i = 3; i <= NF; i++) { split($i, a, "="); v[a[1]] = a[2] }
	if (!v["frames"]) next
	printf "%s frames=%s frame_ms=%s update_ms=%s submit_ms=%s wait_ms=%s", $2, v["frames"], v["frame_avg_ms"], v["update_avg_ms"], v["submit_avg_ms"], v["wait_avg_ms"]
	for (i = 3; i <= NF; i++) {
		split($i, a, "=")
		if (a[1] !~ /^(frames|.*_ms|tris_per_draw|arena_peak)$/) printf " %s/frame=%.3f", a[1], a[2] / v["frames"]
	}
	printf " tris/draw=%s arena_peak=%s\n", v["tris_per_draw"], v["arena_peak"]
}' "$@"
