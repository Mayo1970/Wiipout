#define WII_MAIN_STACK_BYTES (256 * 1024)
static unsigned char stack[WII_MAIN_STACK_BYTES] __attribute__((aligned(8)));
void *__ppc_main_sp __attribute__((section(".sdata"))) = &stack[sizeof(stack)];

#include <stdint.h>
#include <malloc.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dvm.h>
#include <sdcard/wiisd_io.h>
#include <ogc/usbstorage.h>
#include <ogc/conf.h>
#include <ogc/console.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <wiiuse/wpad.h>

#include "platform.h"
#include "platform_wii.h"
#include "system.h"
#include "mem.h"
#include "utils.h"

#ifdef WII_SMOKE
#include <string.h>
#include "input.h"
#endif

static GXRModeObj *video_mode;
static bool video_widescreen;
static void *video_xfb[2];
static bool console_detached;
static char *path_assets = "";
static char *path_userdata = "";
static char temp_path[1024];
static bool wants_to_exit;
static volatile bool power_requested;
static volatile bool reset_requested;
static u64 start_tick;
static FILE *log_file;
static bool log_writing;
static const devoptab_t *console_stdout;
static const devoptab_t *console_stderr;
static devoptab_t log_stdout;
static devoptab_t log_stderr;

#ifdef WII_SMOKE
static u32 smoke_frame;
static bool smoke_menu_seen;
static bool smoke_race_seen;
static u32 smoke_race_frame;
#endif

static ssize_t platform_log_write(const devoptab_t *console, struct _reent *reent, void *fd, const char *buffer, size_t len) {
	ssize_t result = !console_detached && console->write_r ? console->write_r(reent, fd, buffer, len) : (ssize_t)len;
	if (log_file && !log_writing) {
		log_writing = true;
		fwrite(buffer, 1, len, log_file);
		log_writing = false;
	}
	return result;
}

static ssize_t platform_log_write_stdout(struct _reent *reent, void *fd, const char *buffer, size_t len) {
	return platform_log_write(console_stdout, reent, fd, buffer, len);
}

static ssize_t platform_log_write_stderr(struct _reent *reent, void *fd, const char *buffer, size_t len) {
	return platform_log_write(console_stderr, reent, fd, buffer, len);
}

GXRModeObj *platform_wii_video_mode(void) {
	return video_mode;
}

void *platform_wii_xfb(int index) {
	return video_xfb[index];
}

bool platform_wii_widescreen(void) {
	return video_widescreen;
}

void platform_wii_console_detach(void) {
	fflush(stdout);
	fflush(stderr);
	console_detached = true;
}

void platform_wii_log_sync(void) {
	if (log_file) {
		fflush(stdout);
		fflush(stderr);
		fflush(log_file);
		fsync(fileno(log_file));
	}
}

static void platform_log_close(void) {
	if (log_file) {
		platform_wii_log_sync();
		fclose(log_file);
		log_file = NULL;
	}
}

static void platform_log_init(bool sd_mounted) {
	console_stdout = devoptab_list[STD_OUT];
	console_stderr = devoptab_list[STD_ERR];
	log_stdout = *console_stdout;
	log_stderr = *console_stderr;
	log_stdout.write_r = platform_log_write_stdout;
	log_stderr.write_r = platform_log_write_stderr;
	devoptab_list[STD_OUT] = &log_stdout;
	devoptab_list[STD_ERR] = &log_stderr;
	if (sd_mounted) {
		mkdir("sd:/apps/wiipeout", 0777);
		log_file = fopen("sd:/apps/wiipeout/wiipeout.log", "w");
	}
	atexit(platform_log_close);
}

static void platform_power_callback(void) {
	power_requested = true;
}

static void platform_reset_callback(u32 irq, void *context) {
	(void)irq;
	(void)context;
	reset_requested = true;
}

#ifdef WII_SMOKE
static void platform_smoke_update(void) {
	smoke_frame++;
	// RETURN confirms menu pages; tapping every 30 frames makes each press a new edge.
	// The release is always sent so RETURN is never left held when the race loads.
	if (!smoke_race_seen && smoke_frame % 30 == 0) input_set_button_state(INPUT_KEY_RETURN, 1.0f);
	if (smoke_frame % 30 == 1) input_set_button_state(INPUT_KEY_RETURN, 0.0f);
	if (smoke_race_seen && smoke_frame - smoke_race_frame >= 300) {
		printf("SMOKE PASS: main menu and race reached, frame %u\n", (unsigned int)smoke_frame);
		platform_wii_log_sync();
		// Power off, not exit: Dolphin then stops emulation and flushes the SD image.
		power_requested = true;
		return;
	}
	// Reached only while the race has not passed yet, so a late race also times out.
	if (smoke_frame >= 60 * 180) {
		printf("SMOKE FAIL: timeout, menu=%d race=%d\n", smoke_menu_seen, smoke_race_seen);
		platform_wii_log_sync();
		power_requested = true;
	}
}
#endif

static void platform_pump_events(void) {
	WPAD_ScanPads();
	if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) {
		platform_exit();
	}
#ifdef WII_SMOKE
	platform_smoke_update();
#endif
}

static void platform_prepare_frame(void) {
}

static void platform_end_frame(void) {
#if defined(RENDERER_NULL)
	VIDEO_WaitVSync();
#endif
}

static bool platform_data_root_exists(const char *path) {
	struct stat info;
	return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

void platform_exit(void) {
	wants_to_exit = true;
}

vec2i_t platform_screen_size(void) {
	if (!video_mode) return vec2i(640, 480);
	int h = video_mode->efbHeight;
	int w = video_widescreen ? (h * 16 + 9 / 2) / 9 : (h * 4 + 3 / 2) / 3;
	return vec2i(w, h);
}

double platform_now(void) {
	return (double)diff_ticks(start_tick, gettime()) / (double)(TB_TIMER_CLOCK * 1000ULL);
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
	FILE *file = fopen(platform_path(path_assets, name), mode);
	platform_wii_log_sync();
	return file;
}

uint8_t *platform_load_asset(const char *name, uint32_t *bytes_read) {
	uint8_t *bytes = file_load(platform_path(path_assets, name), bytes_read);
	platform_wii_log_sync();
	return bytes;
}

uint8_t *platform_load_userdata(const char *name, uint32_t *bytes_read) {
	char *path = platform_path(path_userdata, name);
	if (!file_exists(path)) {
		*bytes_read = 0;
		platform_wii_log_sync();
		return NULL;
	}
	uint8_t *bytes = file_load(path, bytes_read);
	platform_wii_log_sync();
	return bytes;
}

uint32_t platform_store_userdata(const char *name, void *bytes, int32_t len) {
	uint32_t written = file_store(platform_path(path_userdata, name), bytes, len);
	platform_wii_log_sync();
	return written;
}

static void platform_video_init(void) {
	VIDEO_Init();
	video_mode = VIDEO_GetPreferredMode(NULL);
	video_widescreen = CONF_GetAspectRatio() == CONF_ASPECT_16_9;
	video_xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
	video_xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
	console_init(video_xfb[0], 20, 20, video_mode->fbWidth, video_mode->xfbHeight, video_mode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(video_mode);
	VIDEO_SetNextFramebuffer(video_xfb[0]);
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
#ifdef WII_SMOKE
	if (strcmp(tag, "menu") == 0) smoke_menu_seen = true;
	if (strcmp(tag, "race") == 0 && smoke_menu_seen) {
		smoke_race_seen = true;
		smoke_race_frame = smoke_frame;
	}
	printf("SMOKE scene %s frame %u\n", tag, (unsigned int)smoke_frame);
#endif
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

	start_tick = gettime();
	platform_video_init();
	WPAD_Init();
	SYS_SetPowerCallback(platform_power_callback);
	SYS_SetResetCallback(platform_reset_callback);

	bool sd_mounted = dvmProbeMountDiscIface("sd", &__io_wiisd, 4, 64);
	platform_log_init(sd_mounted);
	const char *data_root = NULL;
	if (sd_mounted && platform_data_root_exists("sd:/apps/wiipeout/wipeout")) {
		data_root = "sd:/apps/wiipeout/";
	}
#if !defined(PATH_ASSETS) || !defined(PATH_USERDATA)
	if (!data_root) {
		for (int i = 0; i < 20; i++) {
			if (dvmProbeMountDiscIface("usb", &__io_usbstorage, 4, 64)) {
				if (platform_data_root_exists("usb:/apps/wiipeout/wipeout")) {
					data_root = "usb:/apps/wiipeout/";
				}
				break;
			}
			if (i < 19) usleep(150000);
		}
	}
	if (!data_root) {
		printf("ERROR: Data directory not found: sd:/apps/wiipeout/wipeout or usb:/apps/wiipeout/wipeout. Press HOME to exit.\n");
		platform_wii_log_sync();
		while (!wants_to_exit && !power_requested && !reset_requested) {
			platform_pump_events();
			VIDEO_WaitVSync();
		}
		platform_log_close();
		if (power_requested) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
		exit(0);
	}
#endif
#ifdef PATH_ASSETS
	path_assets = TOSTRING(PATH_ASSETS);
#else
	path_assets = (char *)data_root;
#endif
#ifdef PATH_USERDATA
	path_userdata = TOSTRING(PATH_USERDATA);
#else
	path_userdata = (char *)data_root;
#endif
	vec2i_t logical = platform_screen_size();
	printf("Wiipeout: data %s, efb %ux%u, logical %dx%d, tv %u %s, %s\n", data_root ? data_root : path_assets,
		(unsigned int)video_mode->fbWidth, (unsigned int)video_mode->efbHeight, logical.x, logical.y,
		(unsigned int)VIDEO_GetCurrentTvMode(),
		(video_mode->viTVMode & VI_NON_INTERLACE) ? "progressive" : "interlaced",
		video_widescreen ? "16:9" : "4:3");
	platform_wii_mem_log("boot");
	if (!wants_to_exit && !power_requested && !reset_requested) {
		system_init();
		while (!wants_to_exit && !power_requested && !reset_requested) {
			platform_pump_events();
			if (wants_to_exit || power_requested || reset_requested) break;
			platform_prepare_frame();
			system_update();
			platform_end_frame();
		}
		system_cleanup();
	}
	platform_log_close();
	if (power_requested) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	exit(0);
}
