#ifndef HUD_H
#define HUD_H

#include "ship.h"
#include "camera.h"

void hud_load(void);
void hud_draw(ship_t *ship, camera_t *camera, const vec3_t *target_projection, int view);

#endif
