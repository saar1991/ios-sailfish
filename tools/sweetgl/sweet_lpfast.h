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
#define SWEET_LPF_BLEND_NONE 0
#define SWEET_LPF_BLEND_PREMUL 1

struct sweet_lpf_job {
	int x0, y0, x1, y1;             /* pixels written, half-open, framebuffer rows */
	float xa, xb, ya, yb;           /* the rectangle in window coordinates */
	float ua, ub, va, vb;           /* normalized texture coordinates at xa, xb and ya, yb */
	int swap, tclamp;
	float cu0, cu1, cv0, cv1;
	uint8_t color[4];               /* RGBA, FILL */
};

struct sweet_lpf {
	int n;                          /* rectangles; 0: nothing pending */
	int kind, blend;
	void *src;                      /* struct pipe_resource * of texture0 (COPY, OPAQUE) */
	struct sweet_lpf_job job[SWEET_LPF_MAX];
};

extern struct sweet_lpf sweet_lpf_pending;

#endif
