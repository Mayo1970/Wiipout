#ifndef PLATFORM_WII_H
#define PLATFORM_WII_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <ogc/video.h>

void render_gx_set_2d_scale(float scale);
float render_gx_get_2d_scale(void);

GXRModeObj *platform_wii_video_mode(void);
bool platform_wii_widescreen(void);
void *platform_wii_xfb(int index);
void platform_wii_console_detach(void);
void platform_wii_log_sync(void);
void platform_wii_mem_log(const char *tag);
enum { WII_MUSIC_LOADING, WII_MUSIC_DONE, WII_MUSIC_ERROR };
typedef struct {
	const uint8_t *data;
	uint32_t generation, loaded, total;
	int state;
} wii_music_status_t;
uint32_t platform_wii_music_request(const char *asset_path);
wii_music_status_t platform_wii_music_status(void);

// Main loop phases; the WII_DIAG watchdog shows the last one when the loop stops.
enum {
	WII_PHASE_INPUT = 1,
	WII_PHASE_UPDATE,
	WII_PHASE_FLUSH_WAIT,
	WII_PHASE_TEXTURE_WAIT,
	WII_PHASE_FRAME_COPY,
	WII_PHASE_FRAME_WAIT,
	WII_PHASE_FRAME_FLIP,
	WII_PHASE_VSYNC,
	WII_PHASE_FRAME_END,
};

#ifdef WII_DIAG
extern volatile u32 wii_diag_phase;
extern u32 wii_diag_dropped_tris;
int render_gx_diag_format(char *out, int size);
#define WII_DIAG_PHASE(p) (wii_diag_phase = (p))
#else
#define WII_DIAG_PHASE(p) ((void)0)
#endif

#endif
