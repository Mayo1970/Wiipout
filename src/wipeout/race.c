#include "../wii_profile.h"
#include "../input.h"
#include "../system.h"
#include "../utils.h"
#include "../render.h"

#include "track.h"
#include "ship.h"
#include "map.h"
#include "weapon.h"
#include "droid.h"
#include "camera.h"
#include "scene.h"
#include "rumble.h"
#include "game.h"
#include "hud.h"
#include "sfx.h"
#include "race.h"
#include "particle.h"
#include "menu.h"
#include "ingame_menus.h"

#define ATTRACT_DURATION 60.0

static bool is_paused = false;
static bool menu_is_scroll_text = false;
static bool has_show_credits = false;
static float attract_start_time;
static menu_t *active_menu = NULL;

void race_init(void) {
	map_init();
	ingame_menus_load();
	menu_is_scroll_text = false;

	const circuit_settings_t *cs = &def.circuits[g.circuit].settings[g.race_class];
	track_load(cs->path);
	scene_load(cs->path, cs->sky_y_offset);
	
	if (g.circuit == CIRCUIT_SILVERSTREAM && g.race_class == RACE_CLASS_RAPIER) {
		scene_init_aurora_borealis();	
	} 

	if (g.is_attract_mode) {
		g.pilot = rand_int(0, len(def.pilots));
	}
	race_start();
	// render_textures_dump("texture_atlas.png");

	if (g.is_attract_mode) {
		attract_start_time = system_time();

		for (int i = 0; i < len(g.ships); i++) {
			flags_rm(g.ships[i].flags, SHIP_VIEW_INTERNAL);
			flags_rm(g.ships[i].flags, SHIP_RACING);
		}

		g.camera.update_func = camera_update_attract_random;
		if (!has_show_credits || rand_int(0, 10) == 0) {
			active_menu = text_scroll_menu_init(def.credits, len(def.credits));
			menu_is_scroll_text = true;
			has_show_credits = true;
		}
	}

	is_paused = false;
}

void race_update(void) {
	if (is_paused) {
		if (!active_menu) {
			active_menu = pause_menu_init();
		}
		if (input_pressed(A_MENU_QUIT)) {
			race_unpause();
		}
	}
	else {
		PROF_WAIT_SNAPSHOT(sim_ships_wait);
		PROF_TIME_START(sim_ships_start);
		ships_update();
		PROF_TIME_ADD(sim_ships_ms, sim_ships_start);
		PROF_WAIT_SUBTRACT(sim_ships_ms, sim_ships_wait);
		PROF_WAIT_SNAPSHOT(sim_droid_wait);
		PROF_TIME_START(sim_droid_start);
		droid_update(&g.droid, &g.ships[g.pilot]);
		PROF_TIME_ADD(sim_droid_ms, sim_droid_start);
		PROF_WAIT_SUBTRACT(sim_droid_ms, sim_droid_wait);
		PROF_WAIT_SNAPSHOT(sim_camera_wait);
		PROF_TIME_START(sim_camera_start);
		camera_update(&g.camera, &g.ships[g.pilot], &g.droid);
		PROF_TIME_ADD(sim_camera_ms, sim_camera_start);
		PROF_WAIT_SUBTRACT(sim_camera_ms, sim_camera_wait);
		PROF_WAIT_SNAPSHOT(sim_weapons_wait);
		PROF_TIME_START(sim_weapons_start);
		weapons_update();
		PROF_TIME_ADD(sim_weapons_ms, sim_weapons_start);
		PROF_WAIT_SUBTRACT(sim_weapons_ms, sim_weapons_wait);
		PROF_WAIT_SNAPSHOT(sim_other_wait);
		PROF_TIME_START(sim_other_start);
		rumble_update();
		particles_update();
		scene_update();
		map_update();
		if (g.race_type != RACE_TYPE_TIME_TRIAL) {
			track_cycle_pickups();
		}
		PROF_TIME_ADD(sim_other_ms, sim_other_start);
		PROF_WAIT_SUBTRACT(sim_other_ms, sim_other_wait);

		if (g.is_attract_mode) {
			if (input_pressed(A_MENU_START) || input_pressed(A_MENU_SELECT)) {
				game_set_scene(GAME_SCENE_MAIN_MENU);
			}
			float duration = system_time() - attract_start_time;
			if ((!active_menu && duration > 30) || duration > 120) {
				game_set_scene(GAME_SCENE_TITLE);
			}
		}
		else if (active_menu == NULL && (input_pressed(A_MENU_START) || input_pressed(A_MENU_QUIT))) {
			race_pause();
		}
	}


	// Draw 3D
	render_set_view(g.camera.position, g.camera.angle);
	render_set_screen_position(g.camera.shake);

	PROF_WAIT_SNAPSHOT(draw_scene_wait);
	PROF_TIME_START(draw_scene_start);
	scene_draw(&g.camera);
	PROF_TIME_ADD(draw_scene_ms, draw_scene_start);
	PROF_WAIT_SUBTRACT(draw_scene_ms, draw_scene_wait);
	render_set_cull_backface(false);
	PROF_WAIT_SNAPSHOT(draw_track_wait);
	PROF_TIME_START(draw_track_start);
	track_draw(&g.camera);
	PROF_TIME_ADD(draw_track_ms, draw_track_start);
	PROF_WAIT_SUBTRACT(draw_track_ms, draw_track_wait);
	render_set_cull_backface(true);

	PROF_WAIT_SNAPSHOT(draw_ships_wait);
	PROF_TIME_START(draw_ships_start);
	ships_draw();
	PROF_TIME_ADD(draw_ships_ms, draw_ships_start);
	PROF_WAIT_SUBTRACT(draw_ships_ms, draw_ships_wait);
	PROF_WAIT_SNAPSHOT(draw_fx_wait);
	PROF_TIME_START(draw_fx_start);
	droid_draw(&g.droid);
	weapons_draw();
	particles_draw();
	PROF_TIME_ADD(draw_fx_ms, draw_fx_start);
	PROF_WAIT_SUBTRACT(draw_fx_ms, draw_fx_wait);

	// Draw 2d
	render_set_screen_position(vec2(0,0));
	render_set_view_2d();
	PROF_WAIT_SNAPSHOT(draw_2d_wait);
	PROF_TIME_START(draw_2d_start);

	if (g.is_attract_mode && !active_menu) {
		ui_draw_text("DEMO MODE", ui_scaled_pos(UI_POS_TOP | UI_POS_CENTER, vec2i(-56, 24)), UI_SIZE_8, UI_COLOR_ACCENT);
	}

	if (flags_is(g.ships[g.pilot].flags, SHIP_RACING)) {
		hud_draw(&g.ships[g.pilot]);
		map_draw(&g.track, g.ships, len(g.ships), g.pilot);
	}

	if (active_menu) {
		if (!menu_is_scroll_text) {
			vec2i_t size = render_size();
			render_push_2d(vec2i(0, 0), size, rgba(0, 0, 0, 128), RENDER_NO_TEXTURE);
		}
		menu_update(active_menu);
	}
	PROF_TIME_ADD(draw_2d_ms, draw_2d_start);
	PROF_WAIT_SUBTRACT(draw_2d_ms, draw_2d_wait);
}

void race_start(void) {
	active_menu = NULL;
	sfx_reset();
	scene_init();
	camera_init(&g.camera, g.track.sections);
	g.camera.update_func = camera_update_race_intro;
	ships_init(g.track.sections);
	droid_init(&g.droid, &g.ships[g.pilot]);
	particles_init();
	weapons_init();

	for (int i = 0; i < len(g.race_ranks); i++) {
		g.race_ranks[i].points = 0;
		g.race_ranks[i].pilot = i;
	}
	for (int i = 0; i < len(g.lap_times); i++) {
		for (int j = 0; j < len(g.lap_times[i]); j++) {
			g.lap_times[i][j] = 0;
		}
	}
	g.is_new_race_record = false;
	g.is_new_lap_record = false;
	g.best_lap = 0;
	g.race_time = 0;
}

void race_restart(void) {
	race_unpause();

	if (g.race_type == RACE_TYPE_CHAMPIONSHIP) {
		g.lives--;
		if (g.lives == 0) {
			race_release_control();
			active_menu = game_over_menu_init();
			return;
		}
	}

	race_start();
}

static bool sort_points_compare(pilot_points_t *pa, pilot_points_t *pb) {
	return (pa->points < pb->points);
}

void race_end(void) {
	rumble_stop();
	race_release_control();

	g.race_position = g.ships[g.pilot].position_rank;

	g.race_time = 0;
	g.best_lap = g.lap_times[g.pilot][0];
	for (int i = 0; i < NUM_LAPS; i++) {
		g.race_time += g.lap_times[g.pilot][i];
		if (g.lap_times[g.pilot][i] < g.best_lap) {
			g.best_lap = g.lap_times[g.pilot][i];
		}
	}

	highscores_t *hs = &save.highscores[g.race_class][g.circuit][g.highscore_tab];
	if (g.best_lap < hs->lap_record) {
		hs->lap_record = g.best_lap;
		g.is_new_lap_record = true;
		save.is_dirty = true;
	}

	for (int i = 0; i < NUM_HIGHSCORES; i++) {
		if (g.race_time < hs->entries[i].time) {
			g.is_new_race_record = true;
			break;
		}
	}

	if (g.race_type == RACE_TYPE_CHAMPIONSHIP) {
		for (int i = 0; i < len(def.race_points_for_rank); i++) {
			g.race_ranks[i].points = def.race_points_for_rank[i];

			// Find the pilot for this race rank in the championship table
			for (int j = 0; j < len(g.championship_ranks); j++) {
				if (g.race_ranks[i].pilot == g.championship_ranks[j].pilot) {
					g.championship_ranks[j].points += def.race_points_for_rank[i];
					break;
				}
			}
		}
		sort(g.championship_ranks, len(g.championship_ranks), sort_points_compare);
	}

	active_menu = race_stats_menu_init();
}

void race_next(void) {
	int next_circuit = g.circuit + 1;

	// Championship complete
	if (
		(save.has_bonus_circuits && next_circuit >= NUM_WIPEOUT_CIRCUITS) ||
		(!save.has_bonus_circuits && next_circuit >= NUM_NON_BONUS_CIRCUITS)
	) {
		if (g.race_class == RACE_CLASS_RAPIER) {
			if (save.has_bonus_circuits) {
				active_menu = text_scroll_menu_init(def.congratulations.rapier_all_circuits, len(def.congratulations.rapier_all_circuits));
			}
			else {
				save.has_bonus_circuits = true;
				active_menu = text_scroll_menu_init(def.congratulations.rapier, len(def.congratulations.rapier));
			}
		}
		else {
			save.has_rapier_class = true;
			if (save.has_bonus_circuits) {
				active_menu = text_scroll_menu_init(def.congratulations.venom_all_circuits, len(def.congratulations.venom_all_circuits));
			}
			else {
				active_menu = text_scroll_menu_init(def.congratulations.venom, len(def.congratulations.venom));
			}
		}
		save.is_dirty = true;
		menu_is_scroll_text = true;
	}

	// Next track
	else {
		g.circuit = next_circuit;
		game_set_scene(GAME_SCENE_RACE);
	}
}

void race_release_control(void) {
	flags_rm(g.ships[g.pilot].flags, SHIP_RACING);
	g.ships[g.pilot].remote_thrust_max = 3160;
	g.ships[g.pilot].remote_thrust_mag = 32;
	g.ships[g.pilot].speed = 3160;
	g.camera.update_func = camera_update_attract_random;
}

void race_pause(void) {
	rumble_stop();
	sfx_pause();
	is_paused = true;
}

void race_unpause(void) {
	sfx_unpause();
	is_paused = false;
	active_menu = NULL;
}
