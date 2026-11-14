/*
 * CPU fast paths for CoreAnimation's simplest draws: one screen-aligned
 * rectangle with a fill, copy or tinted copy shader. softpipe runs those
 * through its shader interpreter at ~60 ns a pixel; a direct loop is many
 * times faster. eagl.c decides which draws qualify (GL state); this file
 * maps the textures through the gallium interface (which flushes softpipe's
 * caches as needed) and runs the pixel loop.
 */
#ifndef SWEETGL_FASTPATH_H
#define SWEETGL_FASTPATH_H

#include <stdint.h>

enum fp_fmt { FP_NONE, FP_BGRA, FP_RGBA, FP_BGRX, FP_RGBX, FP_A8 };

/* fragment shaders handled, by what they compute */
enum fp_kind {
	FPK_NONE,
	FPK_FILL,       /* color */
	FPK_COPY,       /* texture */
	FPK_TINT,       /* color * texture */
	FPK_ATINT,      /* color * texture.a */
	FPK_OPAQUE,     /* vec4(texture.rgb, 1) */
	FPK_ALPHA,      /* vec4(texture.a) */
	FPK_COUNT
};

struct fp_img {
	uint8_t *ptr;
	int w, h, stride, fmt;
	void *xfer;
};

struct fp_job {
	int kind;
	int blend;              /* 0: replace; 1: premultiplied over (ONE, ONE_MINUS_SRC_ALPHA) */
	int x0, y0, x1, y1;     /* pixels written, half-open, framebuffer rows (no flip) */
	float xa, xb, ya, yb;   /* the rectangle in window coordinates, xa < xb, ya < yb */
	float ua, ub, va, vb;   /* texture coordinates at xa, xb and ya, yb */
	int linear;             /* filter for this scale: 0 nearest, 1 linear (clamp to edge) */
	uint8_t color[4];       /* RGBA */
};

/* GL texture name, level 0 layer 0: 0 on success */
int fp_map_tex(unsigned tex, int write, struct fp_img *m);
void fp_unmap(struct fp_img *m);
void fp_read_rgba(const struct fp_img *m, int x, int y, uint8_t rgba[4]);

/*
 * Draws the job into dst, or with out != NULL into out (RGBA, out pointing
 * at pixel x0,y0, out_stride bytes a row), reading dst only for blending.
 */
void fp_run(const struct fp_job *j, struct fp_img *dst, const struct fp_img *src, uint8_t *out, int out_stride);

#endif
