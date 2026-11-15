#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fastpath.h"
struct gl_context; struct gl_texture_object;
struct gl_context *_mesa_get_current_context(void) { return NULL; }
struct gl_texture_object *_mesa_lookup_texture(struct gl_context *c, unsigned id) { (void)c; (void)id; return NULL; }
struct util_format_description;
const struct util_format_description *util_format_description(int f) { (void)f; return 0; }
_Bool util_format_is_pure_integer(int f) { (void)f; return 0; }
void util_format_unpack_rgba_8unorm_rect(int f, void *d, unsigned ds, const void *s, unsigned ss, unsigned w, unsigned h) { (void)f; (void)d; (void)ds; (void)s; (void)ss; (void)w; (void)h; }
typedef struct dispatch_queue_s *dispatch_queue_t;
dispatch_queue_t dispatch_get_global_queue(long a, unsigned long b) { (void)a; (void)b; return NULL; }
void dispatch_apply_f(size_t n, dispatch_queue_t q, void *ctx, void (*work)(void *, size_t)) { (void)q; for (size_t i = 0; i < n; i++) work(ctx, i); }
#define px_rgba old_px_rgba
#define bpp old_bpp
#define texel old_texel
#define mul8 old_mul8
static inline void old_px_rgba(const uint8_t *p, int fmt, uint8_t c[4]);
#include "old_loop.inc"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
int main(void) {
	int W = 750, H = 1334;
	struct fp_img src = {malloc((size_t)W * H * 4), W, H, W * 4, FP_BGRA, 0}, dst = {malloc((size_t)W * H * 4), W, H, W * 4, FP_BGRA, 0};
	for (int i = 0; i < W * H * 4; i++) src.ptr[i] = (uint8_t)(i * 7), dst.ptr[i] = (uint8_t)(i * 3);
	struct { const char *name; int kind, blend, linear; float scale; } T[] = {
		{"copy 1:1", FPK_COPY, 0, 1, 1}, {"copy 1:1 premul", FPK_COPY, 1, 1, 1}, {"tint 1:1 premul", FPK_TINT, 1, 1, 1},
		{"copy scaled linear", FPK_COPY, 0, 1, 0.7f}, {"fill straight", FPK_FILL, 2, 0, 1},
		{"atint 1:1 premul", FPK_ATINT, 1, 1, 1}, {"atint scaled premul", FPK_ATINT, 1, 1, 0.7f}, {"alpha 1:1 premul", FPK_ALPHA, 1, 1, 1},
		{"atintdk 1:1 premul", FPK_ATINTDK, 1, 1, 1}, {"circle premul", FPK_CIRCLE, 1, 1, 1}, {"tint scaled premul", FPK_TINT, 1, 1, 0.7f}};
	fp_threads = 0;
	for (unsigned t = 0; t < sizeof(T) / sizeof(T[0]); t++) {
		struct fp_job j = {.kind = T[t].kind, .blend = T[t].blend, .x0 = 0, .y0 = 0, .x1 = W, .y1 = H, .xa = 0, .xb = (float)W, .ya = 0, .yb = (float)H,
		    .ua = 0, .ub = T[t].scale, .va = 0, .vb = T[t].scale, .linear = T[t].linear, .color = {200, 100, 50, 180}};
		const struct fp_img *sp = T[t].kind == FPK_FILL ? NULL : &src;
		if (T[t].kind == FPK_CIRCLE) { j.ua = -1; j.ub = 1; j.va = -1; j.vb = 1; sp = NULL; }
		double a = now(); if (T[t].kind <= FPK_ALPHA) for (int r = 0; r < 5; r++) old_fp_run(&j, &dst, sp, NULL, 0); double b = now();
		for (int r = 0; r < 5; r++) fp_run(&j, &dst, sp, NULL, 0); double c = now();
		printf("%-20s old %6.2f ms  new %6.2f ms  %.1f ns/px (one core, x86 host)\n", T[t].name, (b - a) / 5, (c - b) / 5, (c - b) / 5 * 1e6 / (W * H));
	}
	return 0;
}
