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

int
fp_map_tex(unsigned tex, int write, struct fp_img *m)
{
	struct gl_context *ctx = _mesa_get_current_context();
	if (ctx == NULL || ctx->st == NULL || tex == 0) {
		return -1;
	}
	struct gl_texture_object *obj = _mesa_lookup_texture(ctx, tex);
	if (obj == NULL || obj->Target != GL_TEXTURE_2D || obj->pt == NULL) {
		return -1;
	}
	struct gl_texture_image *img = obj->Image[0][0];
	struct pipe_resource *res = obj->pt;
	/* the image must already live in the resource the draw will use */
	if (img == NULL || img->pt != res || res->nr_samples > 1 || res->array_size > 1 ||
	    (res->target != PIPE_TEXTURE_2D && res->target != PIPE_TEXTURE_RECT)) {
		return -1;
	}
	int fmt = fmt_of(res->format);
	if (fmt == FP_NONE) {
		return -1;
	}
	struct pipe_context *pipe = ctx->st->pipe;
	struct pipe_transfer *xfer = NULL;
	void *p = pipe_texture_map_3d(pipe, res, 0, PIPE_MAP_READ | (write ? PIPE_MAP_WRITE : 0), 0, 0, 0,
	    res->width0, res->height0, 1, &xfer);
	if (p == NULL || xfer == NULL) {
		return -1;
	}
	m->ptr = p;
	m->w = (int)res->width0;
	m->h = (int)res->height0;
	m->stride = (int)xfer->stride;
	m->fmt = fmt;
	m->xfer = xfer;
	return 0;
}

void
fp_unmap(struct fp_img *m)
{
	struct gl_context *ctx = _mesa_get_current_context();
	if (m->xfer != NULL && ctx != NULL && ctx->st != NULL) {
		pipe_texture_unmap(ctx->st->pipe, m->xfer);
	}
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
};

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
	if (src != NULL) {
		r0 = src->ptr + (size_t)R->rows[3 * y] * (size_t)src->stride;
		r1 = src->ptr + (size_t)R->rows[3 * y + 1] * (size_t)src->stride;
		wy = R->rows[3 * y + 2];
	}
	/*
	 * Pixel for pixel copies between same-order 4-byte formats, written in
	 * place: CA's layer contents are mostly drawn 1:1.
	 */
	if (src != NULL && R->cols_ident && wy == 0 && o == NULL && src->fmt == dst->fmt &&
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
	const int plain = src != NULL && R->cols_plain && wy == 0;
	const int rgba_dst = dst->fmt == FP_RGBA || dst->fmt == FP_RGBX, x_dst = dst->fmt == FP_BGRX || dst->fmt == FP_RGBX;
	for (int x = 0; x < w; x++, d += 4) {
		uint8_t t[4] = {0}, s[4], r[4];
		if (plain) {
			px_rgba(r0 + (size_t)cols[3 * x] * (size_t)sb, src->fmt, t);
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
		shade(j, t, s);
		if (R->ncols != NULL) {
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
			uint8_t dc[4];
			px_rgba(d, dst->fmt, dc);
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
	int y1 = (int)(i + 1) * FP_CHUNK;
	for (int y = (int)i * FP_CHUNK; y < (y1 < R->h ? y1 : R->h); y++) {
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
	int *cols = NULL;
	if (src != NULL) {
		cols = malloc(sizeof(int) * 3 * (size_t)(w + h));
		if (cols == NULL) {
			return;
		}
		int *rows = cols + 3 * w;
		float du = (j->ub - j->ua) / (j->xb - j->xa), dv = (j->vb - j->va) / (j->yb - j->ya);
		R.cols_plain = R.cols_ident = 1;
		for (int i = 0; i < w; i++) {
			texel(j->ua + ((float)(j->x0 + i) + 0.5f - j->xa) * du, src->w, j->linear, &cols[3 * i],
			    &cols[3 * i + 1], &cols[3 * i + 2]);
			R.cols_plain &= cols[3 * i + 2] == 0;
			R.cols_ident &= cols[3 * i + 2] == 0 && cols[3 * i] == cols[0] + i;
		}
		for (int i = 0; i < h; i++) {
			texel(j->va + ((float)(j->y0 + i) + 0.5f - j->ya) * dv, src->h, j->linear, &rows[3 * i],
			    &rows[3 * i + 1], &rows[3 * i + 2]);
		}
		R.cols = cols, R.rows = rows, R.sb = bpp(src->fmt);
	}
	int *ntab = NULL;
	if (j->noise != NULL) {
		ntab = malloc(sizeof(int) * (size_t)(w + h));
		if (ntab == NULL) {
			free(cols);
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
	size_t chunks = ((size_t)h + FP_CHUNK - 1) / FP_CHUNK;
	if (fp_threads && (size_t)w * (size_t)h >= 65536 && chunks > 1) {
		dispatch_apply_f(chunks, dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), &R, fp_chunk);
	} else {
		for (size_t i = 0; i < chunks; i++) {
			fp_chunk(&R, i);
		}
	}
	free(ntab);
	free(cols);
}
