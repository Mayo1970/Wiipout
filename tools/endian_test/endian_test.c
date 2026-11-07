#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/mem.h"
#include "../../src/platform.h"
#include "../../src/utils.h"
#include "../../src/render.h"
#include "../../src/system.h"
#include "../../src/wipeout/camera.h"
#include "../../src/wipeout/game.h"
#include "../../src/wipeout/image.h"
#include "../../src/wipeout/object.h"
#include "../../src/wipeout/track.h"

#define ARENA_SIZE (4 * 1024 * 1024)

static union {
	double double_align;
	void *pointer_align;
	uint8_t bytes[ARENA_SIZE];
} arena;
static uint32_t arena_used;

game_t g;
const game_def_t def = {
	.circuits = {
		[CIRCUIT_ALTIMA_VII] = {.release = GAME_WIPEOUT}
	}
};

static void hash_byte(uint64_t *hash, uint8_t value) {
	*hash = (*hash ^ value) * UINT64_C(1099511628211);
}

static void hash_u32(uint64_t *hash, uint32_t value) {
	for (int shift = 24; shift >= 0; shift -= 8) {
		hash_byte(hash, (uint8_t)(value >> shift));
	}
}

static void hash_i32(uint64_t *hash, int32_t value) {
	hash_u32(hash, (uint32_t)value);
}

static void hash_float(uint64_t *hash, float value) {
	uint64_t fixed = (uint64_t)(int64_t)((double)value * 4096.0);
	hash_u32(hash, (uint32_t)(fixed >> 32));
	hash_u32(hash, (uint32_t)fixed);
}

static void hash_color(uint64_t *hash, rgba_t color) {
	hash_byte(hash, color.r);
	hash_byte(hash, color.g);
	hash_byte(hash, color.b);
	hash_byte(hash, color.a);
}

static void hash_vec3(uint64_t *hash, vec3_t value) {
	hash_float(hash, value.x);
	hash_float(hash, value.y);
	hash_float(hash, value.z);
}

static void hash_primitive_vertex(uint64_t *hash, primitive_vertex_t vertex) {
	hash_i32(hash, vertex.coord);
	hash_byte(hash, vertex.u);
	hash_byte(hash, vertex.v);
	hash_color(hash, vertex.color);
}

static uint8_t *read_file(const char *path, uint32_t *size) {
	FILE *file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "cannot open %s\n", path);
		exit(1);
	}
	if (fseek(file, 0, SEEK_END) != 0) {
		fprintf(stderr, "cannot seek %s\n", path);
		exit(1);
	}
	long length = ftell(file);
	if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fprintf(stderr, "cannot size %s\n", path);
		exit(1);
	}
	uint8_t *bytes = malloc((size_t)length);
	if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(1);
	}
	fclose(file);
	*size = (uint32_t)length;
	return bytes;
}

void *mem_mark(void) {
	return &arena.bytes[arena_used];
}

void *mem_bump(uint32_t size) {
	size = (size + 7) & ~7U;
	if (arena_used + size > ARENA_SIZE) {
		fprintf(stderr, "test arena exhausted\n");
		exit(1);
	}
	void *result = &arena.bytes[arena_used];
	memset(result, 0, size);
	arena_used += size;
	return result;
}

void mem_reset(void *mark) {
	arena_used = (uint8_t *)mark - arena.bytes;
}

void *mem_temp_alloc(uint32_t size) {
	void *result = malloc(size);
	if (!result) {
		fprintf(stderr, "test temporary allocation failed\n");
		exit(1);
	}
	return result;
}

void mem_temp_free(void *ptr) {
	free(ptr);
}

uint8_t *platform_load_asset(const char *name, uint32_t *bytes_read) {
	return read_file(name, bytes_read);
}

void render_set_model_mat(mat4_t *mat) {
	(void)mat;
}

void render_push_tris(tris_t tris, uint16_t texture) {
	(void)tris;
	(void)texture;
}

void render_push_sprite(vec3_t pos, vec2i_t size, rgba_t color, uint16_t texture) {
	(void)pos;
	(void)size;
	(void)color;
	(void)texture;
}

uint16_t render_texture_create(uint32_t width, uint32_t height, rgba_t *pixels) {
	(void)width;
	(void)height;
	(void)pixels;
	return 0;
}

uint16_t render_textures_len(void) {
	return 0;
}

vec3_t camera_forward(camera_t *camera) {
	(void)camera;
	return vec3(0, 0, 1);
}

double system_cycle_time(void) {
	return 0;
}

double system_tick(void) {
	return 0;
}

static void dump_image(void) {
	uint32_t size;
	uint8_t *bytes = read_file("wipeout/textures/speedo.tim", &size);
	image_t *image = image_load_from_bytes(bytes, false);
	uint64_t hash = UINT64_C(14695981039346656037);
	hash_u32(&hash, image->width);
	hash_u32(&hash, image->height);
	for (uint32_t i = 0; i < image->width * image->height; i++) {
		hash_color(&hash, image->pixels[i]);
	}
	printf("image speedo.tim %ux%u hash=%016" PRIx64 " first=%u,%u,%u,%u\n",
		image->width, image->height, hash,
		image->pixels[0].r, image->pixels[0].g,
		image->pixels[0].b, image->pixels[0].a);
	mem_temp_free(image);
	free(bytes);
}

static void dump_track(void) {
	g.circuit = CIRCUIT_ALTIMA_VII;
	vec3_t *vertices = track_load_vertices("wipeout/track01/track.trv");
	track_load_faces("wipeout/track01/track.trf", vertices);
	track_load_sections("wipeout/track01/track.trs");
	uint64_t hash = UINT64_C(14695981039346656037);
	hash_u32(&hash, g.track.vertex_count);
	hash_u32(&hash, g.track.face_count);
	hash_u32(&hash, g.track.section_count);
	for (int i = 0; i < g.track.vertex_count; i++) {
		hash_vec3(&hash, vertices[i]);
	}
	for (int i = 0; i < g.track.face_count; i++) {
		track_face_t *face = &g.track.faces[i];
		hash_byte(&hash, face->flags);
		hash_byte(&hash, face->texture);
		hash_vec3(&hash, face->normal);
		for (int t = 0; t < 2; t++) {
			for (int v = 0; v < 3; v++) {
				vertex_t *vertex = &face->tris[t].vertices[v];
				hash_vec3(&hash, vertex->pos);
				hash_float(&hash, vertex->uv.x);
				hash_float(&hash, vertex->uv.y);
				hash_color(&hash, vertex->color);
			}
		}
	}
	for (int i = 0; i < g.track.section_count; i++) {
		section_t *section = &g.track.sections[i];
		hash_i32(&hash, section->junction ? (int32_t)(section->junction - g.track.sections) : -1);
		hash_i32(&hash, (int32_t)(section->prev - g.track.sections));
		hash_i32(&hash, (int32_t)(section->next - g.track.sections));
		hash_vec3(&hash, section->center);
		hash_i32(&hash, section->face_start);
		hash_i32(&hash, section->face_count);
		hash_i32(&hash, section->flags);
		hash_i32(&hash, section->num);
	}
	track_face_t *face = &g.track.faces[0];
	section_t *section = &g.track.sections[0];
	printf("track vertices=%d faces=%d sections=%d hash=%016" PRIx64
		" v0=%.0f,%.0f,%.0f face0=%u,%u,%.5f,%.5f,%.5f"
		" section0=%d,%d,%d flags=%d\n",
		g.track.vertex_count, g.track.face_count, g.track.section_count, hash,
		vertices[0].x, vertices[0].y, vertices[0].z,
		face->flags, face->texture, face->normal.x, face->normal.y, face->normal.z,
		section->face_start, section->face_count,
		(int32_t)(section->next - g.track.sections), section->flags);
	mem_temp_free(vertices);
}

static uint64_t hash_objects(Object *objects, uint32_t *count_out) {
	uint64_t hash = UINT64_C(14695981039346656037);
	uint32_t count = 0;
	for (Object *object = objects; object; object = object->next) {
		count++;
		for (int i = 0; i < 16; i++) hash_byte(&hash, (uint8_t)object->name[i]);
		hash_i32(&hash, object->vertices_len);
		hash_i32(&hash, object->primitives_len);
		hash_vec3(&hash, object->origin);
		for (int i = 0; i < object->vertices_len; i++) hash_vec3(&hash, object->vertices[i]);
		for (int i = 0; i < object->primitives_len; i++) {
			primitive_t *primitive = &object->primitives[i];
			hash_byte(&hash, (uint8_t)primitive->type);
			hash_byte(&hash, (uint8_t)primitive->flag);
			if (primitive->type == PRM_TYPE_TRI) {
				hash_i32(&hash, primitive->u.tri.texture);
				for (int v = 0; v < 3; v++) hash_primitive_vertex(&hash, primitive->u.tri.v[v]);
			}
			else if (primitive->type == PRM_TYPE_QUAD) {
				hash_i32(&hash, primitive->u.quad.texture);
				for (int v = 0; v < 4; v++) hash_primitive_vertex(&hash, primitive->u.quad.v[v]);
			}
			else {
				hash_i32(&hash, primitive->u.spr.texture);
				hash_i32(&hash, primitive->u.spr.coord);
				hash_i32(&hash, primitive->u.spr.width);
				hash_i32(&hash, primitive->u.spr.height);
				hash_color(&hash, primitive->u.spr.color);
			}
		}
	}
	*count_out = count;
	return hash;
}

static void dump_objects(void) {
	texture_list_t textures = {.start = 0, .len = UINT16_MAX};
	Object *objects = objects_load("wipeout/common/allsh.prm", textures);
	uint32_t count;
	uint64_t hash = hash_objects(objects, &count);
	Object *first = objects;
	printf("object allsh.prm objects=%u hash=%016" PRIx64 " first=%.16s vertices=%d primitives=%d",
		count, hash, first->name, first->vertices_len, first->primitives_len);
	if (first->vertices_len > 0) {
		printf(" v0=%.0f,%.0f,%.0f", first->vertices[0].x,
			first->vertices[0].y, first->vertices[0].z);
	}
	if (first->primitives_len > 0) {
		printf(" p0=%d,%d", first->primitives[0].type, first->primitives[0].flag);
	}
	printf("\n");
}

int main(void) {
	dump_image();
	dump_track();
	dump_objects();
	return 0;
}
