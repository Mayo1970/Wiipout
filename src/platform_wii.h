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

#endif
