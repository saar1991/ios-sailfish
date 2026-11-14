#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fastpath.h"
/* stubs for the Mesa side of fastpath.c */
struct gl_context; struct gl_texture_object;
struct gl_context *_mesa_get_current_context(void) { return NULL; }
struct gl_texture_object *_mesa_lookup_texture(struct gl_context *c, unsigned id) { (void)c; (void)id; return NULL; }
struct util_format_description;
const struct util_format_description *util_format_description(int f) { (void)f; return 0; }
_Bool util_format_is_pure_integer(int f) { (void)f; return 0; }
void util_format_unpack_rgba_8unorm_rect(int f, void *d, unsigned ds, const void *s, unsigned ss, unsigned w, unsigned h) { (void)f; (void)d; (void)ds; (void)s; (void)ss; (void)w; (void)h; }
typedef struct dispatch_queue_s *dispatch_queue_t;
dispatch_queue_t dispatch_get_global_queue(long a, unsigned long b) { (void)a; (void)b; return NULL; }
void dispatch_apply_f(size_t n, dispatch_queue_t q, void *ctx, void (*work)(void *, size_t)) {
	(void)q; /* reverse order: catches dependencies between chunks */
	for (size_t i = n; i-- > 0;) work(ctx, i);
}
/* the old loop, renamed (static helpers in their own namespace) */
#define px_rgba old_px_rgba
#define bpp old_bpp
#define texel old_texel
#define mul8 old_mul8
static inline void old_px_rgba(const uint8_t *p, int fmt, uint8_t c[4]);
#include "old_loop.inc"
#undef px_rgba
#undef bpp
#undef texel
#undef mul8
static uint32_t rs = 12345;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static void fill_img(struct fp_img *m, int w, int h, int fmt) {
	m->w = w; m->h = h; m->fmt = fmt; m->stride = w * (fmt == FP_A8 ? 1 : 4) + 4 * (int)(rnd() % 3);
	m->ptr = malloc((size_t)m->stride * h);
	for (int i = 0; i < m->stride * h; i++) m->ptr[i] = (uint8_t)rnd();
	/* premultiplied-looking pixels half the time */
	if (fmt != FP_A8 && rnd() % 2) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
		uint8_t *p = m->ptr + y * m->stride + 4 * x; int a = rnd() % 4 == 0 ? 255 : rnd() % 4 == 0 ? 0 : p[3];
		p[3] = (uint8_t)a; for (int k = 0; k < 3; k++) p[k] = (uint8_t)(p[k] * a / 255); }
}
static void read_rgba(const struct fp_img *m, int x, int y, uint8_t c[4]) { fp_read_rgba(m, x, y, c); }
static int dc_of(const struct fp_img *m, int x, int y, int k) { uint8_t c[4]; fp_read_rgba(m, x, y, c); return c[k]; }
int main(void) {
	int fails = 0, cases = 0, fast_rows = 0, swaps = 0; long maxd_new = 0;
	const int fmts[4] = {FP_BGRA, FP_RGBA, FP_BGRX, FP_RGBX};
	for (int it = 0; it < 20000; it++) {
		struct fp_img src = {0}, d1 = {0}, d2 = {0};
		int sw = 1 + rnd() % 90, sh = 1 + rnd() % 90, dw = 1 + rnd() % 100, dh = 1 + rnd() % 100;
		int sfmt = rnd() % 6 == 0 ? FP_A8 : fmts[rnd() % 4], dfmt = fmts[rnd() % 4];
		if (rnd() % 3 == 0) sfmt = dfmt;
		fill_img(&src, sw, sh, sfmt); fill_img(&d1, dw, dh, dfmt);
		d2 = d1; d2.ptr = malloc((size_t)d1.stride * dh); memcpy(d2.ptr, d1.ptr, (size_t)d1.stride * dh);
		struct fp_job j = {0};
		j.kind = 1 + (int)(rnd() % 11); j.linear = (int)(rnd() % 2);
		int blendm = (int)(rnd() % 5); j.blend = blendm;
		struct fp_img nz = {0}; int nw = 16 << (rnd() % 2);
		if (j.kind == 7 || j.kind == 8 || j.kind == 10) { fill_img(&nz, nw, nw, FP_RGBA); j.noise = &nz; j.nscale = (float)(rnd() % 100) / 400.f; j.nud = 1.f / nw; j.nu0 = (float)(rnd() % 64) / nw; j.nvd = -1.f / nw; j.nv0 = (float)(rnd() % 64) / nw; }
		for (int k = 0; k < 4; k++) j.color[k] = (uint8_t)rnd();
		int x0 = (int)(rnd() % dw), y0 = (int)(rnd() % dh), x1 = x0 + 1 + (int)(rnd() % (dw - x0)), y1 = y0 + 1 + (int)(rnd() % (dh - y0));
		j.x0 = x0; j.y0 = y0; j.x1 = x1; j.y1 = y1;
		j.xa = (float)x0; j.xb = (float)x1; j.ya = (float)y0; j.yb = (float)y1;
		if (rnd() % 2) { /* 1:1 aligned: texel = pixel + offset */
			int ox = (int)(rnd() % (sw)), oy = (int)(rnd() % (sh));
			j.ua = (float)ox / sw; j.ub = (float)(ox + (x1 - x0)) / sw; j.va = (float)oy / sh; j.vb = (float)(oy + (y1 - y0)) / sh;
		} else {
			j.ua = (float)(rnd() % 1000) / 1000.f; j.ub = (float)(rnd() % 1000) / 1000.f; j.va = (float)(rnd() % 1000) / 1000.f; j.vb = (float)(rnd() % 1000) / 1000.f;
		}
		struct fp_img m2 = {0};
		if (j.kind == FPK_MASK) {
			int mw = 1 + (int)(rnd() % 40), mh = 1 + (int)(rnd() % 40);
			fill_img(&m2, mw, mh, rnd() % 4 == 0 ? FP_A8 : fmts[rnd() % 4]);
			j.src2 = &m2; j.linear2 = (int)(rnd() % 2);
			j.u2a = (float)(rnd() % 1000) / 1000.f; j.u2b = (float)(rnd() % 1000) / 1000.f;
			j.v2a = (float)(rnd() % 1000) / 1000.f; j.v2b = (float)(rnd() % 1000) / 1000.f;
		}
		const struct fp_img *sp = j.kind == FPK_FILL ? NULL : &src;
		int w = x1 - x0, h = y1 - y0;
		uint8_t *o1 = NULL, *o2 = NULL; int use_out = rnd() % 4 == 0;
		if (use_out) { o1 = calloc((size_t)w * h, 4); o2 = calloc((size_t)w * h, 4); }
		cases++;
		if (blendm <= 1 && j.kind >= 2 && j.kind <= 6 && rnd() % 3 == 0 && !use_out) {
			/* turned texture: same pixels as the old loop on the transposed texture */
			struct fp_img T = src; T.w = sh; T.h = sw; int bb = sfmt == FP_A8 ? 1 : 4; T.stride = sh * bb;
			T.ptr = malloc((size_t)T.stride * sw);
			for (int r = 0; r < sw; r++) for (int c = 0; c < sh; c++) memcpy(T.ptr + r * T.stride + c * bb, src.ptr + c * src.stride + r * bb, bb);
			struct fp_job jt = j; jt.ua = j.va; jt.ub = j.vb; jt.va = j.ua; jt.vb = j.ub; jt.swap = 0;
			j.swap = 1;
			old_fp_run(&jt, &d1, &T, NULL, 0);
			fp_threads = (int)(rnd() % 2);
			fp_run(&j, &d2, &src, NULL, 0);
			if (memcmp(d1.ptr, d2.ptr, (size_t)d1.stride * dh) != 0 && fails++ < 10) printf("SWAP MISMATCH it %d kind %d blend %d linear %d\n", it, j.kind, j.blend, j.linear);
			swaps++;
			free(T.ptr);
		} else if (blendm <= 1 && j.kind <= 6) {
			old_fp_run(&j, &d1, sp, o1, w * 4);
			fp_threads = (int)(rnd() % 2);
			fp_run(&j, &d2, sp, o2, w * 4);
			int bad = use_out ? memcmp(o1, o2, (size_t)w * h * 4) != 0 : memcmp(d1.ptr, d2.ptr, (size_t)d1.stride * dh) != 0;
			if (bad && fails++ < 10) printf("MISMATCH it %d kind %d blend %d sfmt %d dfmt %d out %d linear %d rect %d,%d-%d,%d\n", it, j.kind, j.blend, sfmt, dfmt, use_out, j.linear, x0, y0, x1, y1);
		} else {
			/* reference: old loop for the shader output (no blend) into an RGBA buffer, then float blend */
			struct fp_job js = j; js.blend = 0; js.noise = NULL; if (js.kind == 7 || js.kind == 9 || js.kind == 10 || js.kind == FPK_MASK) js.kind = FPK_COPY; if (js.kind == 8) js.kind = FPK_TINT;
			uint8_t *mk_out = NULL;
			if (j.kind == FPK_MASK) {
				/* the mask's texels through the old loop: a copy of src2 with the mask coordinates */
				struct fp_job jm = js; jm.ua = j.u2a; jm.ub = j.u2b; jm.va = j.v2a; jm.vb = j.v2b; jm.linear = j.linear2; jm.swap = 0; jm.tclamp = 0;
				mk_out = calloc((size_t)w * h, 4);
				struct fp_img dr2 = d1; dr2.fmt = FP_RGBA;
				old_fp_run(&jm, &dr2, &m2, mk_out, w * 4);
			}
			uint8_t *sh_out = calloc((size_t)w * h, 4);
			struct fp_img dr = d1; dr.fmt = FP_RGBA;   /* shader output without the X-format alpha */
			old_fp_run(&js, &dr, sp, sh_out, w * 4);
			fp_threads = (int)(rnd() % 2);
			fp_run(&j, &d2, sp, o2, w * 4);
			for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
				uint8_t s[4], dc[4], got[4]; memcpy(s, sh_out + (y * w + x) * 4, 4);
				read_rgba(&d1, x0 + x, y0 + y, dc);
				if (use_out) memcpy(got, o2 + (y * w + x) * 4, 4); else read_rgba(&d2, x0 + x, y0 + y, got);
				if (j.kind == FPK_MASK) {
					float ma = mk_out[(y * w + x) * 4 + 3] / 255.f;
					for (int k = 0; k < 4; k++) { float c = (j.color[k] / 255.f) * (s[k] / 255.f) * ma; c = c < 0 ? 0 : c > 1 ? 1 : c; s[k] = (uint8_t)(c * 255.f + .5f); }
				} else if (j.kind == 9 || j.kind == 10) {
					/* darkenSover over the framebuffer (dc), from the texel t = s (copy output) */
					float S[4], D[4], Rf[4];
					for (int k = 0; k < 4; k++) { D[k] = dc_of(&d1, x0 + x, y0 + y, k) / 255.f; S[k] = j.kind == 9 ? (j.color[k] / 255.f) * (s[3] / 255.f) : s[k] / 255.f; }
					float lum = D[0] * .2125f + D[1] * .7154f + D[2] * .0721f, y4 = (lum * lum) * (lum * lum);
					for (int k = 0; k < 4; k++) Rf[k] = D[k] * (1 - S[3] * y4) + S[k] * y4;
					if (j.kind == 10) {
						int px = x0 + x, py = y0 + y;
						double u = j.nu0 + (double)j.nud * (px + 0.5), v = j.nv0 + (double)j.nvd * (py + 0.5);
						long ix = (long)floor(u * nw) % nw, iy = (long)floor(v * nw) % nw; if (ix < 0) ix += nw; if (iy < 0) iy += nw;
						uint8_t n4[4]; read_rgba(&nz, (int)ix, (int)iy, n4);
						for (int k = 0; k < 3; k++) { float c = Rf[k] + (n4[k] / 255.f - .5f) * j.nscale * Rf[3]; Rf[k] = c < 0 ? 0 : c > Rf[3] ? Rf[3] : c; }
					}
					for (int k = 0; k < 4; k++) { float c = Rf[k] < 0 ? 0 : Rf[k] > 1 ? 1 : Rf[k]; s[k] = (uint8_t)(c * 255.f + .5f); }
				} else if (j.kind >= 7) {
					int px = x0 + x, py = y0 + y;
					double u = j.nu0 + (double)j.nud * (px + 0.5), v = j.nv0 + (double)j.nvd * (py + 0.5);
					long ix = (long)floor(u * nw) % nw, iy = (long)floor(v * nw) % nw; if (ix < 0) ix += nw; if (iy < 0) iy += nw;
					uint8_t n4[4]; read_rgba(&nz, (int)ix, (int)iy, n4);
					float a = s[3] / 255.f;
					for (int k = 0; k < 3; k++) { float c = s[k] / 255.f + (n4[k] / 255.f - .5f) * j.nscale * a; c = c < 0 ? 0 : c > a ? a : c; s[k] = (uint8_t)(c * 255.f + .5f); }
				}
				float sa = s[3] / 255.f;
				for (int k = 0; k < 4; k++) {
					float sf = s[k] / 255.f, df = dc[k] / 255.f, r;
					float da = dc[3] / 255.f;
					if (blendm == FPB_NONE) r = sf;
					else if (blendm == FPB_PREMUL) r = sf + df * (1 - sa);
					else if (blendm == FPB_ATOP) r = sf * da + df * (1 - sa);
					else if (k < 3 || blendm == FPB_STRAIGHT) r = sf * sa + df * (1 - sa); else r = sf + df * (1 - sa);
					if (r > 1) r = 1;
					int want = (int)(r * 255.f + 0.5f);
					if ((dfmt == FP_BGRX || dfmt == FP_RGBX) && k == 3) want = 255;
					long dd = labs((long)want - got[k]); if (dd > maxd_new) maxd_new = dd;
					if (dd > 1 && fails++ < 4) printf("BLEND %d sfmt %d dfmt %d out %d kind %d lin %d rect %d,%d-%d,%d it %d k %d want %d got %d (s %d,%d,%d,%d d %d,%d,%d,%d)\n", blendm, sfmt, dfmt, use_out, j.kind, j.linear, x0,y0,x1,y1, it, k, want, got[k], s[0],s[1],s[2],s[3],dc[0],dc[1],dc[2],dc[3]);
				}
			}
			free(sh_out); free(mk_out);
		}
		free(src.ptr); free(d1.ptr); free(d2.ptr); free(o1); free(o2); free(nz.ptr); free(m2.ptr);
	}
	(void)fast_rows;
	printf("cases %d (turned %d) fails %d max blend diff %ld\n", cases, swaps, fails, maxd_new);
	return fails != 0;
}
