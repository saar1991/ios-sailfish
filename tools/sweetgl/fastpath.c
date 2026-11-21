/*
 * CPU fast paths, the Mesa side: texture mapping and the pixel loop (see
 * fastpath.h). Mapping goes through pipe->texture_map, so softpipe flushes
 * its queued draws and tile caches for the resource first and marks it
 * changed on unmap: direct writes stay coherent with softpipe's own.
 */
#include <dispatch/dispatch.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "state_tracker/st_context.h"
#include "main/texobj.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"

#include "fastpath.h"

struct gl_context *_mesa_get_current_context(void);

static int
fmt_of(enum pipe_format f)
{
	switch (f) {
	case PIPE_FORMAT_B8G8R8A8_UNORM:
		return FP_BGRA;
	case PIPE_FORMAT_R8G8B8A8_UNORM:
		return FP_RGBA;
	case PIPE_FORMAT_B8G8R8X8_UNORM:
		return FP_BGRX;
	case PIPE_FORMAT_R8G8B8X8_UNORM:
		return FP_RGBX;
	case PIPE_FORMAT_A8_UNORM:
		return FP_A8;
	default:
		return FP_NONE;
	}
}

int fp_map_why;
unsigned fp_map_format;

static int
map_fail(int why)
{
	fp_map_why = why;
	return -1;
}

int
fp_map_tex(unsigned tex, int write, struct fp_img *m)
{
	struct gl_context *ctx = _mesa_get_current_context();
	memset(m, 0, sizeof(*m));
	fp_map_format = 0;
	if (ctx == NULL || ctx->st == NULL || tex == 0) {
		return map_fail(FPM_CONTEXT);
	}
	struct gl_texture_object *obj = _mesa_lookup_texture(ctx, tex);
	if (obj == NULL || obj->Target != GL_TEXTURE_2D || obj->pt == NULL) {
		return map_fail(FPM_OBJECT);
	}
	struct gl_texture_image *img = obj->Image[0][0];
	struct pipe_resource *res = obj->pt;
	fp_map_format = (unsigned)res->format;
	/* the image must already live in the resource the draw will use */
	if (img == NULL || img->pt != res) {
		return map_fail(FPM_IMAGE);
	}
	if (res->nr_samples > 1 || res->array_size > 1 ||
	    (res->target != PIPE_TEXTURE_2D && res->target != PIPE_TEXTURE_RECT)) {
		return map_fail(FPM_LAYOUT);
	}
	int fmt = fmt_of(res->format);
	size_t npx = (size_t)res->width0 * (size_t)res->height0;
	if (fmt == FP_NONE && (write || npx > (1u << 20) || util_format_is_compressed(res->format) ||
	    util_format_is_pure_integer(res->format) || util_format_is_depth_or_stencil(res->format))) {
		return map_fail(FPM_FORMAT);
	}
	struct pipe_context *pipe = ctx->st->pipe;
	struct pipe_transfer *xfer = NULL;
	void *p = pipe_texture_map_3d(pipe, res, 0, PIPE_MAP_READ | (write ? PIPE_MAP_WRITE : 0), 0, 0, 0,
	    res->width0, res->height0, 1, &xfer);
	if (p == NULL || xfer == NULL) {
		return map_fail(FPM_MAP);
	}
	m->w = (int)res->width0;
	m->h = (int)res->height0;
	if (fmt == FP_NONE) {
		/* another plain format, read only: an RGBA copy for this draw */
		uint8_t *copy = malloc(npx * 4);
		if (copy == NULL) {
			pipe_texture_unmap(pipe, xfer);
			return map_fail(FPM_MEMORY);
		}
		util_format_unpack_rgba_8unorm_rect(res->format, copy, (unsigned)m->w * 4, p, xfer->stride, (unsigned)m->w,
		    (unsigned)m->h);
		pipe_texture_unmap(pipe, xfer);
		m->ptr = copy;
		m->own = copy;
		m->stride = m->w * 4;
		m->fmt = FP_RGBA;
		return 0;
	}
	m->ptr = p;
	m->stride = (int)xfer->stride;
	m->fmt = fmt;
	m->xfer = xfer;
	return 0;
}

/* the resource behind GL texture name tex, level 0 of a plain 2D texture, or NULL (no mapping) */
void *
fp_tex_resource(unsigned tex)
{
	struct gl_context *ctx = _mesa_get_current_context();
	if (ctx == NULL || ctx->st == NULL || tex == 0) {
		return NULL;
	}
	struct gl_texture_object *obj = _mesa_lookup_texture(ctx, tex);
	if (obj == NULL || obj->Target != GL_TEXTURE_2D || obj->pt == NULL) {
		return NULL;
	}
	struct gl_texture_image *img = obj->Image[0][0];
	struct pipe_resource *res = obj->pt;
	if (img == NULL || img->pt != res || res->nr_samples > 1 || res->array_size > 1 ||
	    (res->target != PIPE_TEXTURE_2D && res->target != PIPE_TEXTURE_RECT)) {
		return NULL;
	}
	return res;
}

void
fp_unmap(struct fp_img *m)
{
	struct gl_context *ctx = _mesa_get_current_context();
	if (m->xfer != NULL && ctx != NULL && ctx->st != NULL) {
		pipe_texture_unmap(ctx->st->pipe, m->xfer);
	}
	free(m->own);
	m->own = NULL;
	m->xfer = NULL;
	m->ptr = NULL;
}

static inline void
px_rgba(const uint8_t *p, int fmt, uint8_t c[4])
{
	switch (fmt) {
	case FP_BGRA:
		c[0] = p[2], c[1] = p[1], c[2] = p[0], c[3] = p[3];
		break;
	case FP_BGRX:
		c[0] = p[2], c[1] = p[1], c[2] = p[0], c[3] = 255;
		break;
	case FP_RGBA:
		c[0] = p[0], c[1] = p[1], c[2] = p[2], c[3] = p[3];
		break;
	case FP_RGBX:
		c[0] = p[0], c[1] = p[1], c[2] = p[2], c[3] = 255;
		break;
	default:        /* A8 */
		c[0] = c[1] = c[2] = 0, c[3] = p[0];
		break;
	}
}

static inline int
bpp(int fmt)
{
	return fmt == FP_A8 ? 1 : 4;
}

void
fp_read_rgba(const struct fp_img *m, int x, int y, uint8_t rgba[4])
{
	px_rgba(m->ptr + (size_t)y * (size_t)m->stride + (size_t)x * (size_t)bpp(m->fmt), m->fmt, rgba);
}

/* texel index pair and weight (0..256) for coordinate u over n texels, clamp to edge */
static void
texel(float u, int n, int linear, int *i0, int *i1, int *w)
{
	if (!linear) {
		float f = floorf(u * (float)n);
		int a = f < 0.0f ? 0 : f >= (float)n ? n - 1 : (int)f;
		*i0 = *i1 = a;
		*w = 0;
		return;
	}
	float s = u * (float)n - 0.5f, f = floorf(s);
	int a = (int)f, wt = (int)((s - f) * 256.0f + 0.5f);
	if (wt >= 256) {
		a++;
		wt = 0;
	}
	int b = a + 1;
	*i0 = a < 0 ? 0 : a > n - 1 ? n - 1 : a;
	*i1 = b < 0 ? 0 : b > n - 1 ? n - 1 : b;
	*w = wt;
}

static inline uint8_t
mul8(int a, int b)
{
	return (uint8_t)((a * b + 127) / 255);
}

/* a*b + c*d over 255, rounded once and clamped (softpipe blends in float, clamps, rounds) */
static inline uint8_t
lerp8(int a, int b, int c, int d)
{
	int v = (a * b + c * d + 127) / 255;
	return (uint8_t)(v > 255 ? 255 : v);
}

int fp_threads = 1;

struct fp_rows {
	const struct fp_job *j;
	struct fp_img *dst;
	const struct fp_img *src;
	uint8_t *out;
	int out_stride, w, h, sb;
	const int *cols, *rows;
	int cols_plain;         /* no column filtering: one texel a pixel across */
	int cols_ident;         /* and consecutive: texel cols[0] + x */
	const int *ncols, *nrows;       /* noise texel per column / row */
	const int *cols2, *rows2;       /* FPK_MASK, FPK_MIXBD: the second texture's tables */
	/*
	 * Shape kinds: the shape's per-column and per-row terms (it is a sum of
	 * one term from each axis) for columns from shx0 and rows from shy0
	 */
	const float *shA, *shB;
	int shx0, shy0;
	/* FPK_BLUR: the texture, and per tap a column and a row table (fp_tap) */
	const struct fp_img *bsrc;
	const struct fp_tap *btc, *btr;
	int chunk;              /* rows a work item */
};

static inline float
clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/*
 * Texel tables for one texture over the w x h pixels at x0, y0: cols holds
 * (index, index, weight) per window column, rows per window row; for a
 * turned texture cols are texture rows (v along x) and rows texture columns.
 */
static void
tables(const struct fp_job *j, const struct fp_img *src, float ua, float ub, float va, float vb, int swap, int linear,
    int clamp, int w, int h, int *cols, int *rows)
{
	if (!swap) {
		float du = (ub - ua) / (j->xb - j->xa), dv = (vb - va) / (j->yb - j->ya);
		for (int i = 0; i < w; i++) {
			float u = ua + ((float)(j->x0 + i) + 0.5f - j->xa) * du;
			texel(clamp ? clampf(u, j->cu0, j->cu1) : u, src->w, linear, &cols[3 * i], &cols[3 * i + 1], &cols[3 * i + 2]);
		}
		for (int i = 0; i < h; i++) {
			float v = va + ((float)(j->y0 + i) + 0.5f - j->ya) * dv;
			texel(clamp ? clampf(v, j->cv0, j->cv1) : v, src->h, linear, &rows[3 * i], &rows[3 * i + 1], &rows[3 * i + 2]);
		}
	} else {
		float dv = (vb - va) / (j->xb - j->xa), du = (ub - ua) / (j->yb - j->ya);
		for (int i = 0; i < w; i++) {
			float v = va + ((float)(j->x0 + i) + 0.5f - j->xa) * dv;
			texel(clamp ? clampf(v, j->cv0, j->cv1) : v, src->h, linear, &cols[3 * i], &cols[3 * i + 1], &cols[3 * i + 2]);
		}
		for (int i = 0; i < h; i++) {
			float u = ua + ((float)(j->y0 + i) + 0.5f - j->ya) * du;
			texel(clamp ? clampf(u, j->cu0, j->cu1) : u, src->w, linear, &rows[3 * i], &rows[3 * i + 1], &rows[3 * i + 2]);
		}
	}
}

/* bilinear texel (x, y in the rectangle) from tables built by tables() */
static inline void
fetch(const struct fp_img *src, const int *cols, const int *rows, int swap, int x, int y, uint8_t t[4])
{
	const int sb = bpp(src->fmt);
	const int *c = swap ? &rows[3 * y] : &cols[3 * x], *r = swap ? &cols[3 * x] : &rows[3 * y];
	const uint8_t *q0 = src->ptr + (size_t)r[0] * (size_t)src->stride, *q1 = src->ptr + (size_t)r[1] * (size_t)src->stride;
	uint8_t a[4], b[4], e[4], f[4];
	px_rgba(q0 + (size_t)c[0] * (size_t)sb, src->fmt, a);
	px_rgba(q0 + (size_t)c[1] * (size_t)sb, src->fmt, b);
	px_rgba(q1 + (size_t)c[0] * (size_t)sb, src->fmt, e);
	px_rgba(q1 + (size_t)c[1] * (size_t)sb, src->fmt, f);
	for (int k = 0; k < 4; k++) {
		int top = a[k] * (256 - c[2]) + b[k] * c[2];
		int bot = e[k] * (256 - c[2]) + f[k] * c[2];
		t[k] = (uint8_t)((top * (256 - r[2]) + bot * r[2] + 32768) >> 16);
	}
}

/* index of coordinate u in n texels, nearest, repeat */
static int
wrap_nearest(float u, int n)
{
	float f = floorf(u * (float)n);
	long i = (long)f % n;
	return (int)(i < 0 ? i + n : i);
}

/* shader output for texel t (RGBA) */
static inline void
shade(const struct fp_job *j, const uint8_t t[4], uint8_t s[4])
{
	switch (j->kind) {
	case FPK_FILL:
		memcpy(s, j->color, 4);
		break;
	case FPK_COPY:
	case FPK_COPYN:
		memcpy(s, t, 4);
		break;
	case FPK_TINT:
	case FPK_TINTN:
		for (int k = 0; k < 4; k++) {
			s[k] = mul8(j->color[k], t[k]);
		}
		break;
	case FPK_ATINT:
		for (int k = 0; k < 4; k++) {
			s[k] = mul8(j->color[k], t[3]);
		}
		break;
	case FPK_OPAQUE:
		s[0] = t[0], s[1] = t[1], s[2] = t[2], s[3] = 255;
		break;
	default:        /* FPK_ALPHA */
		s[0] = s[1] = s[2] = s[3] = t[3];
		break;
	}
}

/* s over framebuffer colour dc (both RGBA) into r */
static inline void
blend(int mode, const uint8_t s[4], const uint8_t dc[4], uint8_t r[4])
{
	int ia = 255 - s[3];
	switch (mode) {
	case FPB_PREMUL:
		for (int k = 0; k < 4; k++) {
			int v = s[k] + mul8(dc[k], ia);
			r[k] = (uint8_t)(v > 255 ? 255 : v);
		}
		break;
	case FPB_STRAIGHT:
		for (int k = 0; k < 4; k++) {
			r[k] = lerp8(s[k], s[3], dc[k], ia);
		}
		break;
	case FPB_ADD:
		for (int k = 0; k < 4; k++) {
			int v = s[k] + dc[k];
			r[k] = (uint8_t)(v > 255 ? 255 : v);
		}
		break;
	case FPB_DSTOUT:
		for (int k = 0; k < 4; k++) {
			r[k] = mul8(dc[k], ia);
		}
		break;
	case FPB_ATOP:
		for (int k = 0; k < 4; k++) {
			r[k] = lerp8(s[k], dc[3], dc[k], ia);
		}
		break;
	case FPB_MIXED:
		for (int k = 0; k < 3; k++) {
			r[k] = lerp8(s[k], s[3], dc[k], ia);
		}
		{
			int v = s[3] + mul8(dc[3], ia);
			r[3] = (uint8_t)(v > 255 ? 255 : v);
		}
		break;
	default:
		memcpy(r, s, 4);
		break;
	}
}

/*
 * CA's supercircleImage and circleImage (GL renderer helper functions):
 *   super:  uv = max(abs(uv) * 1.199 - .199, 0); d = pow(uv.x, 2.68) + pow(uv.y, 2.68)
 *   circle: d = max(dot(uv, uv), 1e-4); d = d * inversesqrt(d)
 * then w = max(fwidth(d), 1e-4); l = clamp((w * .5 + (1 - d)) / w, 0, 1).
 * d is a sum of a term of the coordinate along x and one along y; the
 * terms are tabled per column and row. fwidth as softpipe computes it:
 * coarse, from the 2x2 quad at even x and y (top left, top right, bottom
 * left), d evaluated there like at any pixel.
 */
static inline float
shape_term(int kind, float c)
{
	if (kind == FPK_CIRCLE) {
		return c * c;
	}
	float a = fabsf(c) * 1.199f - 0.199f;
	return powf(a > 0.0f ? a : 0.0f, 2.68f);
}

static inline float
shape_d(int kind, float sum)
{
	if (kind != FPK_CIRCLE) {
		return sum;
	}
	float d = sum > 1e-4f ? sum : 1e-4f;
	return d * (1.0f / sqrtf(d));
}

static inline float
shape_value(const struct fp_rows *R, int X, int Y)
{
	const int kind = R->j->kind == FPK_MASKSUP ? FPK_SUPER : R->j->kind;
	const float *A = R->shA - R->shx0, *B = R->shB - R->shy0;
	int qx = X & ~1, qy = Y & ~1;
	float tl = shape_d(kind, A[qx] + B[qy]), tr = shape_d(kind, A[qx + 1] + B[qy]);
	float bl = shape_d(kind, A[qx] + B[qy + 1]), d = shape_d(kind, A[X] + B[Y]);
	float w = fabsf(tr - tl) + fabsf(bl - tl);
	w = w > 1e-4f ? w : 1e-4f;
	return clampf((w * 0.5f + (1.0f - d)) / w, 0.0f, 1.0f);
}

/*
 * One axis of a blur tap, as softpipe's wrap_linear_clamp_to_edge (i0, i1,
 * weight of i1) or wrap_nearest_clamp_to_edge (i0 == i1, f 0).
 */
struct fp_tap {
	int i0, i1;
	float f;
};

static void
tap_axis(float s, int size, int linear, struct fp_tap *t)
{
	if (linear) {
		float u = s * (float)size;
		u = (u < 0.0f ? 0.0f : u > (float)size ? (float)size : u) - 0.5f;
		int i = (int)floorf(u);
		t->f = u - floorf(u);
		t->i0 = i < 0 ? 0 : i;
		t->i1 = i + 1 >= size ? size - 1 : i + 1;
	} else {
		float u = s * (float)size;
		t->i0 = t->i1 = u < 0.5f ? 0 : u > (float)size - 0.5f ? size - 1 : (int)floorf(u);
		t->f = 0.0f;
	}
}

static inline float
lerpf(float a, float v0, float v1)
{
	return v0 + a * (v1 - v0);
}

/* the blurred pixel x, y of the job (before blending) */
static void
blur_px(const struct fp_rows *R, int x, int y, uint8_t s[4])
{
	const struct fp_job *j = R->j;
	const struct fp_img *src = R->bsrc;
	const int sb = bpp(src->fmt);
	float acc[4] = {0, 0, 0, 0};
	for (int t = 0; t < j->ntap; t++) {
		/* texture x from the window column (row when turned), texture y from the other */
		const struct fp_tap *c = &R->btc[(size_t)t * (size_t)R->w + (size_t)x], *r = &R->btr[(size_t)t * (size_t)R->h + (size_t)y];
		const struct fp_tap *tx = j->swap ? r : c, *ty = j->swap ? c : r;
		const uint8_t *row0 = src->ptr + (size_t)ty->i0 * (size_t)src->stride, *row1 = src->ptr + (size_t)ty->i1 * (size_t)src->stride;
		uint8_t a[4], b[4], e[4], f[4];
		px_rgba(row0 + (size_t)tx->i0 * (size_t)sb, src->fmt, a);
		px_rgba(row0 + (size_t)tx->i1 * (size_t)sb, src->fmt, b);
		px_rgba(row1 + (size_t)tx->i0 * (size_t)sb, src->fmt, e);
		px_rgba(row1 + (size_t)tx->i1 * (size_t)sb, src->fmt, f);
		for (int k = 0; k < 4; k++) {
			const float q = lerpf(ty->f, lerpf(tx->f, (float)a[k] * (1.0f / 255.0f), (float)b[k] * (1.0f / 255.0f)),
			    lerpf(tx->f, (float)e[k] * (1.0f / 255.0f), (float)f[k] * (1.0f / 255.0f)));
			acc[k] = q * j->tw[t] + acc[k];
		}
	}
	if (j->bdiv) {
		float m = acc[3] > 0.001f ? acc[3] : 0.001f;
		for (int k = 0; k < 4; k++) {
			acc[k] /= m;
		}
	}
	for (int k = 0; k < 4; k++) {
		s[k] = (uint8_t)(clampf(acc[k], 0.0f, 1.0f) * 255.0f + 0.5f);
	}
}

static void
fp_row(const struct fp_rows *R, int y)
{
	const struct fp_job *j = R->j;
	const struct fp_img *src = R->src;
	struct fp_img *dst = R->dst;
	const int w = R->w, sb = R->sb, *cols = R->cols;
	uint8_t *d = dst->ptr + (size_t)(j->y0 + y) * (size_t)dst->stride + (size_t)j->x0 * 4;
	uint8_t *o = R->out != NULL ? R->out + (size_t)y * (size_t)R->out_stride : NULL;
	const uint8_t *r0 = NULL, *r1 = NULL;
	int wy = 0;
	if (src != NULL && !j->swap) {
		r0 = src->ptr + (size_t)R->rows[3 * y] * (size_t)src->stride;
		r1 = src->ptr + (size_t)R->rows[3 * y + 1] * (size_t)src->stride;
		wy = R->rows[3 * y + 2];
	}
	/*
	 * Pixel for pixel copies between same-order 4-byte formats, written in
	 * place: CA's layer contents are mostly drawn 1:1.
	 */
	if (src != NULL && !j->swap && R->cols_ident && wy == 0 && o == NULL && src->fmt == dst->fmt &&
	    (dst->fmt == FP_BGRA || dst->fmt == FP_RGBA)) {
		const uint8_t *s = r0 + (size_t)cols[0] * 4;
		if (j->kind == FPK_COPY && j->blend == FPB_NONE) {
			memcpy(d, s, (size_t)w * 4);
			return;
		}
		if (j->kind == FPK_OPAQUE && j->blend == FPB_NONE) {
			memcpy(d, s, (size_t)w * 4);
			for (int x = 0; x < w; x++) {
				d[4 * x + 3] = 255;
			}
			return;
		}
		if (j->kind == FPK_COPY && j->blend == FPB_PREMUL) {
			/* channel order is the same on both sides, alpha last in both */
			for (int x = 0; x < w; x++, s += 4, d += 4) {
				int ia = 255 - s[3];
				if (ia == 0) {
					memcpy(d, s, 4);
				} else if (ia == 255 && (s[0] | s[1] | s[2]) == 0) {
					continue;
				} else {
					for (int k = 0; k < 4; k++) {
						int v = s[k] + mul8(d[k], ia);
						d[k] = (uint8_t)(v > 255 ? 255 : v);
					}
				}
			}
			return;
		}
	}
	const int plain = src != NULL && !j->swap && R->cols_plain && wy == 0;
	const int rgba_dst = dst->fmt == FP_RGBA || dst->fmt == FP_RGBX, x_dst = dst->fmt == FP_BGRX || dst->fmt == FP_RGBX;
	for (int x = 0; x < w; x++, d += 4) {
		uint8_t t[4] = {0}, s[4], r[4];
		if (plain) {
			px_rgba(r0 + (size_t)cols[3 * x] * (size_t)sb, src->fmt, t);
		} else if (src != NULL && j->swap) {
			/* turned: the texture column comes from the window row, the texture row from the window column */
			fetch(src, cols, R->rows, 1, x, y, t);
		} else if (src != NULL) {
			const int *c = &cols[3 * x];
			uint8_t a[4], b[4], e[4], f[4];
			px_rgba(r0 + (size_t)c[0] * (size_t)sb, src->fmt, a);
			px_rgba(r0 + (size_t)c[1] * (size_t)sb, src->fmt, b);
			px_rgba(r1 + (size_t)c[0] * (size_t)sb, src->fmt, e);
			px_rgba(r1 + (size_t)c[1] * (size_t)sb, src->fmt, f);
			for (int k = 0; k < 4; k++) {
				int top = a[k] * (256 - c[2]) + b[k] * c[2];
				int bot = e[k] * (256 - c[2]) + f[k] * c[2];
				t[k] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
			}
		}
		int darken = j->kind == FPK_ATINTDK || j->kind == FPK_COPYDKN;
		uint8_t dc[4];
		int backdrop = j->kind == FPK_LUMC || j->kind == FPK_CM4 || j->kind == FPK_CM3 || j->kind == FPK_MIXBD;
		if (darken || backdrop || j->blend != FPB_NONE) {
			px_rgba(d, dst->fmt, dc);
		}
		if (j->kind == FPK_CM4 || j->kind == FPK_CM3) {
			/* vec4(D.r * m0 + D.g * m1 + D.b * m2 (+ D.a * m3), D.a), in float */
			float D[4];
			for (int k = 0; k < 4; k++) {
				D[k] = (float)dc[k] / 255.0f;
			}
			for (int k = 0; k < 3; k++) {
				float c = D[0] * j->cm[0][k] + D[1] * j->cm[1][k] + D[2] * j->cm[2][k] + D[3] * j->cm[3][k];
				s[k] = (uint8_t)(clampf(c, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
			s[3] = dc[3];
		} else if (j->kind == FPK_LUMC) {
			/* luminanceCurve(D, p, a): x = curve(luma(D)); vec4(mix(D.rgb, vec3(x), a), D.a), in float */
			float D[4];
			for (int k = 0; k < 4; k++) {
				D[k] = (float)dc[k] / 255.0f;
			}
			float xl = D[0] * 0.2125f + D[1] * 0.7154f + D[2] * 0.0721f;
			xl = clampf(((j->farg[0] * xl + j->farg[1]) * xl + j->farg[2]) * xl + j->farg[3], 0.0f, 1.0f);
			for (int k = 0; k < 3; k++) {
				float c = D[k] * (1.0f - j->falpha) + xl * j->falpha;
				s[k] = (uint8_t)(clampf(c, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
			s[3] = dc[3];
		} else if (darken) {
			/*
			 * CA's darkenSover(S, D) with D the framebuffer before the draw:
			 * y = luma(D); S *= y^4; R = D * (1 - S.a) + S. Then the dither
			 * (on R) for the noise variant. In float like the shader.
			 */
			float S[4], D[4], Rf[4];
			for (int k = 0; k < 4; k++) {
				D[k] = (float)dc[k] / 255.0f;
				S[k] = j->kind == FPK_ATINTDK ? ((float)j->color[k] / 255.0f) * ((float)t[3] / 255.0f) : (float)t[k] / 255.0f;
			}
			float lum = D[0] * 0.2125f + D[1] * 0.7154f + D[2] * 0.0721f, y4 = (lum * lum) * (lum * lum);
			for (int k = 0; k < 4; k++) {
				Rf[k] = D[k] * (1.0f - S[3] * y4) + S[k] * y4;
			}
			if (R->ncols != NULL) {
				uint8_t nz[4];
				px_rgba(j->noise->ptr + (size_t)R->nrows[y] * (size_t)j->noise->stride +
				    (size_t)R->ncols[x] * (size_t)bpp(j->noise->fmt), j->noise->fmt, nz);
				for (int k = 0; k < 3; k++) {
					float c = Rf[k] + ((float)nz[k] / 255.0f - 0.5f) * j->nscale * Rf[3];
					Rf[k] = clampf(c, 0.0f, Rf[3]);
				}
			}
			for (int k = 0; k < 4; k++) {
				float c = clampf(Rf[k], 0.0f, 1.0f);
				s[k] = (uint8_t)(c * 255.0f + 0.5f);
			}
		} else if (j->kind == FPK_BLUR) {
			blur_px(R, x, y, s);
		} else if (j->kind == FPK_SUPER || j->kind == FPK_CIRCLE || j->kind == FPK_MASKSUP) {
			/* color * vec4(l) (* s0), in float like the shader */
			float l = shape_value(R, j->x0 + x, j->y0 + y);
			for (int k = 0; k < 4; k++) {
				float c = ((float)j->color[k] / 255.0f) * l;
				c = j->kind == FPK_MASKSUP ? ((float)j->color[k] / 255.0f) * ((float)t[k] / 255.0f) * l : c;
				s[k] = (uint8_t)(clampf(c, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		} else if (j->kind == FPK_MIXBD) {
			/* mix(s1, d0, s0.a), in float */
			uint8_t t2[4];
			fetch(j->src2, R->cols2, R->rows2, j->swap2, x, y, t2);
			float a = (float)t[3] / 255.0f;
			for (int k = 0; k < 4; k++) {
				float c = ((float)t2[k] / 255.0f) * (1.0f - a) + ((float)dc[k] / 255.0f) * a;
				s[k] = (uint8_t)(clampf(c, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		} else if (j->kind == FPK_MASK) {
			/* color * s0 * s1.a in float, like the shader */
			uint8_t t2[4];
			fetch(j->src2, R->cols2, R->rows2, j->swap2, x, y, t2);
			for (int k = 0; k < 4; k++) {
				float c = ((float)j->color[k] / 255.0f) * ((float)t[k] / 255.0f) * ((float)t2[3] / 255.0f);
				s[k] = (uint8_t)(clampf(c, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		} else {
			shade(j, t, s);
		}
		if (!darken && R->ncols != NULL) {
			/* CA's dither, in float like the shader */
			uint8_t nz[4];
			px_rgba(j->noise->ptr + (size_t)R->nrows[y] * (size_t)j->noise->stride +
			    (size_t)R->ncols[x] * (size_t)bpp(j->noise->fmt), j->noise->fmt, nz);
			float a = (float)s[3] / 255.0f;
			for (int k = 0; k < 3; k++) {
				float n = ((float)nz[k] / 255.0f - 0.5f) * j->nscale;
				float c = (float)s[k] / 255.0f + n * a;
				c = c < 0.0f ? 0.0f : c > a ? a : c;
				s[k] = (uint8_t)(c * 255.0f + 0.5f);
			}
		}
		if (j->blend != FPB_NONE) {
			blend(j->blend, s, dc, r);
		} else {
			memcpy(r, s, 4);
		}
		if (x_dst) {
			r[3] = 255;
		}
		if (o != NULL) {
			memcpy(o + 4 * x, r, 4);
		} else if (rgba_dst) {
			memcpy(d, r, 4);
		} else {
			d[0] = r[2], d[1] = r[1], d[2] = r[0], d[3] = r[3];
		}
	}
}

#define FP_CHUNK 16     /* rows a dispatch work item */

static void
fp_chunk(void *ctx, size_t i)
{
	const struct fp_rows *R = ctx;
	int y1 = (int)(i + 1) * R->chunk;
	for (int y = (int)i * R->chunk; y < (y1 < R->h ? y1 : R->h); y++) {
		fp_row(R, y);
	}
}

void
fp_run(const struct fp_job *j, struct fp_img *dst, const struct fp_img *src, uint8_t *out, int out_stride)
{
	int w = j->x1 - j->x0, h = j->y1 - j->y0;
	if (w <= 0 || h <= 0) {
		return;
	}
	struct fp_rows R = {.j = j, .dst = dst, .src = src, .out = out, .out_stride = out_stride, .w = w, .h = h};
	struct fp_tap *btab = NULL;
	if (j->kind == FPK_BLUR) {
		/* the taps' tables; the pixel loop reads the texture through them (not cols and rows) */
		if (src == NULL || j->ntap <= 0 || j->ntap > FP_TAPS ||
		    (btab = malloc(sizeof(*btab) * (size_t)j->ntap * (size_t)(w + h))) == NULL) {
			return;
		}
		const float dx = (j->swap ? j->vb - j->va : j->ub - j->ua) / (j->xb - j->xa);
		const float dy = (j->swap ? j->ub - j->ua : j->vb - j->va) / (j->yb - j->ya);
		const float cx0 = j->swap ? j->va : j->ua, cy0 = j->swap ? j->ua : j->va;
		const int sx = j->swap ? src->h : src->w, sy = j->swap ? src->w : src->h;
		for (int t = 0; t < j->ntap; t++) {
			const float ox = j->toff[t][j->swap ? 1 : 0], oy = j->toff[t][j->swap ? 0 : 1];
			for (int i = 0; i < w; i++) {
				tap_axis(cx0 + ((float)(j->x0 + i) + 0.5f - j->xa) * dx + ox, sx, j->linear,
				    &btab[(size_t)t * (size_t)w + (size_t)i]);
			}
			for (int i = 0; i < h; i++) {
				tap_axis(cy0 + ((float)(j->y0 + i) + 0.5f - j->ya) * dy + oy, sy, j->linear,
				    &btab[(size_t)j->ntap * (size_t)w + (size_t)t * (size_t)h + (size_t)i]);
			}
		}
		R.bsrc = src, R.btc = btab, R.btr = btab + (size_t)j->ntap * (size_t)w;
		R.src = src = NULL;
	}
	int *cols = NULL;
	if (src != NULL) {
		cols = malloc(sizeof(int) * 3 * (size_t)(w + h));
		if (cols == NULL) {
			free(btab);
			return;
		}
		int *rows = cols + 3 * w;
		tables(j, src, j->ua, j->ub, j->va, j->vb, j->swap, j->linear, j->tclamp, w, h, cols, rows);
		R.cols_plain = R.cols_ident = !j->swap;
		for (int i = 0; i < w && !j->swap; i++) {
			R.cols_plain &= cols[3 * i + 2] == 0;
			R.cols_ident &= cols[3 * i + 2] == 0 && cols[3 * i] == cols[0] + i;
		}
		R.cols = cols, R.rows = rows, R.sb = bpp(src->fmt);
	}
	float *shtab = NULL;
	if (j->kind == FPK_SUPER || j->kind == FPK_CIRCLE || j->kind == FPK_MASKSUP) {
		/* terms for columns x0 & ~1 .. x1 and rows y0 & ~1 .. y1 (the quads' far pixels included) */
		const int kind = j->kind == FPK_MASKSUP ? FPK_SUPER : j->kind, two = j->kind == FPK_MASKSUP;
		const float ua = two ? j->u2a : j->ua, ub = two ? j->u2b : j->ub, va = two ? j->v2a : j->va,
		    vb = two ? j->v2b : j->vb;
		const int swap = two ? j->swap2 : j->swap, sx0 = j->x0 & ~1, sy0 = j->y0 & ~1;
		const int nx = j->x1 - sx0 + 1, ny = j->y1 - sy0 + 1;
		if ((shtab = malloc(sizeof(float) * (size_t)(nx + ny))) == NULL) {
			free(cols);
			free(btab);
			return;
		}
		/* the coordinate along x: u (v when turned), along y: v (u) */
		float xa0 = swap ? va : ua, xa1 = swap ? vb : ub, ya0 = swap ? ua : va, ya1 = swap ? ub : vb;
		float dx = (xa1 - xa0) / (j->xb - j->xa), dy = (ya1 - ya0) / (j->yb - j->ya);
		for (int i = 0; i < nx; i++) {
			shtab[i] = shape_term(kind, xa0 + ((float)(sx0 + i) + 0.5f - j->xa) * dx);
		}
		for (int i = 0; i < ny; i++) {
			shtab[nx + i] = shape_term(kind, ya0 + ((float)(sy0 + i) + 0.5f - j->ya) * dy);
		}
		R.shA = shtab, R.shB = shtab + nx, R.shx0 = sx0, R.shy0 = sy0;
	}
	int *mtab = NULL;
	if (j->kind == FPK_MASK || j->kind == FPK_MIXBD) {
		if (j->src2 == NULL || src == NULL || (mtab = malloc(sizeof(int) * 3 * (size_t)(w + h))) == NULL) {
			free(shtab);
			free(cols);
			free(btab);
			return;
		}
		tables(j, j->src2, j->u2a, j->u2b, j->v2a, j->v2b, j->swap2, j->linear2, 0, w, h, mtab, mtab + 3 * w);
		R.cols2 = mtab, R.rows2 = mtab + 3 * w;
	}
	int *ntab = NULL;
	if (j->noise != NULL) {
		ntab = malloc(sizeof(int) * (size_t)(w + h));
		if (ntab == NULL) {
			free(mtab);
			free(shtab);
			free(cols);
			free(btab);
			return;
		}
		for (int i = 0; i < w; i++) {
			ntab[i] = wrap_nearest(j->nu0 + j->nud * ((float)(j->x0 + i) + 0.5f), j->noise->w);
		}
		for (int i = 0; i < h; i++) {
			ntab[w + i] = wrap_nearest(j->nv0 + j->nvd * ((float)(j->y0 + i) + 0.5f), j->noise->h);
		}
		R.ncols = ntab, R.nrows = ntab + w;
	}
	/*
	 * Threads from 4096 pixels (was 65536): the d8a55df run took about
	 * 180 ns a pixel for small tints, 6-8 times the host's, as if the
	 * drawing thread ran on an A55 at 1 GHz; the A78s can take part.
	 * Rows a work item: 16, fewer for short rectangles (at least 8 items).
	 */
	R.chunk = h >= 8 * FP_CHUNK ? FP_CHUNK : h >= 8 ? h / 8 : 1;
	size_t chunks = ((size_t)h + (size_t)R.chunk - 1) / (size_t)R.chunk;
	if (fp_threads && (size_t)w * (size_t)h >= 4096 && chunks > 1) {
		dispatch_apply_f(chunks, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), &R, fp_chunk);
	} else {
		for (size_t i = 0; i < chunks; i++) {
			fp_chunk(&R, i);
		}
	}
	free(ntab);
	free(mtab);
	free(shtab);
	free(cols);
	free(btab);
}
