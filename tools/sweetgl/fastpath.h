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
#define FP_TAPS 16

enum fp_kind {
	FPK_NONE,
	FPK_FILL,       /* color */
	FPK_COPY,       /* texture */
	FPK_TINT,       /* color * texture */
	FPK_ATINT,      /* color * texture.a */
	FPK_OPAQUE,     /* vec4(texture.rgb, 1) */
	FPK_ALPHA,      /* vec4(texture.a) */
	FPK_COPYN,      /* texture, then CA's noise dither */
	FPK_TINTN,      /* color * texture, then CA's noise dither */
	FPK_ATINTDK,    /* color * texture.a over the framebuffer with CA's darkenSover */
	FPK_COPYDKN,    /* texture over the framebuffer with darkenSover, then the noise dither */
	FPK_MASK,       /* color * texture * texture1.a (masked layers, nine-slices) */
	FPK_LUMC,       /* CA's luminanceCurve of the framebuffer pixel (no texture) */
	FPK_CM4,        /* the framebuffer pixel through color_matrix (4 vec3 rows: r, g, b, a), alpha kept */
	FPK_CM3,        /* the same with 3 rows (no alpha term) */
	FPK_SUPER,      /* color * CA's supercircleImage(texcoord0) (icon shapes, antialiased by fwidth) */
	FPK_CIRCLE,     /* color * CA's circleImage(texcoord0) */
	FPK_MASKSUP,    /* color * texture * supercircleImage(texcoord1).a (masked icons) */
	FPK_MIXBD,      /* mix(texture1, framebuffer, texture.a) */
	FPK_BLUR,       /* CA's Gaussian blur pass: weighted taps of texture0 at texcoord0 + offsets */
	FPK_COUNT
};

/* blending, as (rgb factors / alpha factors), equation ADD */
enum fp_blend {
	FPB_NONE,       /* replace */
	FPB_PREMUL,     /* ONE, ONE_MINUS_SRC_ALPHA / ONE, ONE_MINUS_SRC_ALPHA */
	FPB_STRAIGHT,   /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA / SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
	FPB_MIXED,      /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA / ONE, ONE_MINUS_SRC_ALPHA */
	FPB_ATOP,       /* DST_ALPHA, ONE_MINUS_SRC_ALPHA / DST_ALPHA, ONE_MINUS_SRC_ALPHA */
	FPB_DSTOUT,     /* ZERO, ONE_MINUS_SRC_ALPHA / ZERO, ONE_MINUS_SRC_ALPHA */
	FPB_ADD,        /* ONE, ONE / ONE, ONE */
	FPB_COUNT
};

/* 1 (default): large rectangles are split across cores with libdispatch */
extern int fp_threads;

struct fp_img {
	uint8_t *ptr;
	int w, h, stride, fmt;
	void *xfer;
	void *own;              /* converted copy (read-only maps of other formats), freed by fp_unmap */
};

struct fp_job {
	int kind;
	int blend;              /* FPB_ below: how the result combines with the framebuffer */
	int x0, y0, x1, y1;     /* pixels written, half-open, framebuffer rows (no flip) */
	float xa, xb, ya, yb;   /* the rectangle in window coordinates, xa < xb, ya < yb */
	float ua, ub, va, vb;   /* texture coordinates at xa, xb and ya, yb (swap: u at ya, yb and v at xa, xb) */
	int swap;               /* the texture turned: u follows window y, v follows x */
	int tclamp;             /* texture coordinates clamped to [cu0, cu1] x [cv0, cv1] first */
	float cu0, cu1, cv0, cv1;
	/*
	 * FPK_MASK, FPK_MIXBD: the second texture and its own coordinates, like
	 * ua..vb, swap and linear. FPK_MASKSUP: the coordinates only (no texture)
	 */
	const struct fp_img *src2;
	float u2a, u2b, v2a, v2b;
	int swap2, linear2;
	/* FPK_LUMC: the curve's coefficients (function_arg) and the vertex colour's alpha, unquantized */
	float farg[4], falpha;
	/* FPK_CM4 / FPK_CM3: color_matrix rows (the alpha row zero for CM3) */
	float cm[4][3];
	/*
	 * FPK_BLUR: taps of texture0 at (u, v) + toff[i] (texture coordinates),
	 * acc = q[0] * tw[0], then acc = q[i] * tw[i] + acc; divided by
	 * max(.001, acc.a) when bdiv. Filtered like softpipe in float
	 * (clamp to edge), linear or nearest as linear says.
	 */
	int ntap, bdiv;
	float toff[FP_TAPS][2], tw[FP_TAPS];
	int linear;             /* filter for this scale: 0 nearest, 1 linear (clamp to edge) */
	uint8_t color[4];       /* RGBA */
	/*
	 * Noise kinds: rgb = clamp(rgb + (noise.rgb - .5) * nscale * a, 0, a), noise
	 * read nearest with repeat at u = nu0 + nud * (x + .5), v = nv0 + nvd * (y + .5)
	 * (window pixel x, y)
	 */
	const struct fp_img *noise;
	float nu0, nud, nv0, nvd, nscale;
};

/*
 * GL texture name, level 0 layer 0: 0 on success. Read-only maps of other
 * plain formats (up to 1M pixels) come back as an RGBA copy. On failure
 * fp_map_why says why (FPM_) and fp_map_format holds the pipe format.
 */
enum { FPM_OK, FPM_CONTEXT, FPM_OBJECT, FPM_IMAGE, FPM_LAYOUT, FPM_FORMAT, FPM_MAP, FPM_MEMORY };
extern int fp_map_why;
extern unsigned fp_map_format;
int fp_map_tex(unsigned tex, int write, struct fp_img *m);
void fp_unmap(struct fp_img *m);
void *fp_tex_resource(unsigned tex);    /* struct pipe_resource *, not mapped; NULL if not a plain 2D texture */
void fp_read_rgba(const struct fp_img *m, int x, int y, uint8_t rgba[4]);

/*
 * Draws the job into dst, or with out != NULL into out (RGBA, out pointing
 * at pixel x0,y0, out_stride bytes a row), reading dst only for blending.
 */
void fp_run(const struct fp_job *j, struct fp_img *dst, const struct fp_img *src, uint8_t *out, int out_stride);

#endif
