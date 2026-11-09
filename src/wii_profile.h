#ifndef WII_PROFILE_H
#define WII_PROFILE_H

#if defined(PLATFORM_WII) && defined(WII_PROFILE)
#include <stdint.h>
#include "platform.h"
typedef struct {
	uint64_t draws, tris, flush_texture, flush_model, flush_state, flush_full, flush_end, flush_other;
	uint64_t arena_waits, arena_peak, pushes, dropped, track_total, track_drawn, track_far, faces;
	uint64_t objects_total, objects_drawn, ships, particles, uploads, replacements;
	double update_ms, submit_ms, wait_ms;
} wii_profile_frame_t;
extern wii_profile_frame_t wii_profile_frame;
void wii_profile_begin(void);
void wii_profile_end(void);
void wii_profile_total(void);
#define PROF_ADD(key, n) (wii_profile_frame.key += (n))
#define PROF_PEAK(key, n) do { if (wii_profile_frame.key < (n)) wii_profile_frame.key = (n); } while (0)
#define PROF_TIME_START(name) double name = platform_now()
#define PROF_TIME_ADD(key, name) PROF_ADD(key, (platform_now() - (name)) * 1000.0)
#define PROF_WAIT_SNAPSHOT(name) double name = wii_profile_frame.wait_ms
#define PROF_WAIT_SUBTRACT(key, name) (wii_profile_frame.key -= wii_profile_frame.wait_ms - (name))
#define PROF_BEGIN() wii_profile_begin()
#define PROF_END() wii_profile_end()
#define PROF_TOTAL() wii_profile_total()
#else
#define PROF_ADD(key, n) ((void)0)
#define PROF_PEAK(key, n) ((void)0)
#define PROF_TIME_START(name) ((void)0)
#define PROF_TIME_ADD(key, name) ((void)0)
#define PROF_WAIT_SNAPSHOT(name) ((void)0)
#define PROF_WAIT_SUBTRACT(key, name) ((void)0)
#define PROF_BEGIN() ((void)0)
#define PROF_END() ((void)0)
#define PROF_TOTAL() ((void)0)
#endif
#endif
