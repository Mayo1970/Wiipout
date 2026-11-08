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
static float rumble_strength;
static float rumble_acc;
static bool rumble_on;

// libogc's default speaker config (0x07D0) plays 4-bit ADPCM at 12 MHz / 2000 = 6000 Hz.
#define WIIMOTE_SPEAKER_RATE 6000
#define WIIMOTE_SPEAKER_SAMPLES_PER_PACKET 40
#define WIIMOTE_SPEAKER_PACKET_BYTES 20
#define WIIMOTE_SPEAKER_MAX_BYTES 4096
#define WIIMOTE_SPEAKER_RETRY_FRAMES 30

static uint8_t wiimote_speaker_data[WIIMOTE_SPEAKER_MAX_BYTES];
static uint32_t wiimote_speaker_data_len;
static const int16_t *wiimote_speaker_source;
static uint32_t wiimote_speaker_source_len;
static uint32_t wiimote_speaker_source_rate;
static bool wiimote_speaker_encoded;
static bool wiimote_speaker_connected;
static bool wiimote_speaker_on;
static bool wiimote_speaker_pending;
static uint32_t wiimote_speaker_retry;

static u64 start_tick;
static FILE *log_file;
static bool log_writing;
static const devoptab_t *console_stdout;
static const devoptab_t *console_stderr;
static devoptab_t log_stdout;
static devoptab_t log_stderr;

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
		if (audio_mix_cb) audio_mix_cb(audio_scratch, frames * 2);
		else memset(audio_scratch, 0, frames * 2 * sizeof(float));
		for (u32 i = 0; i < frames * 2; i++) {
			float sample = fmaxf(-1.0f, fminf(audio_scratch[i], 1.0f));
			audio_ring[offset * 2 + i] = (int16_t)(sample * 32767.0f);
		}
		audio_write_frame += frames;
	}
	DCFlushRange(audio_ring, AUDIO_RING_BYTES);
}

static void platform_audio_shutdown(void) {
	if (!audio_ready) return;
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
#ifdef WII_SMOKE_INTRO
	// Time, not frames: the GX loop has no vsync wait, so frame counts say nothing about duration.
	if (smoke_intro_seen && platform_now() - smoke_intro_start >= 240.0) {
		printf("SMOKE FAIL: intro timeout\n");
		platform_wii_log_sync();
		power_requested = true;
	}
#else
#ifdef WII_SMOKE_FULL
	// Time, not frames: the GX loop has no vsync wait, so frame counts say nothing about duration.
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
	if (smoke_race_seen) {
		ship_t *ship = &g.ships[g.pilot];
		// AI drives the player ship; flags stay untouched so the HUD still draws.
		if (ship->update_func == ship_player_update_race) {
			ship->update_func = ship_ai_update_race;
			printf("SMOKE autopilot frame %u\n", (unsigned int)smoke_frame);
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

static void platform_wiimote_speaker_encode(const int16_t *samples, uint32_t len, uint32_t rate) {
	if (wiimote_speaker_encoded && samples == wiimote_speaker_source &&
		len == wiimote_speaker_source_len && rate == wiimote_speaker_source_rate) {
		return;
	}

	uint32_t out_len = (uint32_t)(((uint64_t)len * WIIMOTE_SPEAKER_RATE) / rate);
	uint32_t chunks = (out_len + WIIMOTE_SPEAKER_SAMPLES_PER_PACKET - 1) / WIIMOTE_SPEAKER_SAMPLES_PER_PACKET;
	uint32_t max_chunks = sizeof(wiimote_speaker_data) / WIIMOTE_SPEAKER_PACKET_BYTES;
	if (chunks > max_chunks) chunks = max_chunks;

	uint32_t step = (uint32_t)(((uint64_t)rate << 16) / WIIMOTE_SPEAKER_RATE);
	uint64_t pos = 0;
	int16_t pcm[WIIMOTE_SPEAKER_SAMPLES_PER_PACKET];
	WPADEncStatus enc;
	memset(&enc, 0, sizeof(enc));

	for (uint32_t c = 0; c < chunks; c++) {
		for (uint32_t i = 0; i < WIIMOTE_SPEAKER_SAMPLES_PER_PACKET; i++) {
			uint32_t out_index = c * WIIMOTE_SPEAKER_SAMPLES_PER_PACKET + i;
			if (out_index >= out_len) {
				pcm[i] = 0;
				continue;
			}
			uint32_t index = (uint32_t)(pos >> 16);
			uint32_t frac = (uint32_t)(pos & 0xffff);
			int32_t s0 = index < len ? samples[index] : 0;
			int32_t s1 = index + 1 < len ? samples[index + 1] : s0;
			pcm[i] = (int16_t)(s0 + (((s1 - s0) * (int32_t)frac) >> 16));
			pos += step;
		}
		WPAD_EncodeData(&enc, c == 0 ? WPAD_ENC_FIRST : WPAD_ENC_CONT, pcm,
			WIIMOTE_SPEAKER_SAMPLES_PER_PACKET, &wiimote_speaker_data[c * WIIMOTE_SPEAKER_PACKET_BYTES]);
	}

	wiimote_speaker_data_len = chunks * WIIMOTE_SPEAKER_PACKET_BYTES;
	wiimote_speaker_source = samples;
	wiimote_speaker_source_len = len;
	wiimote_speaker_source_rate = rate;
	wiimote_speaker_encoded = true;
}

static void platform_wiimote_speaker_start(void) {
	if (!wiimote_speaker_on || !wiimote_speaker_data_len) return;
	wiimote_speaker_pending = false;
	// libogc streams this buffer as 20-byte packets at the speaker rate.
	WPAD_SendStreamData(WPAD_CHAN_0, wiimote_speaker_data, wiimote_speaker_data_len);
}

void platform_wiimote_speaker_play(const int16_t *samples, uint32_t len, uint32_t rate) {
	if (!samples || !len || !rate) return;
	platform_wiimote_speaker_encode(samples, len, rate);
	if (!wiimote_speaker_data_len) return;
	wiimote_speaker_pending = true;
	if (wiimote_speaker_on) platform_wiimote_speaker_start();
}

static void platform_wiimote_speaker_update(bool connected) {
	if (connected) {
		if (!wiimote_speaker_connected) {
			wiimote_speaker_connected = true;
			wiimote_speaker_on = false;
			wiimote_speaker_retry = 0;
		}
		if (!wiimote_speaker_on) {
			if (wiimote_speaker_retry == 0) {
				WPAD_ControlSpeaker(WPAD_CHAN_0, 1);
				wiimote_speaker_retry = WIIMOTE_SPEAKER_RETRY_FRAMES;
			}
			else {
				wiimote_speaker_retry--;
			}
			if (WPAD_IsSpeakerEnabled(WPAD_CHAN_0) == WPAD_ERR_NONE) {
				wiimote_speaker_on = true;
			}
		}
	}
	else {
		wiimote_speaker_connected = false;
		wiimote_speaker_on = false;
		wiimote_speaker_pending = false;
		wiimote_speaker_retry = 0;
	}
	if (wiimote_speaker_on && wiimote_speaker_pending) platform_wiimote_speaker_start();
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
	platform_wiimote_speaker_update(connected);
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

static void platform_end_frame(void) {
	platform_audio_update();
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
	audio_mix_cb = cb;
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
	char path[256];
	snprintf(path, sizeof(path), "%s%s", path_userdata, name);
	if (!file_exists(path)) {
		// A power cut between remove() and rename() in store leaves only the tmp file
		snprintf(path, sizeof(path), "%s%s.tmp", path_userdata, name);
	}
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
	char path[256];
	char tmp[256];
	snprintf(path, sizeof(path), "%s%s", path_userdata, name);
	snprintf(tmp, sizeof(tmp), "%s%s.tmp", path_userdata, name);

	FILE *f = fopen(tmp, "wb");
	if (!f) {
		printf("save: write failed (open %s)\n", tmp);
		platform_wii_log_sync();
		return 0;
	}
	size_t written = fwrite(bytes, 1, len, f);
	int closed = fclose(f);
	if (written != (size_t)len || closed != 0) {
		remove(tmp);
		printf("save: write failed (write %s)\n", tmp);
		platform_wii_log_sync();
		return 0;
	}

	// FAT rename does not overwrite an existing file
	remove(path);
	if (rename(tmp, path) != 0) {
		printf("save: write failed (rename %s)\n", tmp);
		platform_wii_log_sync();
		return 0;
	}
	platform_wii_log_sync();
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
	platform_audio_init();

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
		platform_rumble_off();
		platform_audio_shutdown();
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
	platform_rumble_off();
	platform_audio_shutdown();
	platform_log_close();
	if (power_requested) SYS_ResetSystem(SYS_POWEROFF, 0, 0);
	exit(0);
}
