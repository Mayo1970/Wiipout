#ifndef PLATFORM_WII_H
#define PLATFORM_WII_H

#include <stdbool.h>
#include <stdio.h>
#include <ogc/video.h>

GXRModeObj *platform_wii_video_mode(void);
bool platform_wii_widescreen(void);
void *platform_wii_xfb(int index);
void platform_wii_console_detach(void);
void platform_wii_log_sync(void);
void platform_wii_mem_log(const char *tag);
FILE *platform_wii_music_load(FILE *file);

// Main loop phases; the WII_DIAG watchdog shows the last one when the loop stops.
enum {
	WII_PHASE_INPUT = 1,
	WII_PHASE_UPDATE,
	WII_PHASE_FLUSH_WAIT,
	WII_PHASE_TEXTURE_WAIT,
	WII_PHASE_FRAME_COPY,
	WII_PHASE_FRAME_WAIT,
	WII_PHASE_FRAME_FLIP,
	WII_PHASE_AUDIO,
};

#ifdef WII_DIAG
extern volatile u32 wii_diag_phase;
extern u32 wii_diag_dropped_tris;
#define WII_DIAG_PHASE(p) (wii_diag_phase = (p))
#else
#define WII_DIAG_PHASE(p) ((void)0)
#endif

#endif
