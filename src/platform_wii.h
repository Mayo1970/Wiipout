#ifndef PLATFORM_WII_H
#define PLATFORM_WII_H

#include <ogc/video.h>

GXRModeObj *platform_wii_video_mode(void);
void *platform_wii_xfb(int index);
void platform_wii_console_detach(void);
void platform_wii_log_sync(void);
void platform_wii_mem_log(const char *tag);

#endif
