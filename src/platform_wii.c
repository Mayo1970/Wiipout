#define WII_MAIN_STACK_BYTES (256 * 1024)
static unsigned char stack[WII_MAIN_STACK_BYTES] __attribute__((aligned(8)));
void *__ppc_main_sp __attribute__((section(".sdata"))) = &stack[sizeof(stack)];

#include <stdint.h>
#include <malloc.h>
#include <ogc/console.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <wiiuse/wpad.h>

#include "platform.h"
#include "platform_wii.h"
#include "mem.h"
#include "utils.h"

static GXRModeObj *video_mode;
static char *path_assets = "";
static char *path_userdata = "";
static char temp_path[512];

void platform_exit(void) {
	exit(0);
}

vec2i_t platform_screen_size(void) {
	return video_mode ? vec2i(video_mode->fbWidth, video_mode->xfbHeight) : vec2i(640, 480);
}

double platform_now(void) {
	return (double)ticks_to_millisecs(gettime()) / 1000.0;
}

bool platform_get_fullscreen(void) {
	return false;
}

void platform_set_fullscreen(bool fullscreen) {
	(void)fullscreen;
}

void platform_set_audio_mix_cb(void (*cb)(float *buffer, uint32_t len)) {
	(void)cb;
}

static char *platform_path(const char *root, const char *name) {
	snprintf(temp_path, sizeof(temp_path), "%s%s", root, name);
	return temp_path;
}

bool platform_asset_exists(const char *name) {
	return file_exists(platform_path(path_assets, name));
}

FILE *platform_open_asset(const char *name, const char *mode) {
	return fopen(platform_path(path_assets, name), mode);
}

uint8_t *platform_load_asset(const char *name, uint32_t *bytes_read) {
	return file_load(platform_path(path_assets, name), bytes_read);
}

uint8_t *platform_load_userdata(const char *name, uint32_t *bytes_read) {
	char *path = platform_path(path_userdata, name);
	if (!file_exists(path)) {
		*bytes_read = 0;
		return NULL;
	}
	return file_load(path, bytes_read);
}

uint32_t platform_store_userdata(const char *name, void *bytes, int32_t len) {
	return file_store(platform_path(path_userdata, name), bytes, len);
}

static void platform_video_init(void) {
	VIDEO_Init();
	WPAD_Init();
	video_mode = VIDEO_GetPreferredMode(NULL);
	void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
	console_init(xfb, 20, 20, video_mode->fbWidth, video_mode->xfbHeight, video_mode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(video_mode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (video_mode->viTVMode & VI_NON_INTERLACE) {
		VIDEO_WaitVSync();
	}
}

void platform_wii_mem_log(const char *tag) {
	mem_stats_t s;
	mem_get_stats(&s);
	unsigned long mem1 = (uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo();
	unsigned long mem2 = (uintptr_t)SYS_GetArena2Hi() - (uintptr_t)SYS_GetArena2Lo();
	unsigned long heap = mallinfo().fordblks;
	static unsigned long low = ~0UL;
	if (mem1 + heap < low) low = mem1 + heap;
	printf("mem %s: bump=%lu temp=%lu bump_peak=%lu temp_peak=%lu total_peak=%lu hunk=%lu heap_free=%lu mem1_arena=%lu mem2_arena=%lu free_low=%lu\n",
		tag, (unsigned long)s.bump, (unsigned long)s.temp, (unsigned long)s.bump_peak,
		(unsigned long)s.temp_peak, (unsigned long)s.total_peak, (unsigned long)s.hunk_size,
		heap, mem1, mem2, low);
}

int main(int argc, char *argv[]) {
	uintptr_t hi = (uintptr_t)SYS_GetArena2Hi();
	uintptr_t lo = (uintptr_t)SYS_GetArena2Lo();
	uintptr_t base = hi >= MEM_HUNK_BYTES ? (hi - MEM_HUNK_BYTES) & ~(uintptr_t)31 : 0;
	if (base < lo || base == 0) {
		platform_video_init();
		printf("ERROR: MEM2 arena is too small for the 16 MB hunk.\n");
		while (true) VIDEO_WaitVSync();
	}
	SYS_SetArena2Hi((void *)base);
	mem_init((void *)base);
	(void)argc;
	(void)argv;

	#ifdef PATH_ASSETS
		path_assets = TOSTRING(PATH_ASSETS);
	#endif
	#ifdef PATH_USERDATA
		path_userdata = TOSTRING(PATH_USERDATA);
	#endif

	platform_video_init();
	platform_wii_mem_log("boot");
	printf("Wiipeout platform stub is running. Press HOME to return to the Homebrew Channel.\n");

	while (true) {
		WPAD_ScanPads();
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) {
			exit(0);
		}
		VIDEO_WaitVSync();
	}

	return 0;
}
