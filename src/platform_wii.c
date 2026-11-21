#include "wii_profile.h"
#define WII_MAIN_STACK_BYTES (256 * 1024)
static unsigned char stack[WII_MAIN_STACK_BYTES] __attribute__((aligned(8)));
void *__ppc_main_sp __attribute__((section(".sdata"))) = &stack[sizeof(stack)];

#include <stdint.h>
#include <math.h>
#include <malloc.h>
#include <string.h>
#include <asndlib.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dvm.h>
#include <sdcard/wiisd_io.h>
#include <ogc/usbstorage.h>
#include <ogc/conf.h>
#include <ogc/cache.h>
#include <ogc/console.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>
#include <ogc/cond.h>
#include <ogc/pad.h>
#include <ogc/system.h>
#include <ogc/video.h>
#include <wiiuse/wpad.h>
#ifdef WII_DIAG
#include <gccore.h>
#include <tuxedo/thread.h>
#include <tuxedo/ppc/exception.h>
#endif

#include "platform.h"
#include "platform_wii.h"
#include "system.h"
#include "mem.h"
#include "utils.h"
#include "input.h"
#if defined(WII_SMOKE) || defined(WII_PROFILE)
#include "wipeout/game.h"
#endif

#ifdef WII_SMOKE
#include "wipeout/ship.h"
#include "wipeout/ship_ai.h"
#include "wipeout/ship_player.h"
#endif

// Only debug-type builds write wiipeout.log; the smoke and profile tools read it.
#if defined(WII_DEBUG) || defined(WII_SMOKE) || defined(WII_PROFILE) || defined(WII_DIAG)
#define WII_LOG_FILE
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
static float rumble_strength;
static float rumble_acc;
static bool rumble_on;
static u64 start_tick;
static FILE *log_file;
static bool log_writing;
static const devoptab_t *console_stdout;
static const devoptab_t *console_stderr;
static devoptab_t log_stdout;
static devoptab_t log_stderr;
static uint8_t *music_buffer;

// One whole QOA track (about 35 KB per second of stereo audio).
#define WII_MUSIC_BYTES (16 * 1024 * 1024)

#define AUDIO_VOICE 0
#define AUDIO_RATE 44100
#define AUDIO_RING_FRAMES 8192
#define AUDIO_LEAD_FRAMES 3072
#define AUDIO_CHUNK_FRAMES 512
#define AUDIO_RING_BYTES (AUDIO_RING_FRAMES * 2 * sizeof(int16_t))

static int16_t *audio_ring;
static float audio_scratch[AUDIO_CHUNK_FRAMES * 2];
static void (*audio_mix_cb)(float *buffer, uint32_t len);
static bool audio_ready;
static bool audio_voice_started;
static u32 audio_last_ticks;
static u64 audio_ticks;
static u64 audio_write_frame;
static lwp_t audio_thread;
static uint8_t audio_stack[32 * 1024] __attribute__((aligned(8)));
static volatile bool audio_stopping;
static bool audio_thread_started;

static void *platform_audio_worker(void *unused);

static void platform_audio_init(void) {
	ASND_Init();
	ASND_Pause(0);
	audio_ready = true;
	audio_ring = memalign(32, AUDIO_RING_BYTES);
	if (!audio_ring) return;
	memset(audio_ring, 0, AUDIO_RING_BYTES);
	DCFlushRange(audio_ring, AUDIO_RING_BYTES);
	if (ASND_SetInfiniteVoice(AUDIO_VOICE, VOICE_STEREO_16BIT, AUDIO_RATE, 0,
		audio_ring, AUDIO_RING_BYTES, 255, 255) == SND_OK) {
		audio_voice_started = true;
		if (LWP_CreateThread(&audio_thread, platform_audio_worker, NULL, audio_stack, sizeof(audio_stack), 90) == 0) {
			audio_thread_started = true;
		}
		else {
			printf("audio: failed to create mixer thread\n");
		}
	}
}

static void platform_audio_update(void) {
	if (!audio_voice_started) return;
	u32 ticks = ASND_GetTickCounterVoice(AUDIO_VOICE);
	audio_ticks += (u32)(ticks - audio_last_ticks);
	audio_last_ticks = ticks;
	u64 play_frame = audio_ticks * AUDIO_RATE / 48000;
	if (audio_write_frame < play_frame || audio_write_frame > play_frame + AUDIO_RING_FRAMES) {
		// Discard stale audio after a missed deadline or a long loading stall.
		memset(audio_ring, 0, AUDIO_RING_BYTES);
		DCFlushRange(audio_ring, AUDIO_RING_BYTES);
		audio_write_frame = play_frame + AUDIO_LEAD_FRAMES;
	}
	u64 target = play_frame + AUDIO_LEAD_FRAMES;
	while (audio_write_frame < target) {
		u32 frames = target - audio_write_frame > AUDIO_CHUNK_FRAMES ? AUDIO_CHUNK_FRAMES : (u32)(target - audio_write_frame);
		u32 offset = (u32)audio_write_frame & (AUDIO_RING_FRAMES - 1);
		if (frames > AUDIO_RING_FRAMES - offset) frames = AUDIO_RING_FRAMES - offset;
		void (*mix_cb)(float *, uint32_t) = __atomic_load_n(&audio_mix_cb, __ATOMIC_ACQUIRE);
		if (mix_cb) mix_cb(audio_scratch, frames * 2);
		else memset(audio_scratch, 0, frames * 2 * sizeof(float));
		for (u32 i = 0; i < frames * 2; i++) {
			float sample = fmaxf(-1.0f, fminf(audio_scratch[i], 1.0f));
			audio_ring[offset * 2 + i] = (int16_t)(sample * 32767.0f);
		}
		audio_write_frame += frames;
	}
	DCFlushRange(audio_ring, AUDIO_RING_BYTES);
}

static void *platform_audio_worker(void *unused) {
	(void)unused;
	while (!__atomic_load_n(&audio_stopping, __ATOMIC_ACQUIRE)) {
		platform_audio_update();
		usleep(5000);
	}
	return NULL;
}

static void platform_audio_shutdown(void) {
	if (!audio_ready) return;
	if (audio_thread_started) {
		__atomic_store_n(&audio_stopping, true, __ATOMIC_RELEASE);
		LWP_JoinThread(audio_thread, NULL);
		audio_thread_started = false;
	}
	ASND_StopVoice(AUDIO_VOICE);
	ASND_End();
	free(audio_ring);
	audio_ring = NULL;
	audio_voice_started = false;
	audio_ready = false;
}

#ifdef WII_SMOKE
static u32 smoke_frame;
#ifdef WII_SMOKE_INTRO
static bool smoke_intro_seen;
static double smoke_intro_start;
static double smoke_intro_last;
static double smoke_intro_max;
static u32 smoke_intro_frames;
static u32 smoke_intro_over40;

static void platform_smoke_intro_sample(double now) {
	double delta = now - smoke_intro_last;
	smoke_intro_last = now;
	smoke_intro_frames++;
	if (delta > smoke_intro_max) smoke_intro_max = delta;
	if (delta > 0.040) smoke_intro_over40++;
}
#else
static bool smoke_menu_seen;
static bool smoke_race_seen;
static u32 smoke_race_frame;
#if defined(WII_PROFILE) && defined(WII_PROFILE_FRAMES) && WII_PROFILE_FRAMES > 0
static u32 smoke_profile_start;
#endif
#ifdef WII_SMOKE_FULL
static bool smoke_full_intro_seen;
static bool smoke_full_title_seen;
static bool smoke_finish_done;
static bool smoke_finish_results;
static u32 smoke_finish_frame;
#else
static bool smoke_lap_done;
static u32 smoke_lap_frame;
#endif
#endif
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

static void platform_log_init(bool sd_mounted, const char *app_dir) {
	console_stdout = devoptab_list[STD_OUT];
	console_stderr = devoptab_list[STD_ERR];
	log_stdout = *console_stdout;
	log_stderr = *console_stderr;
	log_stdout.write_r = platform_log_write_stdout;
	log_stderr.write_r = platform_log_write_stderr;
	devoptab_list[STD_OUT] = &log_stdout;
	devoptab_list[STD_ERR] = &log_stderr;
#ifdef WII_LOG_FILE
	if (sd_mounted) {
		if (app_dir && strncmp(app_dir, "sd:/", 4) == 0) {
			char log_path[300];
			snprintf(log_path, sizeof(log_path), "%swiipeout.log", app_dir);
			log_file = fopen(log_path, "w");
		}
		if (!log_file) {
			mkdir("sd:/apps/wiipeout", 0777);
			log_file = fopen("sd:/apps/wiipeout/wiipeout.log", "w");
		}
	}
#else
	(void)sd_mounted;
	(void)app_dir;
#endif
	atexit(platform_log_close);
}

// HBC passes the DOL path in argv[0]; the data folder sits next to boot.dol.
static const char *platform_app_dir(int argc, char *argv[]) {
	static char dir[256];
	if (argc < 1 || !argv || !argv[0]) return NULL;
	const char *colon = strchr(argv[0], ':');
	const char *slash = strrchr(argv[0], '/');
	if (!colon || !slash || slash < colon) return NULL;
	size_t len = (size_t)(slash - argv[0]) + 1;
	if (len >= sizeof(dir)) return NULL;
	memcpy(dir, argv[0], len);
	dir[len] = '\0';
	return dir;
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
#ifdef WII_SMOKE_INTRO
	// Use elapsed time so the timeout is independent of refresh rate and stalls.
	if (smoke_intro_seen && platform_now() - smoke_intro_start >= 240.0) {
		printf("SMOKE FAIL: intro timeout\n");
		platform_wii_log_sync();
		power_requested = true;
	}
#else
#ifdef WII_SMOKE_FULL
	// Use elapsed time so the timeout is independent of refresh rate and stalls.
	if (platform_now() >= 900.0) {
		printf("SMOKE FAIL: timeout, intro=%d title=%d menu=%d race=%d lap=%d\n", smoke_full_intro_seen,
			smoke_full_title_seen, smoke_menu_seen, smoke_race_seen,
			smoke_race_seen ? g.ships[g.pilot].max_lap : -1);
		platform_wii_log_sync();
		power_requested = true;
		return;
	}
	// RETURN before the title would skip the intro, so the full smoke waits for it to end.
	bool return_ready = smoke_full_title_seen;
#else
	bool return_ready = true;
#endif
	// RETURN confirms menu pages; tapping every 30 frames makes each press a new edge.
	// The release is always sent so RETURN is never left held when the race loads.
	if (return_ready && !smoke_race_seen && smoke_frame % 30 == 0) input_set_button_state(INPUT_KEY_RETURN, 1.0f);
	if (smoke_frame % 30 == 1) input_set_button_state(INPUT_KEY_RETURN, 0.0f);
#ifdef WII_SMOKE_2P
	// The players page sets 1 on RETURN; forcing 2 until the race also adds the P2 pilot page.
	if (!smoke_race_seen) g.players = 2;
	if (smoke_race_seen) {
		static float last_shake[2];
		static uint32_t last_flags[2];
		for (int i = 0; i < 2; i++) {
			ship_t *p = &g.ships[g.pilots[i]];
			if (p->update_func == ship_player_update_race) p->update_func = ship_ai_update_race;
			if (g.cameras[i].shake_timer > last_shake[i] + 0.001f)
				printf("SMOKE2P shake cam=%d frame %u timer=%.2f\n", i, (unsigned int)smoke_frame, g.cameras[i].shake_timer);
			if (p->flags != last_flags[i])
				printf("SMOKE2P flags p%d frame %u %08x -> %08x\n", i + 1, (unsigned int)smoke_frame, (unsigned int)last_flags[i], (unsigned int)p->flags);
			last_shake[i] = g.cameras[i].shake_timer;
			last_flags[i] = p->flags;
		}
	}
#endif
	if (smoke_race_seen) {
		ship_t *ship = &g.ships[g.pilot];
		// AI drives the player ship; flags stay untouched so the HUD still draws.
		if (ship->update_func == ship_player_update_race) {
			ship->update_func = ship_ai_update_race;
			printf("SMOKE autopilot frame %u\n", (unsigned int)smoke_frame);
#if defined(WII_PROFILE) && defined(WII_PROFILE_FRAMES) && WII_PROFILE_FRAMES > 0
			smoke_profile_start = smoke_frame;
#endif
		}
#ifdef WII_SMOKE_FULL
		if (!smoke_finish_done && ship->max_lap >= NUM_LAPS) {
			smoke_finish_done = true;
			smoke_finish_frame = smoke_frame;
			smoke_finish_results = flags_not(ship->flags, SHIP_RACING);
			printf("SMOKE race finished frame %u, results=%d\n", (unsigned int)smoke_frame, smoke_finish_results);
		}
		if (smoke_finish_done && smoke_frame - smoke_finish_frame >= 180) {
			if (smoke_finish_results) {
				printf("SMOKE PASS: intro, menu, race, finish and results reached, frame %u\n", (unsigned int)smoke_frame);
			} else {
				printf("SMOKE FAIL: no results screen\n");
			}
			platform_wii_log_sync();
			power_requested = true;
			return;
		}
#else
#ifdef WII_SMOKE_2P
		if (!smoke_lap_done && ship->max_lap >= 2) {
#else
		if (!smoke_lap_done && ship->max_lap >= 1) {
#endif
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
#endif
	}
#ifndef WII_SMOKE_FULL
	// Reached only while the race has not passed yet, so a late race also times out.
	if (smoke_frame >= 60 * 360) {
		printf("SMOKE FAIL: timeout, menu=%d race=%d lap=%d\n", smoke_menu_seen, smoke_race_seen,
			smoke_race_seen ? g.ships[g.pilot].max_lap : -1);
		platform_wii_log_sync();
		power_requested = true;
	}
#endif
#endif
}
#endif

#define WII_PAD_COUNT (INPUT_GAMEPAD_R_STICK_RIGHT - INPUT_GAMEPAD_A + 1)
#define WII_TILT_BRAKE_ON 0.40f
#define WII_TILT_BRAKE_OFF 0.30f
#ifdef WII_DIAG
static float diag_cc_l = -1, diag_cc_r = -1;
static u8 diag_cc_type, diag_cc_ls_raw, diag_cc_rs_raw;
static u32 diag_cc_btns;
#endif

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

static void platform_rumble_off(void) {
	rumble_strength = 0;
	rumble_acc = 0;
	if (!rumble_on) return;
	WPAD_Rumble(WPAD_CHAN_0, 0);
	PAD_ControlMotor(PAD_CHAN0, PAD_MOTOR_STOP_HARD);
	rumble_on = false;
}

static void platform_pump_events(void) {
	static float last[2][WII_PAD_COUNT];
	static bool cc_format_sent[2];
	static bool tilt_brake_left[2], tilt_brake_right[2];
	WPAD_ScanPads();
	PAD_ScanPads();
	for (int player = 0; player < 2; player++) {
		float pad[WII_PAD_COUNT] = {0};
		u32 type;
		WPADData *data = WPAD_Data(player);
		bool connected = WPAD_Probe(player, &type) == WPAD_ERR_NONE && data && data->err == WPAD_ERR_NONE;
		if (!connected || type != WPAD_EXP_NONE) tilt_brake_left[player] = tilt_brake_right[player] = false;
		if (connected && type == WPAD_EXP_CLASSIC) {
			if (!cc_format_sent[player]) {
				// Retry the format after the Classic Controller handshake completes.
				WPAD_SetDataFormat(player, WPAD_FMT_BTNS_ACC_IR);
				cc_format_sent[player] = true;
			}
		}
		else cc_format_sent[player] = false;
		if (connected) {
			u32 held = data->btns_h;
			if (player == 0 && (data->btns_d & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME))) platform_exit();
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
				platform_pad_button(pad, INPUT_GAMEPAD_L_SHOULDER, held, WPAD_CLASSIC_BUTTON_ZL);
				platform_pad_button(pad, INPUT_GAMEPAD_R_SHOULDER, held, WPAD_CLASSIC_BUTTON_ZR);
				// The analog L/R read 1.0 at rest on hardware (both air brakes on), so only the digital click counts.
#ifdef WII_DIAG
				if (player == 0) {
					diag_cc_l = data->exp.classic.l_shoulder;
					diag_cc_r = data->exp.classic.r_shoulder;
					diag_cc_type = data->exp.classic.type;
					diag_cc_ls_raw = data->exp.classic.ls_raw;
					diag_cc_rs_raw = data->exp.classic.rs_raw;
					diag_cc_btns = data->exp.classic.btns;
				}
#endif
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
				if (nunchuk) {
					platform_pad_button(pad, INPUT_GAMEPAD_L_SHOULDER, held, WPAD_NUNCHUK_BUTTON_Z);
					platform_pad_button(pad, INPUT_GAMEPAD_R_SHOULDER, held, WPAD_NUNCHUK_BUTTON_C);
				}
				else {
					float y = data->gforce.y;
					tilt_brake_left[player] = tilt_brake_left[player] ? y >= WII_TILT_BRAKE_OFF : y >= WII_TILT_BRAKE_ON;
					tilt_brake_right[player] = tilt_brake_right[player] ? y <= -WII_TILT_BRAKE_OFF : y <= -WII_TILT_BRAKE_ON;
					if (tilt_brake_left[player]) platform_pad_value(pad, INPUT_GAMEPAD_L_SHOULDER, 1.0f);
					if (tilt_brake_right[player]) platform_pad_value(pad, INPUT_GAMEPAD_R_SHOULDER, 1.0f);
				}
				platform_pad_button(pad, INPUT_GAMEPAD_Y, held, WPAD_BUTTON_MINUS);
				platform_pad_button(pad, INPUT_GAMEPAD_START, held, WPAD_BUTTON_PLUS);
				if (nunchuk) platform_pad_joystick(pad, &data->exp.nunchuk.js, false);
			}
		}
		u32 gc = PAD_ButtonsHeld(player);
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
		float left = PAD_TriggerL(player) / 255.0f;
		float right = PAD_TriggerR(player) / 255.0f;
		if (left > 0.15f) platform_pad_value(pad, INPUT_GAMEPAD_L_SHOULDER, left);
		if (right > 0.15f) platform_pad_value(pad, INPUT_GAMEPAD_R_SHOULDER, right);
		platform_pad_button(pad, INPUT_GAMEPAD_SELECT, gc, PAD_TRIGGER_Z);
		platform_pad_button(pad, INPUT_GAMEPAD_START, gc, PAD_BUTTON_START);
		platform_pad_stick(pad, PAD_StickX(player) / 72.0f, PAD_StickY(player) / 72.0f, false);
		platform_pad_stick(pad, PAD_SubStickX(player) / 72.0f, PAD_SubStickY(player) / 72.0f, true);
		for (int i = 0; i < WII_PAD_COUNT; i++) {
			if (pad[i] != last[player][i]) {
				input_set_player_button_state(player, INPUT_GAMEPAD_A + i, pad[i]);
				last[player][i] = pad[i];
			}
		}
	}
	// Motors are on/off only, so strength is approximated by frame-rate PWM.
	rumble_acc += rumble_strength;
	bool on = rumble_acc >= 1.0f;
	if (on) rumble_acc -= 1.0f;
	if (on != rumble_on) {
		WPAD_Rumble(WPAD_CHAN_0, on);
		PAD_ControlMotor(PAD_CHAN0, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP_HARD);
		rumble_on = on;
	}
#ifdef WII_SMOKE
	platform_smoke_update();
#endif
}

static void platform_prepare_frame(void) {
}

static void platform_wii_io_poll(void);

static void platform_end_frame(void) {
	platform_wii_io_poll();
#if defined(RENDERER_NULL)
	VIDEO_WaitVSync();
#endif
#if defined(WII_SMOKE) && defined(WII_SMOKE_INTRO)
	if (smoke_intro_seen) platform_smoke_intro_sample(platform_now());
#endif
}

static bool platform_data_root_exists(const char *path) {
	struct stat info;
	return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

// USB drives can take a few seconds to spin up, so retry for about 3 s.
static bool platform_mount_usb(void) {
	for (int i = 0; i < 20; i++) {
		if (dvmProbeMountDiscIface("usb", &__io_usbstorage, 4, 64)) return true;
		if (i < 19) usleep(150000);
	}
	return false;
}

void platform_exit(void) {
	platform_rumble_off();
	wants_to_exit = true;
}

void platform_set_rumble(float strength) {
	if (strength <= 0) platform_rumble_off();
	else rumble_strength = fminf(strength, 1.0f);
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
	__atomic_store_n(&audio_mix_cb, cb, __ATOMIC_RELEASE);
}

static void platform_wii_io_shutdown(void);

#define WII_IO_CHUNK (32 * 1024)
static lwp_t io_thread;
static mutex_t io_mutex;
static cond_t io_cond;
static uint8_t io_stack[32 * 1024] __attribute__((aligned(8)));
static bool io_ready, io_stopping, io_save_active;
static uint8_t *io_save_data;
static uint32_t io_save_len;
static char io_save_path[256], io_music_path[1024];
static volatile uint32_t io_music_generation, io_music_loaded, io_music_total;
static volatile int io_music_state = WII_MUSIC_ERROR;
static uint32_t io_save_failures;
#ifdef WII_PROFILE
typedef struct {
	bool music;
	uint32_t bytes;
	double ms;
} wii_io_event_t;
static wii_io_event_t io_events[64];
static uint32_t io_event_read, io_event_write;

static void platform_wii_io_event(bool music, uint32_t bytes, u64 start) {
	if (io_event_write - io_event_read == 64) return;
	wii_io_event_t *event = &io_events[io_event_write++ % 64];
	event->music = music;
	event->bytes = bytes;
	event->ms = (double)diff_ticks(start, gettime()) / TB_TIMER_CLOCK;
}
#endif

static bool platform_wii_write_save(const char *path, const uint8_t *data, uint32_t len) {
	char tmp[260];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *file = fopen(tmp, "wb");
	if (!file) return false;
	size_t written = fwrite(data, 1, len, file);
	int closed = fclose(file);
	if (written != len || closed != 0) {
		remove(tmp);
		return false;
	}
	remove(path);
	return rename(tmp, path) == 0;
}

static void *platform_wii_io_worker(void *unused) {
	(void)unused;
	FILE *music_file = NULL;
	uint32_t generation = 0, total = 0, loaded = 0;
#ifdef WII_PROFILE
	u64 music_start = 0;
#endif
	for (;;) {
		LWP_MutexLock(io_mutex);
		while (!io_stopping && !io_save_data && generation == io_music_generation && !music_file)
			LWP_CondWait(io_cond, io_mutex);
		if (io_stopping && !io_save_data) {
			LWP_MutexUnlock(io_mutex);
			break;
		}
		if (io_save_data) {
			uint8_t *data = io_save_data;
			uint32_t len = io_save_len;
			char path[sizeof(io_save_path)];
			memcpy(path, io_save_path, sizeof(path));
			io_save_data = NULL;
			io_save_active = true;
			LWP_MutexUnlock(io_mutex);
#ifdef WII_PROFILE
			u64 start = gettime();
#endif
			bool ok = platform_wii_write_save(path, data, len);
			free(data);
			LWP_MutexLock(io_mutex);
			io_save_active = false;
			if (!ok) io_save_failures++;
#ifdef WII_PROFILE
			platform_wii_io_event(false, len, start);
#endif
			LWP_MutexUnlock(io_mutex);
			continue;
		}
		if (generation != io_music_generation) {
			generation = io_music_generation;
			char path[sizeof(io_music_path)];
			memcpy(path, io_music_path, sizeof(path));
			LWP_MutexUnlock(io_mutex);
			if (music_file) fclose(music_file);
#ifdef WII_PROFILE
			music_start = gettime();
#endif
			music_file = music_buffer ? fopen(path, "rb") : NULL;
			long size = -1;
			if (music_file && fseek(music_file, 0, SEEK_END) == 0) {
				size = ftell(music_file);
				if (fseek(music_file, 0, SEEK_SET) != 0) size = -1;
			}
			loaded = 0;
			total = size > 0 && size <= WII_MUSIC_BYTES ? (uint32_t)size : 0;
			LWP_MutexLock(io_mutex);
			if (generation == io_music_generation) {
				io_music_total = total;
				if (!total) io_music_state = WII_MUSIC_ERROR;
			}
			LWP_MutexUnlock(io_mutex);
			if (!total && music_file) { fclose(music_file); music_file = NULL; }
			continue;
		}
		LWP_MutexUnlock(io_mutex);
		if (music_file) {
			uint32_t chunk = total - loaded;
			if (chunk > WII_IO_CHUNK) chunk = WII_IO_CHUNK;
			size_t count = fread(music_buffer + loaded, 1, chunk, music_file);
			loaded += count;
			bool finished = count != chunk || loaded == total;
			if (finished) { fclose(music_file); music_file = NULL; }
			LWP_MutexLock(io_mutex);
			if (generation == io_music_generation) {
				__asm__ volatile("sync" ::: "memory");
				io_music_loaded = loaded;
				if (finished) io_music_state = count == chunk ? WII_MUSIC_DONE : WII_MUSIC_ERROR;
#ifdef WII_PROFILE
				if (finished && count == chunk) {
					platform_wii_io_event(true, loaded, music_start);
				}
#endif
			}
			LWP_MutexUnlock(io_mutex);
		}
	}
	if (music_file) fclose(music_file);
	return NULL;
}

static void platform_wii_io_init(void) {
	if (LWP_MutexInit(&io_mutex, false) < 0) return;
	if (LWP_CondInit(&io_cond) < 0) { LWP_MutexDestroy(io_mutex); return; }
	LWP_SetThreadPriority(LWP_GetSelf(), 80);
	if (LWP_CreateThread(&io_thread, platform_wii_io_worker, NULL, io_stack, sizeof(io_stack), 50) < 0) {
		LWP_CondDestroy(io_cond);
		LWP_MutexDestroy(io_mutex);
		return;
	}
	io_ready = true;
	atexit(platform_wii_io_shutdown);
}

uint32_t platform_wii_music_request(const char *asset_path) {
	if (!io_ready) { io_music_state = WII_MUSIC_ERROR; return 0; }
	LWP_MutexLock(io_mutex);
	io_music_generation++;
	snprintf(io_music_path, sizeof(io_music_path), "%s%s", path_assets, asset_path);
	io_music_loaded = io_music_total = 0;
	io_music_state = WII_MUSIC_LOADING;
	uint32_t generation = io_music_generation;
	LWP_CondSignal(io_cond);
	LWP_MutexUnlock(io_mutex);
	return generation;
}

wii_music_status_t platform_wii_music_status(void) {
	wii_music_status_t status;
	status.generation = io_music_generation;
	status.loaded = io_music_loaded;
	__asm__ volatile("sync" ::: "memory");
	status.total = io_music_total;
	status.state = io_music_state;
	status.data = music_buffer;
	return status;
}

static void platform_wii_wait_save(bool bounded) {
	if (!io_ready) return;
	u64 start = gettime();
	for (;;) {
		LWP_MutexLock(io_mutex);
		bool busy = io_save_active || io_save_data;
		LWP_MutexUnlock(io_mutex);
		if (!busy || (bounded && diff_ticks(start, gettime()) >= TB_TIMER_CLOCK * 2000ULL)) break;
		usleep(1000);
	}
}

static void platform_wii_io_poll(void) {
	if (!io_ready) return;
	LWP_MutexLock(io_mutex);
	uint32_t failures = io_save_failures;
	io_save_failures = 0;
#ifdef WII_PROFILE
	wii_io_event_t events[64];
	uint32_t event_count = 0;
	while (io_event_read != io_event_write)
		events[event_count++] = io_events[io_event_read++ % 64];
#endif
	LWP_MutexUnlock(io_mutex);
	while (failures--) printf("save: write failed (worker)\n");
#ifdef WII_PROFILE
	for (uint32_t i = 0; i < event_count; i++) {
		printf("PROF %s bytes=%u ms=%.1f\n", events[i].music ? "music_load" : "save_write", events[i].bytes, events[i].ms);
	}
#endif
}

static void platform_wii_io_shutdown(void) {
	if (!io_ready) return;
	LWP_MutexLock(io_mutex);
	io_stopping = true;
	LWP_CondSignal(io_cond);
	LWP_MutexUnlock(io_mutex);
	platform_wii_wait_save(true);
	platform_wii_io_poll();
	io_ready = false;
}

static char *platform_path(const char *root, const char *name) {
	snprintf(temp_path, sizeof(temp_path), "%s%s", root, name);
	return temp_path;
}

bool platform_asset_exists(const char *name) {
	return file_exists(platform_path(path_assets, name));
}

#ifdef WII_PROFILE
static uint32_t scene_assets;
static uint64_t scene_asset_bytes, scene_asset_ticks;
#endif

FILE *platform_open_asset(const char *name, const char *mode) {
	FILE *file = fopen(platform_path(path_assets, name), mode);
#if defined(WII_DIAG) || defined(WII_SMOKE)
	platform_wii_log_sync();
#endif
	return file;
}

uint8_t *platform_load_asset(const char *name, uint32_t *bytes_read) {
#ifdef WII_PROFILE
	u64 start = gettime();
#endif
	uint8_t *bytes = file_load(platform_path(path_assets, name), bytes_read);
#ifdef WII_PROFILE
	scene_asset_ticks += diff_ticks(start, gettime());
	scene_assets++;
	if (bytes) scene_asset_bytes += *bytes_read;
#endif
#if defined(WII_DIAG) || defined(WII_SMOKE)
	platform_wii_log_sync();
#endif
	return bytes;
}

uint8_t *platform_load_userdata(const char *name, uint32_t *bytes_read) {
	platform_wii_wait_save(false);
	char path[256];
	snprintf(path, sizeof(path), "%s%s", path_userdata, name);
	if (!file_exists(path)) {
		// A power cut between remove() and rename() in store leaves only the tmp file
		snprintf(path, sizeof(path), "%s%s.tmp", path_userdata, name);
	}
	if (!file_exists(path)) {
		*bytes_read = 0;
#if defined(WII_DIAG) || defined(WII_SMOKE)
		platform_wii_log_sync();
#endif
		return NULL;
	}
	uint8_t *bytes = file_load(path, bytes_read);
#if defined(WII_DIAG) || defined(WII_SMOKE)
	platform_wii_log_sync();
#endif
	return bytes;
}

uint32_t platform_store_userdata(const char *name, void *bytes, int32_t len) {
	if (!io_ready || len < 0) {
		printf("save: write failed (worker unavailable)\n");
		return 0;
	}
	uint8_t *copy = malloc(len ? (size_t)len : 1);
	if (!copy) {
		printf("save: write failed (out of memory)\n");
		return 0;
	}
	memcpy(copy, bytes, len);
	LWP_MutexLock(io_mutex);
	uint8_t *old = io_save_data;
	io_save_data = copy;
	io_save_len = len;
	snprintf(io_save_path, sizeof(io_save_path), "%s%s", path_userdata, name);
	LWP_CondSignal(io_cond);
	LWP_MutexUnlock(io_mutex);
	free(old);
	return len;
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
#ifdef WII_PROFILE
	printf("PROF scene_load tag=%s assets=%u bytes=%llu io_ms=%.1f\n", tag,
		scene_assets, (unsigned long long)scene_asset_bytes, (double)scene_asset_ticks / TB_TIMER_CLOCK);
	scene_assets = 0;
	scene_asset_bytes = scene_asset_ticks = 0;
#endif
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
#ifdef WII_DIAG
	printf("diag %s: dropped_tris=%lu cc_l=%.3f cc_r=%.3f cc_type=%u cc_ls_raw=%u cc_rs_raw=%u cc_btns=%lu\n",
		tag, (unsigned long)wii_diag_dropped_tris, diag_cc_l, diag_cc_r, (unsigned int)diag_cc_type,
		(unsigned int)diag_cc_ls_raw, (unsigned int)diag_cc_rs_raw, (unsigned long)diag_cc_btns);
#endif
#ifdef WII_SMOKE
#ifdef WII_SMOKE_INTRO
	if (strcmp(tag, "intro") == 0) {
		smoke_intro_seen = true;
		smoke_intro_start = smoke_intro_last = platform_now();
	}
	else if (strcmp(tag, "title") == 0 && smoke_intro_seen) {
		double now = platform_now();
		double wall = now - smoke_intro_start;
		platform_smoke_intro_sample(now);
		smoke_intro_seen = false;
		printf("SMOKE intro wall=%.2fs frames=%u avg=%.2fms max=%.2fms over40=%u\n",
			wall, (unsigned int)smoke_intro_frames, wall * 1000.0 / smoke_intro_frames,
			smoke_intro_max * 1000.0, (unsigned int)smoke_intro_over40);
		printf(wall <= 93.68 * 1.05 ? "SMOKE PASS: intro full speed\n" : "SMOKE FAIL: intro slow\n");
		platform_wii_log_sync();
		power_requested = true;
	}
#else
#ifdef WII_SMOKE_FULL
	if (strcmp(tag, "intro") == 0) smoke_full_intro_seen = true;
	if (strcmp(tag, "title") == 0 && smoke_full_intro_seen) smoke_full_title_seen = true;
#endif
	if (strcmp(tag, "menu") == 0) smoke_menu_seen = true;
	if (strcmp(tag, "race") == 0 && smoke_menu_seen) {
		smoke_race_seen = true;
		smoke_race_frame = smoke_frame;
	}
#endif
	printf("SMOKE scene %s frame %u\n", tag, (unsigned int)smoke_frame);
#endif
}

#ifdef WII_DIAG
#define DIAG_CHAIN 10
volatile u32 wii_diag_phase;
u32 wii_diag_dropped_tris;
static volatile u32 diag_frame;
static KThread *diag_main;
static u8 diag_stack[16 * 1024] __attribute__((aligned(32)));
static lwp_t diag_thread;
static char diag_text[1024];
static volatile bool diag_crashed;

static const char *diag_phase_names[] = {
	"none", "input", "update", "flush wait", "texture wait", "frame copy", "frame wait", "frame flip", "vsync", "frame end",
};

static bool diag_ram(u32 p) {
	return !(p & 3) && ((p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000));
}

static void diag_format(const char *title, u32 pc, u32 lr, u32 sp, u32 extra) {
	u32 phase = wii_diag_phase;
	u8 overhi, underlow, read_idle, cmd_idle, brkpt;
	GX_GetGPStatus(&overhi, &underlow, &read_idle, &cmd_idle, &brkpt);
	int n = snprintf(diag_text, sizeof(diag_text),
		"\n\n %s\n frame %lu, phase %lu (%s), dropped tris %lu\n GP: overhi=%u underlow=%u read_idle=%u cmd_idle=%u brkpt=%u\n"
		" pc %08lx lr %08lx sp %08lx x %08lx\n chain:",
		title, (unsigned long)diag_frame, (unsigned long)phase,
		phase < sizeof(diag_phase_names) / sizeof(diag_phase_names[0]) ? diag_phase_names[phase] : "?",
		(unsigned long)wii_diag_dropped_tris, overhi, underlow, read_idle, cmd_idle, brkpt,
		(unsigned long)pc, (unsigned long)lr, (unsigned long)sp, (unsigned long)extra);
	u32 f = sp;
	for (int j = 0; j < DIAG_CHAIN && n < (int)sizeof(diag_text) - 32; j++) {
		f = diag_ram(f) ? *(u32 *)f : 0;
		u32 ret = diag_ram(f) ? *((u32 *)f + 1) : 0;
		n += snprintf(diag_text + n, sizeof(diag_text) - n, "%s %08lx", j == 5 ? "\n       " : "", (unsigned long)ret);
	}
	snprintf(diag_text + n, sizeof(diag_text) - n, "\n\n Take a photo of this screen.\n");
}

// Draw on both XFBs: plain memory writes, so this also works with interrupts off.
static void diag_show(void) {
	for (int i = 0; i < 2; i++) {
		void *xfb = platform_wii_xfb(i);
		VIDEO_ClearFrameBuffer(video_mode, xfb, COLOR_BLACK);
		console_init(xfb, 20, 20, video_mode->fbWidth, video_mode->xfbHeight, video_mode->fbWidth * VI_DISPLAY_PIX_SZ);
		console_stdout->write_r(_REENT, NULL, diag_text, strlen(diag_text));
	}
}

static void diag_panic(unsigned exid, PPCContext *ctx) {
	// Interrupts stay off from here, so only the screen report is possible, not the SD log.
	if (!diag_crashed) {
		diag_crashed = true;
		u32 dar;
		__asm__ volatile("mfdar %0" : "=r"(dar));
		char title[48];
		snprintf(title, sizeof(title), "WIIPEOUT CRASH: exception %u (x = dar)", exid);
		diag_format(title, ctx->pc, ctx->lr, ctx->gpr[1], dar);
		diag_show();
	}
	while (true) {}
}

// Higher priority than main, so it runs even when the main thread spins.
static void *diag_watch(void *arg) {
	(void)arg;
	u32 last = diag_frame;
	u64 since = gettime();
	while (true) {
		usleep(250000);
		if (diag_frame != last) {
			last = diag_frame;
			since = gettime();
			continue;
		}
		u32 stalled = diff_msec(since, gettime());
		if (stalled < 3000) continue;
		if (!diag_crashed) {
			// main is switched out here, so its saved context holds where it stopped.
			PPCContext *ctx = &diag_main->ctx;
			diag_format("WIIPEOUT HANG: main loop stopped (x = msr)", ctx->pc, ctx->lr, ctx->gpr[1], ctx->msr);
			diag_show();
		}
		// The SD write can block if the main thread holds the FAT lock, so it goes last.
		if (log_file) {
			static char batches[2560];
			fwrite(diag_text, 1, strlen(diag_text), log_file);
			fprintf(log_file, "diag: last GX batches (> = last one the GPU finished; fl: 1=2d 2=ztest 4=zwrite, blend<<4)\n");
			int len = render_gx_diag_format(batches, sizeof(batches));
			fwrite(batches, 1, len < (int)sizeof(batches) ? len : (int)sizeof(batches) - 1, log_file);
			fflush(log_file);
			fsync(fileno(log_file));
		}
#ifdef WII_DIAG_SELFTEST
		// Dolphin flushes its SD image only on a clean power off.
		SYS_ResetSystem(SYS_POWEROFF, 0, 0);
#endif
		while (true) usleep(1000000);
	}
	return NULL;
}

#ifdef WII_DIAG_SELFTEST
static void __attribute__((noinline)) diag_selftest(void) {
	if (WII_DIAG_SELFTEST == 2) *(volatile u32 *)0x10 = 1;
	while (true) __asm__ volatile("nop");
}
#endif

static void diag_init(void) {
	diag_main = KThreadGetSelf();
	PPCExcptCurPanicFn = diag_panic;
	LWP_CreateThread(&diag_thread, diag_watch, NULL, diag_stack, sizeof(diag_stack), 100);
	printf("diag: watchdog on\n");
}

static void diag_frame_done(void) {
	diag_frame++;
#ifdef WII_DIAG_SELFTEST
	if (diag_frame == 600) diag_selftest();
#endif
}
#endif

#ifdef WII_PROFILE
#ifndef WII_PROFILE_WINDOW
#define WII_PROFILE_WINDOW 300
#endif
wii_profile_frame_t wii_profile_frame;
typedef struct {
	wii_profile_frame_t sum, worst;
	uint64_t frames, missed_vsyncs, missed_frames, log_sync_vsyncs, log_sync_frames;
	bool has_worst;
	double frame_ms, frame_max, update_max, submit_max, wait_max;
	double vsync_wait_max, work_ms, work_max;
	double sim_rumble_max, sim_particles_max, sim_scene_max, sim_map_max, sim_pickups_max;
	double sim_ships_max, sim_droid_max, sim_camera_max, sim_weapons_max, sim_other_max, draw_scene_max, draw_track_max, draw_ships_max, draw_fx_max, draw_2d_max, push_max;
} profile_stats_t;
static profile_stats_t profile_window, profile_total;
static double profile_start;
static bool profile_after_log_sync;

static void profile_accumulate(profile_stats_t *s, double frame_ms) {
	if (!profile_after_log_sync && (!s->has_worst || s->frame_max < frame_ms)) {
		s->has_worst = true;
		s->frame_max = frame_ms;
		s->worst = wii_profile_frame;
	}
	s->frames++;
	s->frame_ms += frame_ms;
	double work_ms = frame_ms - wii_profile_frame.vsync_wait_ms;
	s->work_ms += work_ms;
	if (s->work_max < work_ms) s->work_max = work_ms;
	s->sum.vsync_wait_ms += wii_profile_frame.vsync_wait_ms;
	if (s->vsync_wait_max < wii_profile_frame.vsync_wait_ms) s->vsync_wait_max = wii_profile_frame.vsync_wait_ms;
	if (profile_after_log_sync) {
		s->log_sync_vsyncs += wii_profile_frame.missed;
		s->log_sync_frames += wii_profile_frame.missed > 0;
	}
	else {
		s->missed_vsyncs += wii_profile_frame.missed;
		s->missed_frames += wii_profile_frame.missed > 0;
	}
	s->sum.update_ms += wii_profile_frame.update_ms;
	if (s->update_max < wii_profile_frame.update_ms) s->update_max = wii_profile_frame.update_ms;
	s->sum.submit_ms += wii_profile_frame.submit_ms;
	if (s->submit_max < wii_profile_frame.submit_ms) s->submit_max = wii_profile_frame.submit_ms;
	s->sum.wait_ms += wii_profile_frame.wait_ms;
	if (s->wait_max < wii_profile_frame.wait_ms) s->wait_max = wii_profile_frame.wait_ms;
	s->sum.sim_ships_ms += wii_profile_frame.sim_ships_ms;
	if (s->sim_ships_max < wii_profile_frame.sim_ships_ms) s->sim_ships_max = wii_profile_frame.sim_ships_ms;
	s->sum.sim_droid_ms += wii_profile_frame.sim_droid_ms;
	if (s->sim_droid_max < wii_profile_frame.sim_droid_ms) s->sim_droid_max = wii_profile_frame.sim_droid_ms;
	s->sum.sim_camera_ms += wii_profile_frame.sim_camera_ms;
	if (s->sim_camera_max < wii_profile_frame.sim_camera_ms) s->sim_camera_max = wii_profile_frame.sim_camera_ms;
	s->sum.sim_weapons_ms += wii_profile_frame.sim_weapons_ms;
	if (s->sim_weapons_max < wii_profile_frame.sim_weapons_ms) s->sim_weapons_max = wii_profile_frame.sim_weapons_ms;
	s->sum.sim_rumble_ms += wii_profile_frame.sim_rumble_ms;
	if (s->sim_rumble_max < wii_profile_frame.sim_rumble_ms) s->sim_rumble_max = wii_profile_frame.sim_rumble_ms;
	s->sum.sim_particles_ms += wii_profile_frame.sim_particles_ms;
	if (s->sim_particles_max < wii_profile_frame.sim_particles_ms) s->sim_particles_max = wii_profile_frame.sim_particles_ms;
	s->sum.sim_scene_ms += wii_profile_frame.sim_scene_ms;
	if (s->sim_scene_max < wii_profile_frame.sim_scene_ms) s->sim_scene_max = wii_profile_frame.sim_scene_ms;
	s->sum.sim_map_ms += wii_profile_frame.sim_map_ms;
	if (s->sim_map_max < wii_profile_frame.sim_map_ms) s->sim_map_max = wii_profile_frame.sim_map_ms;
	s->sum.sim_pickups_ms += wii_profile_frame.sim_pickups_ms;
	if (s->sim_pickups_max < wii_profile_frame.sim_pickups_ms) s->sim_pickups_max = wii_profile_frame.sim_pickups_ms;
	s->sum.sim_other_ms += wii_profile_frame.sim_other_ms;
	if (s->sim_other_max < wii_profile_frame.sim_other_ms) s->sim_other_max = wii_profile_frame.sim_other_ms;
	s->sum.draw_scene_ms += wii_profile_frame.draw_scene_ms;
	if (s->draw_scene_max < wii_profile_frame.draw_scene_ms) s->draw_scene_max = wii_profile_frame.draw_scene_ms;
	s->sum.draw_track_ms += wii_profile_frame.draw_track_ms;
	if (s->draw_track_max < wii_profile_frame.draw_track_ms) s->draw_track_max = wii_profile_frame.draw_track_ms;
	s->sum.draw_ships_ms += wii_profile_frame.draw_ships_ms;
	if (s->draw_ships_max < wii_profile_frame.draw_ships_ms) s->draw_ships_max = wii_profile_frame.draw_ships_ms;
	s->sum.draw_fx_ms += wii_profile_frame.draw_fx_ms;
	if (s->draw_fx_max < wii_profile_frame.draw_fx_ms) s->draw_fx_max = wii_profile_frame.draw_fx_ms;
	s->sum.draw_2d_ms += wii_profile_frame.draw_2d_ms;
	if (s->draw_2d_max < wii_profile_frame.draw_2d_ms) s->draw_2d_max = wii_profile_frame.draw_2d_ms;
	double push_ms = (double)wii_profile_frame.push_ticks / (double)TB_TIMER_CLOCK;
	s->sum.push_ms += push_ms;
	if (s->push_max < push_ms) s->push_max = push_ms;
	s->sum.draws += wii_profile_frame.draws;
	s->sum.tris += wii_profile_frame.tris;
	s->sum.flush_texture += wii_profile_frame.flush_texture;
	s->sum.flush_model += wii_profile_frame.flush_model;
	s->sum.flush_state += wii_profile_frame.flush_state;
	s->sum.flush_full += wii_profile_frame.flush_full;
	s->sum.flush_end += wii_profile_frame.flush_end;
	s->sum.flush_other += wii_profile_frame.flush_other;
	s->sum.arena_waits += wii_profile_frame.arena_waits;
	s->sum.pushes += wii_profile_frame.pushes;
	s->sum.dropped += wii_profile_frame.dropped;
	s->sum.track_total += wii_profile_frame.track_total;
	s->sum.track_drawn += wii_profile_frame.track_drawn;
	s->sum.track_far += wii_profile_frame.track_far;
	s->sum.track_frustum_culled += wii_profile_frame.track_frustum_culled;
	s->sum.objects_frustum_culled += wii_profile_frame.objects_frustum_culled;
	s->sum.faces += wii_profile_frame.faces;
	s->sum.objects_total += wii_profile_frame.objects_total;
	s->sum.objects_drawn += wii_profile_frame.objects_drawn;
	s->sum.ships += wii_profile_frame.ships;
	s->sum.particles += wii_profile_frame.particles;
	s->sum.uploads += wii_profile_frame.uploads;
	s->sum.replacements += wii_profile_frame.replacements;
	if (s->sum.arena_peak < wii_profile_frame.arena_peak) s->sum.arena_peak = wii_profile_frame.arena_peak;
}

static void profile_print(const char *kind, const profile_stats_t *s) {
	if (!s->frames) return;
	printf("PROF %s players=%d frames=%llu frame_avg_ms=%.3f frame_max_ms=%.3f update_avg_ms=%.3f update_max_ms=%.3f submit_avg_ms=%.3f submit_max_ms=%.3f wait_avg_ms=%.3f wait_max_ms=%.3f"
		" sim_ships_avg_ms=%.3f sim_ships_max_ms=%.3f"
		" sim_droid_avg_ms=%.3f sim_droid_max_ms=%.3f"
		" sim_camera_avg_ms=%.3f sim_camera_max_ms=%.3f"
		" sim_weapons_avg_ms=%.3f sim_weapons_max_ms=%.3f"
		" sim_rumble_avg_ms=%.3f sim_rumble_max_ms=%.3f"
		" sim_particles_avg_ms=%.3f sim_particles_max_ms=%.3f"
		" sim_scene_avg_ms=%.3f sim_scene_max_ms=%.3f"
		" sim_map_avg_ms=%.3f sim_map_max_ms=%.3f"
		" sim_pickups_avg_ms=%.3f sim_pickups_max_ms=%.3f"
		" sim_other_avg_ms=%.3f sim_other_max_ms=%.3f"
		" draw_scene_avg_ms=%.3f draw_scene_max_ms=%.3f"
		" draw_track_avg_ms=%.3f draw_track_max_ms=%.3f"
		" draw_ships_avg_ms=%.3f draw_ships_max_ms=%.3f"
		" draw_fx_avg_ms=%.3f draw_fx_max_ms=%.3f"
		" draw_2d_avg_ms=%.3f draw_2d_max_ms=%.3f"
		" push_avg_ms=%.3f push_max_ms=%.3f"
		" tris_per_draw=%.3f"
		" draws=%llu"
		" tris=%llu"
		" flush_texture=%llu"
		" flush_model=%llu"
		" flush_state=%llu"
		" flush_full=%llu"
		" flush_end=%llu"
		" flush_other=%llu"
		" arena_waits=%llu"
		" pushes=%llu"
		" dropped=%llu"
		" track_total=%llu"
		" track_drawn=%llu"
		" track_far=%llu"
		" faces=%llu"
		" objects_total=%llu"
		" objects_drawn=%llu"
		" ships=%llu"
		" particles=%llu"
		" uploads=%llu"
		" replacements=%llu"
		" arena_peak=%llu"
		" vsync_wait_avg_ms=%.3f vsync_wait_max_ms=%.3f work_avg_ms=%.3f work_max_ms=%.3f missed_vsyncs=%llu missed_frames=%llu log_sync_vsyncs=%llu log_sync_frames=%llu track_frustum_culled=%llu objects_frustum_culled=%llu\n", kind, g.is_attract_mode ? 1 : g.players, (unsigned long long)s->frames, s->frame_ms / s->frames, s->frame_max,
		s->sum.update_ms / s->frames, s->update_max, s->sum.submit_ms / s->frames, s->submit_max,
		s->sum.wait_ms / s->frames, s->wait_max,
		s->sum.sim_ships_ms / s->frames, s->sim_ships_max,
		s->sum.sim_droid_ms / s->frames, s->sim_droid_max,
		s->sum.sim_camera_ms / s->frames, s->sim_camera_max,
		s->sum.sim_weapons_ms / s->frames, s->sim_weapons_max,
		s->sum.sim_rumble_ms / s->frames, s->sim_rumble_max,
		s->sum.sim_particles_ms / s->frames, s->sim_particles_max,
		s->sum.sim_scene_ms / s->frames, s->sim_scene_max,
		s->sum.sim_map_ms / s->frames, s->sim_map_max,
		s->sum.sim_pickups_ms / s->frames, s->sim_pickups_max,
		s->sum.sim_other_ms / s->frames, s->sim_other_max,
		s->sum.draw_scene_ms / s->frames, s->draw_scene_max,
		s->sum.draw_track_ms / s->frames, s->draw_track_max,
		s->sum.draw_ships_ms / s->frames, s->draw_ships_max,
		s->sum.draw_fx_ms / s->frames, s->draw_fx_max,
		s->sum.draw_2d_ms / s->frames, s->draw_2d_max,
		s->sum.push_ms / s->frames, s->push_max,
		s->sum.draws ? (double)s->sum.tris / s->sum.draws : 0.0,
		(unsigned long long)s->sum.draws,
		(unsigned long long)s->sum.tris,
		(unsigned long long)s->sum.flush_texture,
		(unsigned long long)s->sum.flush_model,
		(unsigned long long)s->sum.flush_state,
		(unsigned long long)s->sum.flush_full,
		(unsigned long long)s->sum.flush_end,
		(unsigned long long)s->sum.flush_other,
		(unsigned long long)s->sum.arena_waits,
		(unsigned long long)s->sum.pushes,
		(unsigned long long)s->sum.dropped,
		(unsigned long long)s->sum.track_total,
		(unsigned long long)s->sum.track_drawn,
		(unsigned long long)s->sum.track_far,
		(unsigned long long)s->sum.faces,
		(unsigned long long)s->sum.objects_total,
		(unsigned long long)s->sum.objects_drawn,
		(unsigned long long)s->sum.ships,
		(unsigned long long)s->sum.particles,
		(unsigned long long)s->sum.uploads,
		(unsigned long long)s->sum.replacements,
		(unsigned long long)s->sum.arena_peak,
		s->sum.vsync_wait_ms / s->frames, s->vsync_wait_max, s->work_ms / s->frames, s->work_max,
		(unsigned long long)s->missed_vsyncs, (unsigned long long)s->missed_frames,
		(unsigned long long)s->log_sync_vsyncs, (unsigned long long)s->log_sync_frames,
		(unsigned long long)s->sum.track_frustum_culled, (unsigned long long)s->sum.objects_frustum_culled);
}

static void profile_print_worst(const char *kind, const profile_stats_t *s) {
	if (!s->has_worst) return;
	const wii_profile_frame_t *f = &s->worst;
	printf("PROF %s frame_ms=%.3f update_ms=%.3f submit_ms=%.3f wait_ms=%.3f"
		" sim_ships_ms=%.3f sim_droid_ms=%.3f sim_camera_ms=%.3f sim_weapons_ms=%.3f"
		" sim_rumble_ms=%.3f sim_particles_ms=%.3f sim_scene_ms=%.3f sim_map_ms=%.3f"
		" sim_pickups_ms=%.3f sim_other_ms=%.3f draw_scene_ms=%.3f draw_track_ms=%.3f"
		" draw_ships_ms=%.3f draw_fx_ms=%.3f draw_2d_ms=%.3f push_ms=%.3f"
		" draws=%llu tris=%llu flush_texture=%llu flush_model=%llu flush_state=%llu flush_full=%llu"
		" flush_end=%llu flush_other=%llu pushes=%llu dropped=%llu track_total=%llu track_drawn=%llu"
		" track_far=%llu faces=%llu objects_total=%llu objects_drawn=%llu ships=%llu particles=%llu"
		" uploads=%llu replacements=%llu arena_waits=%llu arena_peak=%llu"
		" vsync_wait_ms=%.3f retraces=%u missed=%u track_frustum_culled=%llu objects_frustum_culled=%llu\n",
		kind, s->frame_max, f->update_ms, f->submit_ms, f->wait_ms,
		f->sim_ships_ms, f->sim_droid_ms, f->sim_camera_ms, f->sim_weapons_ms,
		f->sim_rumble_ms, f->sim_particles_ms, f->sim_scene_ms, f->sim_map_ms,
		f->sim_pickups_ms, f->sim_other_ms, f->draw_scene_ms, f->draw_track_ms,
		f->draw_ships_ms, f->draw_fx_ms, f->draw_2d_ms, (double)f->push_ticks / (double)TB_TIMER_CLOCK,
		(unsigned long long)f->draws, (unsigned long long)f->tris,
		(unsigned long long)f->flush_texture, (unsigned long long)f->flush_model,
		(unsigned long long)f->flush_state, (unsigned long long)f->flush_full,
		(unsigned long long)f->flush_end, (unsigned long long)f->flush_other,
		(unsigned long long)f->pushes, (unsigned long long)f->dropped,
		(unsigned long long)f->track_total, (unsigned long long)f->track_drawn,
		(unsigned long long)f->track_far, (unsigned long long)f->faces,
		(unsigned long long)f->objects_total, (unsigned long long)f->objects_drawn,
		(unsigned long long)f->ships, (unsigned long long)f->particles,
		(unsigned long long)f->uploads, (unsigned long long)f->replacements,
		(unsigned long long)f->arena_waits, (unsigned long long)f->arena_peak,
		f->vsync_wait_ms, (unsigned int)f->retraces, (unsigned int)f->missed,
		(unsigned long long)f->track_frustum_culled, (unsigned long long)f->objects_frustum_culled);
}

static volatile u32 profile_retrace;
static VIRetraceCallback profile_previous_retrace_callback;

static void profile_retrace_callback(u32 retrace) {
	profile_retrace++;
	if (profile_previous_retrace_callback) profile_previous_retrace_callback(retrace);
}

void wii_profile_flip(void) {
	static u32 previous_retrace;
	static bool have_previous;
	u32 retrace = profile_retrace;
	if (have_previous) {
		wii_profile_frame.retraces = retrace - previous_retrace;
		wii_profile_frame.missed = wii_profile_frame.retraces > 1 ? wii_profile_frame.retraces - 1 : 0;
	}
	previous_retrace = retrace;
	have_previous = true;
}

void wii_profile_begin(void) {
	static bool callback_installed;
	if (!callback_installed) {
		profile_previous_retrace_callback = VIDEO_SetPostRetraceCallback(profile_retrace_callback);
		callback_installed = true;
	}
	memset(&wii_profile_frame, 0, sizeof(wii_profile_frame));
	profile_start = platform_now();
}

void wii_profile_end(void) {
	double elapsed = (platform_now() - profile_start) * 1000.0;
	profile_accumulate(&profile_window, elapsed);
	profile_accumulate(&profile_total, elapsed);
	profile_after_log_sync = false;
	if (profile_window.frames == WII_PROFILE_WINDOW) {
		profile_print("window", &profile_window);
		profile_print_worst("worst", &profile_window);
		platform_wii_log_sync();
		profile_after_log_sync = true;
		memset(&profile_window, 0, sizeof(profile_window));
	}
#if defined(WII_SMOKE) && !defined(WII_SMOKE_INTRO) && defined(WII_PROFILE_FRAMES) && WII_PROFILE_FRAMES > 0
	if (smoke_profile_start && smoke_frame - smoke_profile_start + 1 >= WII_PROFILE_FRAMES) {
		printf("SMOKE PASS: profile frame limit reached, race frames %u, frame %u\n",
			(unsigned int)(smoke_frame - smoke_profile_start + 1), (unsigned int)smoke_frame);
		platform_wii_log_sync();
		power_requested = true;
	}
#endif
}

void wii_profile_total(void) {
	profile_print("total", &profile_total);
	profile_print_worst("worst_total", &profile_total);
	if (profile_total.frames) platform_wii_log_sync();
}
#endif

int main(int argc, char *argv[]) {
	uintptr_t hi = (uintptr_t)SYS_GetArena2Hi();
	uintptr_t lo = (uintptr_t)SYS_GetArena2Lo();
	uintptr_t base = hi >= MEM_HUNK_BYTES ? (hi - MEM_HUNK_BYTES) & ~(uintptr_t)31 : 0;
	if (base < lo || base == 0) {
		platform_video_init();
		printf("ERROR: MEM2 arena is too small for the 16 MB hunk.\n");
		while (true) VIDEO_WaitVSync();
	}
	uintptr_t music_base = base >= WII_MUSIC_BYTES ? (base - WII_MUSIC_BYTES) & ~(uintptr_t)31 : 0;
	if (music_base >= lo && music_base != 0) {
		music_buffer = (uint8_t *)music_base;
		SYS_SetArena2Hi((void *)music_base);
	}
	else {
		SYS_SetArena2Hi((void *)base);
	}
	mem_init((void *)base);
	const char *app_dir = platform_app_dir(argc, argv);

	start_tick = gettime();
	platform_video_init();
	WPAD_Init();
	PAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
	WPAD_SetDataFormat(WPAD_CHAN_1, WPAD_FMT_BTNS_ACC_IR);
	SYS_SetPowerCallback(platform_power_callback);
	SYS_SetResetCallback(platform_reset_callback);
	platform_audio_init();

	bool sd_mounted = dvmProbeMountDiscIface("sd", &__io_wiisd, 4, 64);
	platform_log_init(sd_mounted, app_dir);
	platform_wii_io_init();
	atexit(platform_audio_shutdown);
	const char *data_root = NULL;
	bool usb_mounted = false;
	bool usb_tried = false;
	static char app_data[300];
	if (app_dir) {
		bool on_sd = strncmp(app_dir, "sd:/", 4) == 0;
		bool on_usb = strncmp(app_dir, "usb:/", 5) == 0;
		if (on_usb) {
			usb_mounted = platform_mount_usb();
			usb_tried = true;
		}
		snprintf(app_data, sizeof(app_data), "%swipeout", app_dir);
		if (((on_sd && sd_mounted) || (on_usb && usb_mounted)) && platform_data_root_exists(app_data)) {
			data_root = app_dir;
		}
	}
	if (!data_root && sd_mounted && platform_data_root_exists("sd:/apps/wiipeout/wipeout")) {
		data_root = "sd:/apps/wiipeout/";
	}
#if !defined(PATH_ASSETS) || !defined(PATH_USERDATA)
	if (!data_root) {
		if (!usb_tried) usb_mounted = platform_mount_usb();
		if (usb_mounted && platform_data_root_exists("usb:/apps/wiipeout/wipeout")) {
			data_root = "usb:/apps/wiipeout/";
		}
	}
	if (!data_root) {
		if (app_dir) printf("ERROR: Data directory not found: %swipeout\n", app_dir);
		printf("ERROR: Data directory not found: sd:/apps/wiipeout/wipeout or usb:/apps/wiipeout/wipeout. Press HOME to exit.\n");
		platform_wii_log_sync();
		while (!wants_to_exit && !power_requested && !reset_requested) {
			platform_pump_events();
			VIDEO_WaitVSync();
		}
		platform_rumble_off();
		platform_audio_shutdown();
		platform_wii_io_shutdown();
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
#ifdef WII_DIAG
	diag_init();
#endif
	if (!wants_to_exit && !power_requested && !reset_requested) {
		system_init();
		while (!wants_to_exit && !power_requested && !reset_requested) {
			PROF_BEGIN();
			WII_DIAG_PHASE(WII_PHASE_INPUT);
			platform_pump_events();
			if (wants_to_exit || power_requested || reset_requested) break;
			platform_prepare_frame();
			WII_DIAG_PHASE(WII_PHASE_UPDATE);
			system_update();
			WII_DIAG_PHASE(WII_PHASE_FRAME_END);
			platform_end_frame();
			PROF_END();
#ifdef WII_DIAG
			diag_frame_done();
#endif
		}
		system_cleanup();
	}
	PROF_TOTAL();
	platform_rumble_off();
	platform_audio_shutdown();
	platform_wii_io_shutdown();
	platform_log_close();
	if (power_requested) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	exit(0);
}
