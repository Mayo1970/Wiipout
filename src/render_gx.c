#include <gccore.h>
#include <malloc.h>
#include <string.h>

#include "render.h"
#include "platform_wii.h"
#include "utils.h"

#define TEXTURES_MAX 1024
#define TRIS_MAX 2048
#define FIFO_SIZE (256 * 1024)

typedef struct {
	GXTexObj obj;
	void *data;
	uint32_t width, height, padded_width, padded_height;
	u8 format;
} gx_texture_t;

uint16_t RENDER_NO_TEXTURE;
static gx_texture_t textures[TEXTURES_MAX];
static uint16_t textures_len;
static tris_t tris_buffer[TRIS_MAX];
static uint32_t tris_len;
static uint16_t batch_texture;
static vec2i_t screen_size;
static vec2_t screen_position;
static render_blend_mode_t blend_mode;
static render_stats_t running_stats, end_stats;
static bool view_2d;
static void *fifo;
static int back = 1;
static mat4_t model_mat;
static bool depth_write, depth_test, cull_backface;
static float depth_offset;

static void render_flush(void) {
	if (!tris_len) return;
	gx_texture_t *texture = &textures[batch_texture];
	GX_LoadTexObj(&texture->obj, GX_TEXMAP0);
	GX_Begin(GX_TRIANGLES, GX_VTXFMT0, tris_len * 3);
	for (uint32_t i = 0; i < tris_len; i++) {
		for (int j = 0; j < 3; j++) {
			vertex_t *v = &tris_buffer[i].vertices[j];
			GX_Position3f32(v->pos.x, v->pos.y, v->pos.z);
			GX_Color4u8(v->color.r, v->color.g, v->color.b, v->color.a);
			GX_TexCoord2f32(v->uv.x / texture->padded_width, v->uv.y / texture->padded_height);
		}
	}
	GX_End();
	running_stats.num_tris += tris_len;
	running_stats.num_draw_calls++;
	tris_len = 0;
}

static void set_projection(void) {
	Mtx44 projection;
	guOrtho(projection, 0, screen_size.y, 0, screen_size.x, -1, 1);
	projection[0][3] += screen_position.x;
	projection[1][3] -= screen_position.y;
	GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
}

static void encode_texture(gx_texture_t *t, rgba_t *pixels) {
	bool alpha = !t->width || !t->height;
	for (uint32_t i = 0; i < t->width * t->height; i++) {
		if (pixels[i].a < 255) { alpha = true; break; }
	}
	t->format = alpha ? GX_TF_RGB5A3 : GX_TF_RGB565;
	uint16_t *out = t->data;
	// Upstream accepts empty images; keep them as one fully transparent 4x4 tile.
	if (!t->width || !t->height) memset(out, 0, t->padded_width * t->padded_height * 2);
	else for (uint32_t ty = 0; ty < t->padded_height; ty += 4) {
		for (uint32_t tx = 0; tx < t->padded_width; tx += 4) {
			for (uint32_t y = 0; y < 4; y++) {
				for (uint32_t x = 0; x < 4; x++) {
					uint32_t px = tx + x < t->width ? tx + x : t->width - 1;
					uint32_t py = ty + y < t->height ? ty + y : t->height - 1;
					rgba_t p = pixels[py * t->width + px];
					if (!alpha) *out++ = ((p.r >> 3) << 11) | ((p.g >> 2) << 5) | (p.b >> 3);
					else if (p.a >= 224) *out++ = 0x8000 | ((p.r >> 3) << 10) | ((p.g >> 3) << 5) | (p.b >> 3);
					else *out++ = ((p.a >> 5) << 12) | ((p.r >> 4) << 8) | ((p.g >> 4) << 4) | (p.b >> 4);
				}
			}
		}
	}
	DCFlushRange(t->data, t->padded_width * t->padded_height * 2);
	GX_InitTexObj(&t->obj, t->data, t->padded_width, t->padded_height, t->format, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&t->obj, GX_LINEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GX_InvalidateTexAll();
}

void render_init(vec2i_t size) {
	GXRModeObj *mode = platform_wii_video_mode();
	platform_wii_console_detach();
	fifo = memalign(32, FIFO_SIZE);
	error_if(!fifo, "GX FIFO allocation failed");
	memset(fifo, 0, FIFO_SIZE);
	GX_Init(fifo, FIFO_SIZE);
	GX_SetCopyClear((GXColor){0, 0, 0, 255}, GX_MAX_Z24);
	GX_SetViewport(0, 0, mode->fbWidth, mode->efbHeight, 0, 1);
	GX_SetScissor(0, 0, mode->fbWidth, mode->efbHeight);
	f32 scale = GX_GetYScaleFactor(mode->efbHeight, mode->xfbHeight);
	u32 lines = GX_SetDispCopyYScale(scale);
	GX_SetDispCopySrc(0, 0, mode->fbWidth, mode->efbHeight);
	GX_SetDispCopyDst(mode->fbWidth, lines);
	GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
	GX_SetFieldMode(mode->field_rendering, mode->viHeight == 2 * mode->xfbHeight);
	GX_SetPixelFmt(mode->aa ? GX_PF_RGBA6_Z24 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	GX_SetDispCopyGamma(GX_GM_1_0);
	GX_SetCullMode(GX_CULL_NONE);
	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(1);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
	GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_TRUE, GX_TEVPREV);
	GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
	GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GX_SetAlphaCompare(GX_GREATER, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetZCompLoc(GX_FALSE);
	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	Mtx identity;
	guMtxIdentity(identity);
	GX_LoadPosMtxImm(identity, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	screen_size = size;
	render_textures_reset(0);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetColorUpdate(GX_TRUE);
	GX_CopyDisp(platform_wii_xfb(0), GX_TRUE);
	GX_DrawDone();
}

void render_cleanup(void) {
	render_textures_reset(0);
	GX_DrawDone();
	free(textures[0].data);
	textures[0].data = NULL;
	textures_len = 0;
	free(fifo);
	fifo = NULL;
}
void render_set_screen_size(vec2i_t size) { render_flush(); screen_size = size; set_projection(); }
void render_set_resolution(render_resolution_t res) { (void)res; }
void render_set_post_effect(render_post_effect_t post) { (void)post; }
vec2i_t render_size(void) { return screen_size; }
void render_frame_prepare(void) {
	running_stats = (render_stats_t){0};
	screen_position = vec2(0, 0);
	view_2d = true;
	blend_mode = RENDER_BLEND_NORMAL;
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetCullMode(GX_CULL_NONE);
	set_projection();
}
void render_frame_end(void) {
	render_flush();
	GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
	GX_SetColorUpdate(GX_TRUE);
	GX_CopyDisp(platform_wii_xfb(back), GX_TRUE);
	GX_DrawDone();
	VIDEO_SetNextFramebuffer(platform_wii_xfb(back));
	VIDEO_Flush();
	back ^= 1;
	end_stats = running_stats;
}
const render_stats_t *render_frame_get_stats(void) { return &end_stats; }
void render_set_view(vec3_t pos, vec3_t angles) { (void)pos; (void)angles; render_flush(); view_2d = false; }
void render_set_view_2d(void) { render_flush(); view_2d = true; set_projection(); }
void render_set_model_mat(mat4_t *m) { render_flush(); model_mat = *m; }
void render_set_depth_write(bool enabled) { render_flush(); depth_write = enabled; }
void render_set_depth_test(bool enabled) { render_flush(); depth_test = enabled; }
void render_set_depth_offset(float offset) { render_flush(); depth_offset = offset; }
void render_set_cull_backface(bool enabled) { render_flush(); cull_backface = enabled; }
void render_set_screen_position(vec2_t pos) { render_flush(); screen_position = pos; set_projection(); }
void render_set_blend_mode(render_blend_mode_t mode) {
	if (mode == blend_mode) return;
	render_flush();
	blend_mode = mode;
	GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, mode == RENDER_BLEND_LIGHTER ? GX_BL_ONE : GX_BL_INVSRCALPHA, GX_LO_CLEAR);
}
vec3_t render_transform(vec3_t pos) { return pos; }
void render_push_tris(tris_t tris, uint16_t texture) {
	if (!view_2d) return;
	error_if(texture >= textures_len, "Invalid texture %d", texture);
	if (tris_len && (batch_texture != texture || tris_len == TRIS_MAX)) render_flush();
	batch_texture = texture;
	tris_buffer[tris_len++] = tris;
}
void render_push_2d_tile(vec2i_t pos, vec2i_t uv, vec2i_t uv_size, vec2i_t size, rgba_t color, uint16_t texture) {
	float x = pos.x, y = pos.y, w = size.x, h = size.y;
	float u = uv.x, v = uv.y, uw = uv_size.x, vh = uv_size.y;
	render_push_tris((tris_t){.vertices = {{{x,y+h,0},{u,v+vh},color}, {{x+w,y,0},{u+uw,v},color}, {{x,y,0},{u,v},color}}}, texture);
	render_push_tris((tris_t){.vertices = {{{x+w,y+h,0},{u+uw,v+vh},color}, {{x+w,y,0},{u+uw,v},color}, {{x,y+h,0},{u,v+vh},color}}}, texture);
}
void render_push_2d(vec2i_t pos, vec2i_t size, rgba_t color, uint16_t texture) {
	render_push_2d_tile(pos, vec2i(0, 0), render_texture_size(texture), size, color, texture);
}
void render_push_sprite(vec3_t pos, vec2i_t size, rgba_t color, uint16_t texture) {
	render_push_2d(vec2i(pos.x - size.x * 0.5f, pos.y - size.y * 0.5f), size, color, texture);
}
uint16_t render_texture_create(uint32_t width, uint32_t height, rgba_t *pixels) {
	error_if(textures_len >= TEXTURES_MAX || width > 1024 || height > 1024 || (width && height && !pixels), "Invalid GX texture %ux%u", (unsigned)width, (unsigned)height);
	uint16_t index = textures_len++;
	gx_texture_t *t = &textures[index];
	t->width = width; t->height = height;
	t->padded_width = width ? (width + 3) & ~3u : 4; t->padded_height = height ? (height + 3) & ~3u : 4;
	t->data = memalign(32, t->padded_width * t->padded_height * 2);
	error_if(!t->data, "GX texture allocation failed");
	encode_texture(t, pixels);
	return index;
}
vec2i_t render_texture_size(uint16_t index) {
	error_if(index >= textures_len, "Invalid texture %d", index);
	return vec2i(textures[index].width, textures[index].height);
}
void render_texture_replace_pixels(int16_t index, rgba_t *pixels) {
	error_if(index < 0 || index >= textures_len || !pixels, "Invalid texture %d", index);
	if (tris_len && batch_texture == index) render_flush();
	GX_DrawDone();
	encode_texture(&textures[index], pixels);
}
uint16_t render_textures_len(void) { return textures_len; }
void render_textures_reset(uint16_t len) {
	error_if(len > textures_len, "Invalid texture count %d", len);
	render_flush();
	GX_DrawDone();
	for (uint16_t i = len; i < textures_len; i++) { free(textures[i].data); textures[i].data = NULL; }
	textures_len = len;
	if (!len) {
		rgba_t pixels[4] = {rgba(128,128,128,255), rgba(128,128,128,255), rgba(128,128,128,255), rgba(128,128,128,255)};
		RENDER_NO_TEXTURE = render_texture_create(2, 2, pixels);
	}
}
void render_textures_dump(const char *path) { (void)path; }
