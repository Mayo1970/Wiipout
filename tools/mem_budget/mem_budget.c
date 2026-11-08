#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../../src/platform.h"
#include "../../src/mem.h"
#include "../../src/system.h"
#include "../../src/utils.h"
#include "../../src/wipeout/game.h"
#include <qoa.h>

#define render_size null_render_size
#define render_texture_create null_texture_create
#define render_texture_size null_texture_size
#define render_textures_len null_textures_len
#define render_textures_reset null_textures_reset
#include "../../src/render_null.c"
#undef render_size
#undef render_texture_create
#undef render_texture_size
#undef render_textures_len
#undef render_textures_reset

vec2i_t render_size(void) { return vec2i(640, 480); }
static vec2i_t textures[65536];
static uint16_t texture_count;
uint16_t render_texture_create(uint32_t w, uint32_t h, rgba_t *pixels) {
	(void)pixels;
	if (texture_count == UINT16_MAX) { fprintf(stderr, "too many textures\n"); exit(1); }
	textures[texture_count] = vec2i(w, h);
	return texture_count++;
}
vec2i_t render_texture_size(uint16_t i) { return textures[i]; }
uint16_t render_textures_len(void) { return texture_count; }
void render_textures_reset(uint16_t n) { texture_count = n; }
static uint64_t texture_pixels(void) {
	uint64_t n = 0;
	for (unsigned i = 0; i < texture_count; i++) n += (uint64_t)textures[i].x * textures[i].y;
	return n;
}

void platform_exit(void) { exit(0); }
vec2i_t platform_screen_size(void) { return vec2i(640, 480); }
double platform_now(void) { static double t; return t += 1.0 / 60.0; }
bool platform_get_fullscreen(void) { return false; }
void platform_set_fullscreen(bool fullscreen) { (void)fullscreen; }
void platform_set_audio_mix_cb(void (*cb)(float *, uint32_t)) { (void)cb; }
bool platform_asset_exists(const char *name) { return file_exists(name); }
FILE *platform_open_asset(const char *name, const char *mode) { return fopen(name, mode); }
uint8_t *platform_load_asset(const char *name, uint32_t *size) { return file_load(name, size); }
uint8_t *platform_load_userdata(const char *name, uint32_t *size) { (void)name; *size = 0; return NULL; }
uint32_t platform_store_userdata(const char *name, void *bytes, int32_t len) { (void)name; (void)bytes; return len; }

/* Wrapping only calls from the linked game objects avoids counting libc internals. */
typedef union { struct { size_t size; } value; long double alignment; } alloc_header;
void *__real_malloc(size_t n);
void __real_free(void *p);
static size_t alloc_live, alloc_peak;
void *__wrap_malloc(size_t n) {
	alloc_header *h = __real_malloc(sizeof(*h) + n);
	if (!h) return NULL;
	h->value.size = n; alloc_live += n;
	if (alloc_live > alloc_peak) alloc_peak = alloc_live;
	return h + 1;
}
void __wrap_free(void *p) {
	if (p) { alloc_header *h = (alloc_header *)p - 1; alloc_live -= h->value.size; __real_free(h); }
}
void *__wrap_realloc(void *p, size_t n) {
	if (!p) return __wrap_malloc(n);
	if (!n) { __wrap_free(p); return NULL; }
	alloc_header *h = (alloc_header *)p - 1;
	void *q = __wrap_malloc(n);
	if (q) { memcpy(q, p, h->value.size < n ? h->value.size : n); __wrap_free(p); }
	return q;
}
static unsigned long sfx_bytes(void) {
	FILE *f = fopen("wipeout/sound/wipeout.vb", "rb");
	if (!f) { perror("wipeout.vb"); exit(1); }
	fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f);
	return (unsigned long)(n / 16) * 28 * sizeof(int16_t);
}
static void checkpoint(const char *tag) {
	mem_stats_t s; mem_get_stats(&s);
	unsigned long long pixels = texture_pixels();
	printf("BUDGET %s bump=%u temp=%u bump_peak=%u temp_peak=%u total_peak=%u hunk=%u textures=%u rgba8888=%llu rgb16=%llu sfx_samples=%lu music_buffers=%lu malloc_peak=%lu\n",
		tag, s.bump, s.temp, s.bump_peak, s.temp_peak, s.total_peak, s.hunk_size,
		texture_count, pixels * 4, pixels * 2, sfx_bytes(),
		(unsigned long)(QOA_FRAME_SIZE(2, QOA_SLICES_PER_FRAME) + 2 * QOA_FRAME_LEN * sizeof(short) * 2), (unsigned long)alloc_peak);
}
static void frames(int n) { while (n--) system_update(); }
static void race(int circuit, int class_id) {
	g.race_class = class_id; g.race_type = RACE_TYPE_SINGLE; g.highscore_tab = HIGHSCORE_TAB_RACE;
	g.team = TEAM_AG_SYSTEMS; g.pilot = def.teams[g.team].pilots[0]; g.circuit = circuit;
	game_set_scene(GAME_SCENE_RACE); frames(60);
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	system_init(); game_set_scene(GAME_SCENE_MAIN_MENU); save.is_dirty = true; frames(3); checkpoint("menu");
	race(CIRCUIT_TERRAMAX, RACE_CLASS_VENOM); checkpoint("track01_venom");
	uint32_t max_hunk = 0; uint64_t max_pixels = 0; int t_c = -1, t_cl = -1;
	for (int c = 0; c < NUM_CIRCUITS; c++) for (int cl = 0; cl < NUM_RACE_CLASSES; cl++) {
		if (!platform_asset_exists(def.circuits[c].settings[cl].path)) { printf("SKIP circuit=%d class=%d path=%s (not installed)\n", c, cl, def.circuits[c].settings[cl].path); continue; }
		mem_stats_t after; race(c, cl); mem_get_stats(&after);
		if (after.total_peak > max_hunk) max_hunk = after.total_peak;
		uint64_t pixels = texture_pixels(); if (pixels > max_pixels) { max_pixels = pixels; t_c = c; t_cl = cl; }
		char tag[64]; snprintf(tag, sizeof(tag), "circuit_%d_class_%d", c, cl); checkpoint(tag);
	}
	printf("MAX cumulative_hunk_peak=%u texture_rgba8888=%llu texture_rgb16=%llu circuit=%d class=%d\n", max_hunk, (unsigned long long)max_pixels * 4, (unsigned long long)max_pixels * 2, t_c, t_cl);
	printf("INTRO skipped: decoder installs an external audio callback that outlives a forced headless scene switch.\n");
	system_cleanup(); return 0;
}
