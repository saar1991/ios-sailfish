/*
 * sweetgl → llvmpipe: Core Animation draws that llvmpipe's rasterizer
 * threads draw directly (lp-sweet.py part 3), instead of running them
 * through the vertex pipeline and the JIT fragment shader.
 *
 * eagl.c's fast_draw() already works out, for draws it recognises, the
 * rectangles, texture coordinates, colour and blend (fastpath.h). For
 * the kinds below it leaves them in sweet_lpf_pending just before
 * glDraw*; llvmpipe_draw_vbo() takes them when every rectangle is a 1:1
 * texel copy (or a fill) it can draw exactly, and otherwise draws
 * normally. eagl.c clears n after the draw either way.
 *
 * Copied into Mesa's llvmpipe directory by lp-sweet.py: keep it plain C.
 */
#ifndef SWEET_LPFAST_H
#define SWEET_LPFAST_H

#include <stdint.h>

#define SWEET_LPF_MAX 16        /* eagl.c FAST_RECTS */

/* kinds (eagl.c FPK_) and blends (FPB_) taken */
#define SWEET_LPF_FILL 1
#define SWEET_LPF_COPY 2
#define SWEET_LPF_OPAQUE 5
#define SWEET_LPF_TINT 3                /* colour * texture */
#define SWEET_LPF_ATINT 4               /* colour * texture.a */
#define SWEET_LPF_COPYN 7               /* copy, then CA's noise dither */
#define SWEET_LPF_ALPHA 6               /* texture.a in every channel */
#define SWEET_LPF_TINTN 8               /* colour * texture, then the dither */
#define SWEET_LPF_ATINTDK 9             /* CA's darkenSover(colour * texture.a, framebuffer) */
#define SWEET_LPF_COPYDKN 10            /* darkenSover(texture, framebuffer), then the dither */
#define SWEET_LPF_LUMC 12               /* luminanceCurve(framebuffer, farg, alpha) */
#define SWEET_LPF_CM4 13                /* colour matrix on the framebuffer (with its alpha row) */
#define SWEET_LPF_CM3 14                /* colour matrix on the framebuffer */
#define SWEET_LPF_BLUR 19               /* taps of texture0, weighted (divided by alpha when bdiv) */
#define SWEET_LPF_TAPS 16               /* fastpath.h FP_TAPS */
/* blends (eagl.c FPB_): 0 none, 1 premul, 2 straight, 3 mixed, 4 atop, 5 dstout, 6 add */
#define SWEET_LPF_BLEND_NONE 0
#define SWEET_LPF_BLEND_PREMUL 1

struct sweet_lpf_job {
	int x0, y0, x1, y1;             /* pixels written, half-open, framebuffer rows */
	float xa, xb, ya, yb;           /* the rectangle in window coordinates */
	float ua, ub, va, vb;           /* normalized texture coordinates at xa, xb and ya, yb */
	int swap, tclamp;
	float cu0, cu1, cv0, cv1;
	uint8_t color[4];               /* RGBA: FILL, TINT, ATINT, TINTN */
	/* COPYN: the noise texel at window pixel x, y is (u, v) = (nu0 + nud * (x + .5), nv0 + nvd * (y + .5)), nearest, repeat */
	float nu0, nud, nv0, nvd;
	float falpha;                   /* LUMC: the vertex colour's alpha, unquantized */
};

struct sweet_lpf {
	int n;                          /* rectangles; 0: nothing pending */
	int kind, blend;
	void *src;                      /* struct pipe_resource * of texture0 (COPY, OPAQUE, COPYN) */
	void *noise;                    /* COPYN: the noise texture */
	float nscale;                   /* COPYN: rgb += (noise.rgb - .5) * nscale * a, clamped to [0, a] */
	float farg[4];                  /* LUMC: the curve's coefficients */
	float cm[4][3];                 /* CM4 / CM3: colour matrix rows (alpha row zero for CM3) */
	int ntap, bdiv;                 /* BLUR */
	float toff[SWEET_LPF_TAPS][2], tw[SWEET_LPF_TAPS];
	struct sweet_lpf_job job[SWEET_LPF_MAX];
};

extern struct sweet_lpf sweet_lpf_pending;

#endif
