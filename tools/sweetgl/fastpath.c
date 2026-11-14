/*
 * CPU fast paths, the Mesa side: texture mapping and the pixel loop (see
 * fastpath.h). Mapping goes through pipe->texture_map, so softpipe flushes
 * its queued draws and tile caches for the resource first and marks it
 * changed on unmap: direct writes stay coherent with softpipe's own.
 */
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

void
fp_run(const struct fp_job *j, struct fp_img *dst, const struct fp_img *src, uint8_t *out, int out_stride)
{
	int w = j->x1 - j->x0, h = j->y1 - j->y0;
	if (w <= 0 || h <= 0) {
		return;
	}
	int *cols = NULL, *rows = NULL;
	if (src != NULL) {
		cols = malloc(sizeof(int) * 3 * (size_t)(w + h));
		if (cols == NULL) {
			return;
		}
		rows = cols + 3 * w;
		float du = (j->ub - j->ua) / (j->xb - j->xa), dv = (j->vb - j->va) / (j->yb - j->ya);
		for (int i = 0; i < w; i++) {
			texel(j->ua + ((float)(j->x0 + i) + 0.5f - j->xa) * du, src->w, j->linear, &cols[3 * i],
			    &cols[3 * i + 1], &cols[3 * i + 2]);
		}
		for (int i = 0; i < h; i++) {
			texel(j->va + ((float)(j->y0 + i) + 0.5f - j->ya) * dv, src->h, j->linear, &rows[3 * i],
			    &rows[3 * i + 1], &rows[3 * i + 2]);
		}
	}
	const int sb = src != NULL ? bpp(src->fmt) : 0, db = 4;
	const int rgba_dst = dst->fmt == FP_RGBA || dst->fmt == FP_RGBX, x_dst = dst->fmt == FP_BGRX || dst->fmt == FP_RGBX;
	for (int y = 0; y < h; y++) {
		uint8_t *d = dst->ptr + (size_t)(j->y0 + y) * (size_t)dst->stride + (size_t)j->x0 * (size_t)db;
		uint8_t *o = out != NULL ? out + (size_t)y * (size_t)out_stride : NULL;
		const uint8_t *r0 = NULL, *r1 = NULL;
		int wy = 0;
		if (src != NULL) {
			r0 = src->ptr + (size_t)rows[3 * y] * (size_t)src->stride;
			r1 = src->ptr + (size_t)rows[3 * y + 1] * (size_t)src->stride;
			wy = rows[3 * y + 2];
		}
		for (int x = 0; x < w; x++, d += db) {
			uint8_t s[4], t[4];
			if (src != NULL) {
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
			switch (j->kind) {
			case FPK_FILL:
				memcpy(s, j->color, 4);
				break;
			case FPK_COPY:
				memcpy(s, t, 4);
				break;
			case FPK_TINT:
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
			uint8_t r[4];
			if (j->blend) {
				uint8_t dc[4];
				px_rgba(d, dst->fmt, dc);
				for (int k = 0; k < 4; k++) {
					int v = s[k] + mul8(dc[k], 255 - s[3]);
					r[k] = (uint8_t)(v > 255 ? 255 : v);
				}
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
	free(cols);
}
