#define WII_MAIN_STACK_BYTES (256 * 1024)
static unsigned char stack[WII_MAIN_STACK_BYTES] __attribute__((aligned(8)));
void *__ppc_main_sp __attribute__((section(".sdata"))) = &stack[sizeof(stack)];

#include <stdint.h>
#include <math.h>
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
#include <ogc/pad.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <wiiuse/wpad.h>

#include "platform.h"
#include "platform_wii.h"
#include "system.h"
#include "mem.h"
#include "utils.h"
#include "input.h"

#ifdef WII_SMOKE
#include <string.h>
#include "wipeout/game.h"
#include "wipeout/ship.h"
#include "wipeout/ship_ai.h"
#include "wipeout/ship_player.h"
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
static bool smoke_lap_done;
static u32 smoke_lap_frame;
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
	if (smoke_race_seen) {
		ship_t *ship = &g.ships[g.pilot];
		// AI drives the player ship; flags stay untouched so the HUD still draws.
		if (ship->update_func == ship_player_update_race) {
			ship->update_func = ship_ai_update_race;
			printf("SMOKE autopilot frame %u\n", (unsigned int)smoke_frame);
		}
		if (!smoke_lap_done && ship->max_lap >= 1) {
			smoke_lap_done = true;
			smoke_lap_frame = smoke_frame;
			printf("SMOKE lap 1 done frame %u\n", (unsigned int)smoke_frame);
		}
		if (smoke_lap_done && smoke_frame - smoke_lap_frame >= 120) {
			printf("SMOKE PASS: menu, race and first lap reached, frame %u\n", (unsigned int)smoke_frame);
			platform_wii_log_sync();
			// Power off, not exit: Dolphin then stops emulation and flushes the SD image.
			power_requested = true;
			return;
		}
	}
	// Reached only while the race has not passed yet, so a late race also times out.
	if (smoke_frame >= 60 * 360) {
		printf("SMOKE FAIL: timeout, menu=%d race=%d lap=%d\n", smoke_menu_seen, smoke_race_seen,
			smoke_race_seen ? g.ships[g.pilot].max_lap : -1);
		platform_wii_log_sync();
		power_requested = true;
	}
}
#endif

#define WII_PAD_COUNT (INPUT_GAMEPAD_R_STICK_RIGHT - INPUT_GAMEPAD_A + 1)

static void platform_pad_value(float *pad, button_t button, float value) {
	int index = button - INPUT_GAMEPAD_A;
	if (value > pad[index]) pad[index] = value;
}

static void platform_pad_button(float *pad, button_t button, u32 held, u32 mask) {
	if (held & mask) platform_pad_value(pad, button, 1.0f);
}

static void platform_pad_stick(float *pad, float x, float y, bool right) {
	float magnitude = sqrtf(x * x + y * y);
	if (magnitude <= 0.2f) return;
	float scaled = (fminf(magnitude, 1.0f) - 0.2f) / (0.8f * magnitude);
	x *= scaled;
	y *= scaled;
	button_t up = right ? INPUT_GAMEPAD_R_STICK_UP : INPUT_GAMEPAD_L_STICK_UP;
	platform_pad_value(pad, up, y);
	platform_pad_value(pad, up + 1, -y);
	platform_pad_value(pad, up + 2, -x);
	platform_pad_value(pad, up + 3, x);
	if (!right) {
		platform_pad_value(pad, INPUT_GAMEPAD_DPAD_UP, y);
		platform_pad_value(pad, INPUT_GAMEPAD_DPAD_DOWN, -y);
		platform_pad_value(pad, INPUT_GAMEPAD_DPAD_LEFT, -x);
		platform_pad_value(pad, INPUT_GAMEPAD_DPAD_RIGHT, x);
	}
}

static void platform_pad_joystick(float *pad, joystick_t *stick, bool right) {
	float angle = stick->ang * (3.14159265358979323846f / 180.0f);
	platform_pad_stick(pad, sinf(angle) * stick->mag, cosf(angle) * stick->mag, right);
}

static void platform_pump_events(void) {
	static float last[WII_PAD_COUNT];
	static bool cc_format_sent;
	float pad[WII_PAD_COUNT] = {0};
	u32 type;
	WPAD_ScanPads();
	PAD_ScanPads();
	WPADData *data = WPAD_Data(WPAD_CHAN_0);
	bool connected = WPAD_Probe(WPAD_CHAN_0, &type) == WPAD_ERR_NONE && data && data->err == WPAD_ERR_NONE;
	if (connected && type == WPAD_EXP_CLASSIC) {
		if (!cc_format_sent) {
			// Retry the format after the Classic Controller handshake completes.
			WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
			cc_format_sent = true;
		}
	}
	else cc_format_sent = false;
	if (connected) {
		u32 held = data->btns_h;
		if (data->btns_d & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) platform_exit();
		if (type == WPAD_EXP_CLASSIC && data->exp.type == (int)type) {
			platform_pad_button(pad, INPUT_GAMEPAD_DPAD_UP, held, WPAD_CLASSIC_BUTTON_UP);
			platform_pad_button(pad, INPUT_GAMEPAD_DPAD_DOWN, held, WPAD_CLASSIC_BUTTON_DOWN);
			platform_pad_button(pad, INPUT_GAMEPAD_DPAD_LEFT, held, WPAD_CLASSIC_BUTTON_LEFT);
			platform_pad_button(pad, INPUT_GAMEPAD_DPAD_RIGHT, held, WPAD_CLASSIC_BUTTON_RIGHT);
			platform_pad_button(pad, INPUT_GAMEPAD_A, held, WPAD_CLASSIC_BUTTON_B);
			platform_pad_button(pad, INPUT_GAMEPAD_X, held, WPAD_CLASSIC_BUTTON_Y);
			platform_pad_button(pad, INPUT_GAMEPAD_B, held, WPAD_CLASSIC_BUTTON_A);
			platform_pad_button(pad, INPUT_GAMEPAD_Y, held, WPAD_CLASSIC_BUTTON_X);
			platform_pad_button(pad, INPUT_GAMEPAD_L_SHOULDER, held, WPAD_CLASSIC_BUTTON_FULL_L);
			platform_pad_button(pad, INPUT_GAMEPAD_R_SHOULDER, held, WPAD_CLASSIC_BUTTON_FULL_R);
			platform_pad_value(pad, INPUT_GAMEPAD_L_SHOULDER, fminf(data->exp.classic.l_shoulder, 1.0f));
			platform_pad_value(pad, INPUT_GAMEPAD_R_SHOULDER, fminf(data->exp.classic.r_shoulder, 1.0f));
			platform_pad_button(pad, INPUT_GAMEPAD_L_TRIGGER, held, WPAD_CLASSIC_BUTTON_ZL);
			platform_pad_button(pad, INPUT_GAMEPAD_R_TRIGGER, held, WPAD_CLASSIC_BUTTON_ZR);
			platform_pad_button(pad, INPUT_GAMEPAD_SELECT, held, WPAD_CLASSIC_BUTTON_MINUS);
			platform_pad_button(pad, INPUT_GAMEPAD_START, held, WPAD_CLASSIC_BUTTON_PLUS);
			platform_pad_joystick(pad, &data->exp.classic.ljs, false);
			platform_pad_joystick(pad, &data->exp.classic.rjs, true);
		}
		else if (type == WPAD_EXP_NONE || (type == WPAD_EXP_NUNCHUK && data->exp.type == (int)type)) {
			bool nunchuk = type == WPAD_EXP_NUNCHUK;
			platform_pad_button(pad, INPUT_GAMEPAD_DPAD_UP, held, nunchuk ? WPAD_BUTTON_UP : WPAD_BUTTON_RIGHT);
			platform_pad_button(pad, INPUT_GAMEPAD_DPAD_DOWN, held, nunchuk ? WPAD_BUTTON_DOWN : WPAD_BUTTON_LEFT);
			platform_pad_button(pad, INPUT_GAMEPAD_DPAD_LEFT, held, nunchuk ? WPAD_BUTTON_LEFT : WPAD_BUTTON_UP);
			platform_pad_button(pad, INPUT_GAMEPAD_DPAD_RIGHT, held, nunchuk ? WPAD_BUTTON_RIGHT : WPAD_BUTTON_DOWN);
			platform_pad_button(pad, INPUT_GAMEPAD_A, held, nunchuk ? WPAD_BUTTON_A : WPAD_BUTTON_2);
			platform_pad_button(pad, INPUT_GAMEPAD_X, held, nunchuk ? WPAD_BUTTON_B : WPAD_BUTTON_1);
			platform_pad_button(pad, INPUT_GAMEPAD_L_SHOULDER, held, nunchuk ? WPAD_NUNCHUK_BUTTON_Z : WPAD_BUTTON_A);
			platform_pad_button(pad, INPUT_GAMEPAD_R_SHOULDER, held, nunchuk ? WPAD_NUNCHUK_BUTTON_C : WPAD_BUTTON_B);
			platform_pad_button(pad, INPUT_GAMEPAD_Y, held, WPAD_BUTTON_MINUS);
			platform_pad_button(pad, INPUT_GAMEPAD_START, held, WPAD_BUTTON_PLUS);
			if (nunchuk) platform_pad_joystick(pad, &data->exp.nunchuk.js, false);
		}
	}
	u32 gc = PAD_ButtonsHeld(PAD_CHAN0);
	platform_pad_button(pad, INPUT_GAMEPAD_DPAD_UP, gc, PAD_BUTTON_UP);
	platform_pad_button(pad, INPUT_GAMEPAD_DPAD_DOWN, gc, PAD_BUTTON_DOWN);
	platform_pad_button(pad, INPUT_GAMEPAD_DPAD_LEFT, gc, PAD_BUTTON_LEFT);
	platform_pad_button(pad, INPUT_GAMEPAD_DPAD_RIGHT, gc, PAD_BUTTON_RIGHT);
	platform_pad_button(pad, INPUT_GAMEPAD_A, gc, PAD_BUTTON_A);
	platform_pad_button(pad, INPUT_GAMEPAD_X, gc, PAD_BUTTON_B);
	platform_pad_button(pad, INPUT_GAMEPAD_B, gc, PAD_BUTTON_X);
	platform_pad_button(pad, INPUT_GAMEPAD_Y, gc, PAD_BUTTON_Y);
	platform_pad_button(pad, INPUT_GAMEPAD_L_SHOULDER, gc, PAD_TRIGGER_L);
	platform_pad_button(pad, INPUT_GAMEPAD_R_SHOULDER, gc, PAD_TRIGGER_R);
	float left = PAD_TriggerL(PAD_CHAN0) / 255.0f;
	float right = PAD_TriggerR(PAD_CHAN0) / 255.0f;
	if (left > 0.15f) platform_pad_value(pad, INPUT_GAMEPAD_L_SHOULDER, left);
	if (right > 0.15f) platform_pad_value(pad, INPUT_GAMEPAD_R_SHOULDER, right);
	platform_pad_button(pad, INPUT_GAMEPAD_SELECT, gc, PAD_TRIGGER_Z);
	platform_pad_button(pad, INPUT_GAMEPAD_START, gc, PAD_BUTTON_START);
	platform_pad_stick(pad, PAD_StickX(PAD_CHAN0) / 72.0f, PAD_StickY(PAD_CHAN0) / 72.0f, false);
	platform_pad_stick(pad, PAD_SubStickX(PAD_CHAN0) / 72.0f, PAD_SubStickY(PAD_CHAN0) / 72.0f, true);
	for (int i = 0; i < WII_PAD_COUNT; i++) {
		if (pad[i] != last[i]) {
			input_set_button_state(INPUT_GAMEPAD_A + i, pad[i]);
			last[i] = pad[i];
		}
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
	PAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
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
