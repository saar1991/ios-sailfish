/* fp_run's shape and mix kinds against a per-pixel double model of the shaders on softpipe */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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
void dispatch_apply_f(size_t n, dispatch_queue_t q, void *ctx, void (*work)(void *, size_t)) { (void)q; for (size_t i = n; i-- > 0;) work(ctx, i); }
static uint32_t rs = 4242;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frand(float a, float b) { return a + (b - a) * (float)(rnd() % 100000) / 100000.f; }
static void fill(struct fp_img *m, int w, int h, int fmt) {
	m->w = w; m->h = h; m->fmt = fmt; m->stride = 4 * w + 4 * (int)(rnd() % 2); m->ptr = malloc((size_t)m->stride * h);
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *p = m->ptr + y * m->stride + 4 * x; int a = rnd() % 4 == 0 ? rnd() % 6 : rnd() % 256;
		p[3] = a; for (int k = 0; k < 3; k++) p[k] = (uint8_t)((rnd() % 256) * a / 255); }
}
/* softpipe: wrap_*_clamp_to_edge, lerp_2d, per pixel (no tables) */
static void sp_sample(const struct fp_img *s, float u, float v, int linear, double q[4]) {
	uint8_t c[4][4]; int x0, x1, y0, y1; float fx = 0, fy = 0;
	if (linear) {
		float a = (u * s->w < 0 ? 0 : u * s->w > s->w ? s->w : u * s->w) - .5f, b = (v * s->h < 0 ? 0 : v * s->h > s->h ? s->h : v * s->h) - .5f;
		x0 = (int)floorf(a); x1 = x0 + 1; y0 = (int)floorf(b); y1 = y0 + 1; fx = a - floorf(a); fy = b - floorf(b);
		x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 >= s->w ? s->w - 1 : x1; y1 = y1 >= s->h ? s->h - 1 : y1;
	} else {
		float a = u * s->w, b = v * s->h;
		x0 = x1 = a < .5f ? 0 : a > s->w - .5f ? s->w - 1 : (int)floorf(a); y0 = y1 = b < .5f ? 0 : b > s->h - .5f ? s->h - 1 : (int)floorf(b);
	}
	int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
	for (int i = 0; i < 4; i++) fp_read_rgba(s, xs[i], ys[i], c[i]);
	for (int k = 0; k < 4; k++) { double t0 = c[0][k] / 255. + fx * (c[1][k] / 255. - c[0][k] / 255.), t1 = c[2][k] / 255. + fx * (c[3][k] / 255. - c[2][k] / 255.); q[k] = t0 + fy * (t1 - t0); }
}
int main(void) {
	int cases = 0, bad = 0, maxd = 0; long px = 0; const int fmts[4] = {FP_BGRA, FP_RGBA, FP_BGRX, FP_RGBX};
	for (int it = 0; it < 4000; it++) {
		struct fp_job j = {0}; struct fp_img dst, src;
		j.kind = FPK_BLUR; int dw = 10 + rnd() % 120, dh = 10 + rnd() % 120; fill(&dst, dw, dh, fmts[rnd() % 4]); fill(&src, 1 + rnd() % 100, 1 + rnd() % 100, fmts[rnd() % 4]);
		j.xa = frand(-3, dw * .5f); j.xb = j.xa + frand(2, dw); j.ya = frand(-3, dh * .5f); j.yb = j.ya + frand(2, dh);
		j.x0 = (int)floorf(j.xa + .5f); j.x1 = (int)floorf(j.xb + .5f); j.y0 = (int)ceilf(j.ya - .5f); j.y1 = (int)ceilf(j.yb - .5f);
		j.x0 = j.x0 < 0 ? 0 : j.x0; j.y0 = j.y0 < 0 ? 0 : j.y0; j.x1 = j.x1 > dw ? dw : j.x1; j.y1 = j.y1 > dh ? dh : j.y1;
		if (j.x1 <= j.x0 || j.y1 <= j.y0) continue;
		j.ua = frand(-.1f, .5f); j.ub = frand(.5f, 1.1f); if (rnd() % 2) { float t = j.ua; j.ua = j.ub; j.ub = t; } j.va = frand(-.1f, .5f); j.vb = frand(.5f, 1.1f); j.swap = rnd() % 2;
		j.linear = rnd() % 4 != 0; j.ntap = 1 + rnd() % 14; j.bdiv = rnd() % 2; j.blend = 0;
		for (int t = 0; t < j.ntap; t++) { j.toff[t][0] = frand(-.05f, .05f); j.toff[t][1] = rnd() % 2 ? 0 : frand(-.05f, .05f); j.tw[t] = frand(0, .3f); }
		int ow = j.x1 - j.x0, oh = j.y1 - j.y0; uint8_t *out = malloc((size_t)4 * ow * oh);
		fp_run(&j, &dst, &src, out, 4 * ow); cases++;
		for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
			int X = j.x0 + x, Y = j.y0 + y; float u, v;
			float fx = (float)X + .5f - j.xa, fy = (float)Y + .5f - j.ya;
			if (!j.swap) { u = j.ua + fx * ((j.ub - j.ua) / (j.xb - j.xa)); v = j.va + fy * ((j.vb - j.va) / (j.yb - j.ya)); }
			else { v = j.va + fx * ((j.vb - j.va) / (j.xb - j.xa)); u = j.ua + fy * ((j.ub - j.ua) / (j.yb - j.ya)); }
			double acc[4] = {0};
			for (int t = 0; t < j.ntap; t++) { double q[4]; sp_sample(&src, u + j.toff[t][0], v + j.toff[t][1], j.linear, q); for (int k = 0; k < 4; k++) acc[k] += q[k] * j.tw[t]; }
			if (j.bdiv) { double m = acc[3] > .001 ? acc[3] : .001; for (int k = 0; k < 4; k++) acc[k] /= m; }
			for (int k = 0; k < 4; k++) { int want = k == 3 && (dst.fmt == FP_BGRX || dst.fmt == FP_RGBX) ? 255 : (int)(fmin(fmax(acc[k], 0), 1) * 255 + .5), got = out[4 * (y * ow + x) + k]; int d = abs(want - got);
				maxd = d > maxd ? d : maxd; if (d > 1 && bad++ < 6) printf("px %d,%d k %d want %d got %d (div %d swap %d lin %d)\n", X, Y, k, want, got, j.bdiv, j.swap, j.linear); }
			px++;
		}
		free(out); free(dst.ptr); free(src.ptr);
	}
	printf("cases %d pixels %ld bad %d max diff %d\n", cases, px, bad, maxd);
	return bad != 0;
}
