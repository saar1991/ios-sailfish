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
static uint32_t rs = 777;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static float frand(float a, float b) { return a + (b - a) * (float)(rnd() % 100000) / 100000.f; }
static void fill(struct fp_img *m, int w, int h) {
	m->w = w; m->h = h; m->fmt = FP_RGBA; m->stride = 4 * w; m->ptr = malloc((size_t)4 * w * h);
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *p = m->ptr + 4 * (y * w + x); int a = rnd() % 256;
		p[3] = a; for (int k = 0; k < 3; k++) p[k] = (uint8_t)((rnd() % 256) * a / 255); }
}
/* coordinate pair at window point (px, py), as the varying would interpolate */
static void coord(const struct fp_job *j, double ua, double ub, double va, double vb, int swap, double px, double py, double *u, double *v) {
	double fx = (px - j->xa) / (j->xb - j->xa), fy = (py - j->ya) / (j->yb - j->ya);
	if (!swap) { *u = ua + (ub - ua) * fx; *v = va + (vb - va) * fy; } else { *v = va + (vb - va) * fx; *u = ua + (ub - ua) * fy; }
}
static double dval(int circle, double u, double v) {
	if (circle) { double d = u * u + v * v; d = d > 1e-4 ? d : 1e-4; return sqrt(d); }
	double a = fabs(u) * 1.199 - 0.199, b = fabs(v) * 1.199 - 0.199; a = a > 0 ? a : 0; b = b > 0 ? b : 0;
	return pow(a, 2.68) + pow(b, 2.68);
}
static double lval(const struct fp_job *j, int circle, int two, int X, int Y) {
	double ua = two ? j->u2a : j->ua, ub = two ? j->u2b : j->ub, va = two ? j->v2a : j->va, vb = two ? j->v2b : j->vb;
	int swap = two ? j->swap2 : j->swap, qx = X & ~1, qy = Y & ~1; double u, v;
	coord(j, ua, ub, va, vb, swap, qx + .5, qy + .5, &u, &v); double tl = dval(circle, u, v);
	coord(j, ua, ub, va, vb, swap, qx + 1.5, qy + .5, &u, &v); double tr = dval(circle, u, v);
	coord(j, ua, ub, va, vb, swap, qx + .5, qy + 1.5, &u, &v); double bl = dval(circle, u, v);
	coord(j, ua, ub, va, vb, swap, X + .5, Y + .5, &u, &v); double d = dval(circle, u, v);
	double w = fabs(tr - tl) + fabs(bl - tl); w = w > 1e-4 ? w : 1e-4;
	double l = (w * .5 + (1. - d)) / w; return l < 0 ? 0 : l > 1 ? 1 : l;
}
static int texel_n(float u, int n) { int i = (int)floorf(u * (float)n); return i < 0 ? 0 : i >= n ? n - 1 : i; }
static void fetch_n(const struct fp_img *s, const struct fp_job *j, int two, int X, int Y, double t[4]) {
	/* texel choice in float, as the shim's tables (a texel boundary is not what this test is about) */
	float ua = two ? j->u2a : j->ua, ub = two ? j->u2b : j->ub, va = two ? j->v2a : j->va, vb = two ? j->v2b : j->vb, u, v;
	if (!(two ? j->swap2 : j->swap)) { u = ua + ((float)X + .5f - j->xa) * ((ub - ua) / (j->xb - j->xa)); v = va + ((float)Y + .5f - j->ya) * ((vb - va) / (j->yb - j->ya)); }
	else { v = va + ((float)X + .5f - j->xa) * ((vb - va) / (j->xb - j->xa)); u = ua + ((float)Y + .5f - j->ya) * ((ub - ua) / (j->yb - j->ya)); }
	if (!two && j->tclamp) { u = u < j->cu0 ? j->cu0 : u > j->cu1 ? j->cu1 : u; v = v < j->cv0 ? j->cv0 : v > j->cv1 ? j->cv1 : v; }
	const uint8_t *p = s->ptr + 4 * (texel_n(v, s->h) * s->w + texel_n(u, s->w));
	for (int k = 0; k < 4; k++) t[k] = p[k] / 255.;
}
int main(void) {
	int cases = 0, bad = 0, maxd = 0, edge = 0; long px = 0;
	const int kinds[4] = {FPK_SUPER, FPK_CIRCLE, FPK_MASKSUP, FPK_MIXBD};
	for (int it = 0; it < 6000; it++) {
		struct fp_job j = {0}; struct fp_img dst, s0, s1;
		j.kind = kinds[it % 4];
		int dw = 20 + rnd() % 200, dh = 20 + rnd() % 200; fill(&dst, dw, dh); fill(&s0, 1 + rnd() % 64, 1 + rnd() % 64); fill(&s1, 1 + rnd() % 64, 1 + rnd() % 64);
		j.xa = frand(-5, dw * .6f); j.xb = j.xa + frand(2, dw); j.ya = frand(-5, dh * .6f); j.yb = j.ya + frand(2, dh);
		j.x0 = (int)floorf(j.xa + .5f); j.x1 = (int)floorf(j.xb + .5f); j.y0 = (int)ceilf(j.ya - .5f); j.y1 = (int)ceilf(j.yb - .5f);
		j.x0 = j.x0 < 0 ? 0 : j.x0; j.y0 = j.y0 < 0 ? 0 : j.y0; j.x1 = j.x1 > dw ? dw : j.x1; j.y1 = j.y1 > dh ? dh : j.y1;
		if (j.x1 <= j.x0 || j.y1 <= j.y0) continue;
		float lo = -1.f, hi = 1.f; if (rnd() % 3 == 0) { lo = frand(-1.3f, 0); hi = frand(0.1f, 1.3f); }
		j.ua = rnd() % 2 ? lo : hi; j.ub = j.ua == lo ? hi : lo; j.va = lo; j.vb = hi; j.swap = rnd() % 2;
		j.u2a = -1; j.u2b = 1; j.v2a = rnd() % 2 ? -1 : 1; j.v2b = -j.v2a; j.swap2 = rnd() % 2;
		if (j.kind == FPK_MASKSUP || j.kind == FPK_MIXBD) { j.ua = frand(0, .5f); j.ub = frand(.5f, 1); j.va = frand(0, .5f); j.vb = frand(.5f, 1); }
		if (j.kind == FPK_MIXBD) { j.u2a = frand(0, .5f); j.u2b = frand(.5f, 1); j.v2a = frand(0, .5f); j.v2b = frand(.5f, 1); }
		if (j.kind == FPK_MASKSUP && rnd() % 2) { j.tclamp = 1; j.cu0 = .2f; j.cu1 = .8f; j.cv0 = .1f; j.cv1 = .9f; }
		for (int k = 0; k < 4; k++) j.color[k] = rnd() % 256;
		j.src2 = &s1; j.linear = 0; j.linear2 = 0;
		int ow = j.x1 - j.x0, oh = j.y1 - j.y0; uint8_t *out = malloc((size_t)4 * ow * oh);
		fp_run(&j, &dst, j.kind == FPK_SUPER || j.kind == FPK_CIRCLE ? NULL : &s0, out, 4 * ow);
		cases++;
		for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
			int X = j.x0 + x, Y = j.y0 + y; double r[4], t[4], t2[4];
			const uint8_t *dc = dst.ptr + 4 * (Y * dw + X);
			if (j.kind == FPK_MIXBD) { fetch_n(&s0, &j, 0, X, Y, t); fetch_n(&s1, &j, 1, X, Y, t2);
				for (int k = 0; k < 4; k++) r[k] = t2[k] * (1 - t[3]) + dc[k] / 255. * t[3];
			} else { double l = lval(&j, j.kind == FPK_CIRCLE, j.kind == FPK_MASKSUP, X, Y); if (l > 0 && l < 1) edge++;
				if (j.kind == FPK_MASKSUP) fetch_n(&s0, &j, 0, X, Y, t);
				for (int k = 0; k < 4; k++) r[k] = j.color[k] / 255. * l * (j.kind == FPK_MASKSUP ? t[k] : 1); }
			for (int k = 0; k < 4; k++) { int want = (int)(fmin(fmax(r[k], 0), 1) * 255 + .5), got = out[4 * (y * ow + x) + k];
				int d = abs(want - got); maxd = d > maxd ? d : maxd; if (d > 1 && bad++ < 5) printf("kind %d px %d,%d k %d want %d got %d\n", j.kind, X, Y, k, want, got); }
			px++;
		}
		free(out); free(dst.ptr); free(s0.ptr); free(s1.ptr);
	}
	printf("cases %d pixels %ld edge pixels %d bad %d max diff %d\n", cases, px, edge, bad, maxd);
	return bad != 0;
}
