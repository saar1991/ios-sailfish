#!/usr/bin/env python3
# sweetgl part 3 (called by lp-sweet.py): Core Animation's plain draws drawn
# directly by llvmpipe's rasterizer threads. See sweet_lpfast.h.
#
#   eagl.c fast_draw()  -> sweet_lpf_pending (rectangles, kind, blend, texture)
#   llvmpipe_draw_vbo() -> sweet_lp_fast_draw(): every rectangle a fill or a
#                          1:1 texel copy (no scaling, texel centres on pixel
#                          centres, inside the texture)? Then one
#                          LP_RAST_OP_SWEET_FAST command per covered tile and
#                          no vertex pipeline / JIT shader; else draw normally.
#   rasterizer thread   -> lp_rast_sweet_fast(): NEON row loops: copy (channel
#                          order by table lookup), fill, premultiplied
#                          source-over (exactly rounded /255, saturating add).
# Scenes, ordering and pipelining stay llvmpipe's: the command sits in the
# bins between the draws before and after it. Environment SGLLPFAST=0 (read
# in eagl.c) turns it off.
import os, shutil


def run(M, LP, patch, here):
    shutil.copy(os.path.join(here, 'sweet_lpfast.h'), os.path.join(LP, 'lp_sweet_fast.h'))

    # ---- lp_rast.h: the command, its argument, the rectangle it draws
    patch('lp_rast.h', [
        ('''  LP_RAST_OP_BLIT =              0x29,  /* Keep at end */
  LP_RAST_OP_MAX =               0x2a,''', '''  LP_RAST_OP_BLIT =              0x29,  /* Keep at end */
  LP_RAST_OP_SWEET_FAST =        0x2a,  /* sweetgl: lp-fast.py */
  LP_RAST_OP_MAX =               0x2b,'''),
        ('''   struct lp_fence *fence;
   struct llvmpipe_query *query_obj;
};''', '''   struct lp_fence *fence;
   struct llvmpipe_query *query_obj;
   const struct sweet_fast_rect *sweet;   /* sweetgl: LP_RAST_OP_SWEET_FAST */
};

/* sweetgl (lp-fast.py): one rectangle drawn by LP_RAST_OP_SWEET_FAST, in scene memory */
struct sweet_fast_rect {
   int disable;               /* set when binning ran out of memory half way */
   int blend;                 /* 0 replace, 1 premultiplied source-over */
   int x0, y0, x1, y1;        /* framebuffer pixels, half-open */
   const uint8_t *src;        /* texel for pixel (x0, y0); NULL: fill */
   ptrdiff_t src_dy;          /* bytes from one pixel row's texels to the next row's */
   uint8_t perm[16];          /* destination byte i of 4 pixels <- source byte perm[i] */
   uint8_t orm[16];           /* OR-ed after the lookup (opaque alpha) */
   uint32_t color;            /* fill: the pixel, destination byte order */
   /* scaled copies (bilinear or nearest, clamp to edge): per destination column / row
    * (index from x0 / y0) the left / top texel of a 2x2 pair and the weight of the
    * right / bottom one in 128ths */
   int scaled;
   const int32_t *cx, *ry;
   const uint8_t *wx, *wy;
   const uint8_t *sbase;      /* texel 0,0 */
   ptrdiff_t sstride;
   /* COPYN: CA's dither, rgb += (noise.rgb - .5) * nscale * a clamped to [0, a], after the
    * texture and before blending; noise texel per destination column / row (repeat, nearest) */
   int noise;
   const int32_t *ncx, *nry;
   const uint8_t *nbase;
   ptrdiff_t nstride;
   uint8_t nperm[16];         /* noise bytes into destination order */
   float nk;                  /* nscale / 255 */
   /* half-float RGBA sources (8 bytes a texel): filtered and dithered in float, then
    * quantized to RGBA8 and drawn by the 8-bit row (perm from RGBA order) */
   int f16;
   uint8_t npos[4];           /* noise byte of R, G, B, A */
   uint8_t opaque;            /* alpha 1 (OPAQUE kind, RGBX source) */
   /* a turned texture one texel high (CA's gradient ramps): texture x follows window y,
    * so a row is one colour: ry / wy are the texel pair along x per destination row */
   int swap1;
   uint8_t spos[4];           /* 8-bit sources in the float path: byte of R, G, B, A */
};'''),
    ])

    # ---- lp_rast.c: tables, the command
    RAST_CMD = r'''

/* sweetgl (lp-fast.py): LP_RAST_OP_SWEET_FAST */
#include "util/half_float.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

static inline uint8_t
sweet_div255(unsigned t)
{
   t += 128;
   return (uint8_t)((t + (t >> 8)) >> 8);
}

/* CA's dither on one pixel in destination order (alpha byte 3), noise in the same order */
static inline void
sweet_dither_px(uint8_t px[4], const uint8_t nz[4], float nk)
{
   float a = px[3];
   for (int c = 0; c < 3; c++) {
      float v = px[c] + ((float)nz[c] - 127.5f) * nk * a;
      v = v < 0.0f ? 0.0f : v > a ? a : v;
      px[c] = (uint8_t)(v + 0.5f);
   }
}

static void
sweet_fast_row(const struct sweet_fast_rect *r, uint8_t *d, const uint8_t *s, int n, const uint8_t *nrow,
               const int32_t *ncx)
{
   int i = 0;
#if defined(__aarch64__)
   static const uint8_t aidx_b[16] = {3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15};
   const uint8x16_t aidx = vld1q_u8(aidx_b);
   const uint8x16_t perm = vld1q_u8(r->perm), orm = vld1q_u8(r->orm);
   const uint8x16_t fill = vreinterpretq_u8_u32(vdupq_n_u32(r->color));
   const uint8x16_t nperm = vld1q_u8(r->nperm);
   const float32x4_t half = vdupq_n_f32(127.5f), zero = vdupq_n_f32(0.0f);
   for (; i + 4 <= n; i += 4) {
      uint8x16_t sv = s ? vorrq_u8(vqtbl1q_u8(vld1q_u8(s + 4 * i), perm), orm) : fill;
      if (nrow) {
         uint32_t nb[4];
         for (int q = 0; q < 4; q++)
            memcpy(&nb[q], nrow + 4 * ncx[i + q], 4);
         uint8x16_t nv = vqtbl1q_u8(vreinterpretq_u8_u32(vld1q_u32(nb)), nperm);
         uint16x8_t s16[2] = {vmovl_u8(vget_low_u8(sv)), vmovl_high_u8(sv)};
         uint16x8_t n16[2] = {vmovl_u8(vget_low_u8(nv)), vmovl_high_u8(nv)};
         uint16x4_t o16[4];
         for (int q = 0; q < 4; q++) {
            uint16x4_t sq = q & 1 ? vget_high_u16(s16[q >> 1]) : vget_low_u16(s16[q >> 1]);
            uint16x4_t nq = q & 1 ? vget_high_u16(n16[q >> 1]) : vget_low_u16(n16[q >> 1]);
            float32x4_t sf = vcvtq_f32_u32(vmovl_u16(sq)), nf = vcvtq_f32_u32(vmovl_u16(nq));
            float32x4_t a = vdupq_laneq_f32(sf, 3);
            float32x4_t c = vmlaq_f32(sf, vmulq_n_f32(vsubq_f32(nf, half), r->nk), a);
            c = vminq_f32(vmaxq_f32(c, zero), a);
            c = vcopyq_laneq_f32(c, 3, sf, 3);
            o16[q] = vmovn_u32(vcvtnq_u32_f32(c));
         }
         sv = vcombine_u8(vmovn_u16(vcombine_u16(o16[0], o16[1])), vmovn_u16(vcombine_u16(o16[2], o16[3])));
      }
      if (r->blend) {
         uint8x16_t dv = vld1q_u8(d + 4 * i);
         uint8x16_t ia = vmvnq_u8(vqtbl1q_u8(sv, aidx));
         uint16x8_t lo = vmull_u8(vget_low_u8(dv), vget_low_u8(ia));
         uint16x8_t hi = vmull_high_u8(dv, ia);
         /* (t + 128 + ((t + 128) >> 8)) >> 8: t / 255, rounded */
         uint8x16_t q = vcombine_u8(vrshrn_n_u16(vrsraq_n_u16(lo, lo, 8), 8),
                                    vrshrn_n_u16(vrsraq_n_u16(hi, hi, 8), 8));
         sv = vqaddq_u8(sv, q);
      }
      vst1q_u8(d + 4 * i, sv);
   }
#endif
   for (; i < n; i++) {
      uint8_t px[4], *dp = d + 4 * i;
      for (int c = 0; c < 4; c++) {
         px[c] = s ? (uint8_t)((r->perm[c] < 4 ? s[4 * i + r->perm[c]] : 0) | r->orm[c])
                   : (uint8_t)(r->color >> (8 * c));
      }
      if (nrow) {
         uint8_t nz[4];
         for (int c = 0; c < 4; c++)
            nz[c] = nrow[4 * ncx[i] + (r->nperm[c] & 3)];
         sweet_dither_px(px, nz, r->nk);
      }
      if (r->blend) {
         unsigned ia = 255u - px[3];
         for (int c = 0; c < 4; c++) {
            unsigned v = px[c] + sweet_div255(dp[c] * ia);
            px[c] = (uint8_t)(v > 255 ? 255 : v);
         }
      }
      memcpy(dp, px, 4);
   }
}

/* one destination row of a scaled copy into out (source byte order), n <= TILE_SIZE */
static void
sweet_resample_row(const struct sweet_fast_rect *r, int k0, int n, int ky, uint8_t *out)
{
   const uint8_t *row0 = r->sbase + (ptrdiff_t)r->ry[ky] * r->sstride, *row1 = row0 + r->sstride;
   const unsigned v = r->wy[ky];
   for (int i = 0; i < n; i++) {
      const int c = r->cx[k0 + i];
      const unsigned w = r->wx[k0 + i];
#if defined(__aarch64__)
      uint8x8_t a = vld1_u8(row0 + 4 * c), b = vld1_u8(row1 + 4 * c);
      uint8x8_t wv = vext_u8(vdup_n_u8((uint8_t)(128 - w)), vdup_n_u8((uint8_t)w), 4);
      uint16x8_t ha = vmull_u8(a, wv), hb = vmull_u8(b, wv);
      uint16x4_t ta = vadd_u16(vget_low_u16(ha), vget_high_u16(ha));
      uint16x4_t tb = vadd_u16(vget_low_u16(hb), vget_high_u16(hb));
      uint32x4_t acc = vmull_n_u16(ta, (uint16_t)(128 - v));
      acc = vmlal_n_u16(acc, tb, (uint16_t)v);
      uint16x4_t q = vrshrn_n_u32(acc, 14);
      uint8x8_t q8 = vmovn_u16(vcombine_u16(q, q));
      vst1_lane_u32((uint32_t *)(void *)(out + 4 * i), vreinterpret_u32_u8(q8), 0);
#else
      for (int ch = 0; ch < 4; ch++) {
         unsigned ta = row0[4 * c + ch] * (128 - w) + row0[4 * c + 4 + ch] * w;
         unsigned tb = row1[4 * c + ch] * (128 - w) + row1[4 * c + 4 + ch] * w;
         out[4 * i + ch] = (uint8_t)((ta * (128 - v) + tb * v + 8192) >> 14);
      }
#endif
   }
}

/* texel i of a row as RGBA floats: half-float, or 8-bit in r->spos order */
static inline void
sweet_texel_f(const struct sweet_fast_rect *r, const uint8_t *row, int i, float t[4])
{
   if (r->f16) {
      const uint16_t *h = (const uint16_t *)(const void *)(row + 8 * i);
      for (int ch = 0; ch < 4; ch++)
         t[ch] = _mesa_half_to_float(h[ch]);
   } else {
      for (int ch = 0; ch < 4; ch++)
         t[ch] = row[4 * i + r->spos[ch]] * (1.0f / 255);
   }
}

/*
 * One destination row into out (RGBA8) through float: half-float sources
 * (1:1, or bilinear / nearest) and turned one-texel-high ramps; clamp,
 * opaque alpha, CA's dither like the shader, quantize.
 */
static void
sweet_f16_row(const struct sweet_fast_rect *r, const uint8_t *s1, int k0, int n, int ky, const uint8_t *nrow,
              const int32_t *ncx, uint8_t *out)
{
   const uint8_t *row0 = NULL, *row1 = NULL;
   float v = 0.0f, rowc[4] = {0, 0, 0, 0};
   if (r->swap1) {
      /* one colour for the whole row: the pair along the texture's only row */
      const int c = r->ry[ky];
      const float w = r->wy[ky] * (1.0f / 128);
      float a[4], b[4];
      sweet_texel_f(r, r->sbase, c, a);
      sweet_texel_f(r, r->sbase, c + 1, b);
      for (int ch = 0; ch < 4; ch++)
         rowc[ch] = a[ch] + (b[ch] - a[ch]) * w;
   } else if (r->scaled) {
      row0 = r->sbase + (ptrdiff_t)r->ry[ky] * r->sstride;
      row1 = row0 + r->sstride;
      v = r->wy[ky] * (1.0f / 128);
   }
#if defined(__aarch64__)
   const float32x4_t zero = vdupq_n_f32(0.0f), one = vdupq_n_f32(1.0f), c255 = vdupq_n_f32(255.0f);
   const float32x4_t rowv = vld1q_f32(rowc);
   const float nk = r->nk * 255.0f / 255.0f;   /* nscale / 255: (nz - 127.5) * nk = (nz / 255 - .5) * nscale */
   for (int i = 0; i < n; i++) {
      float32x4_t tv;
      if (r->swap1) {
         tv = rowv;
      } else if (r->scaled) {
         const int c = r->cx[k0 + i];
         const float w = r->wx[k0 + i] * (1.0f / 128);
         uint16x8_t a = vld1q_u16((const uint16_t *)(const void *)(row0 + 8 * c));
         uint16x8_t b = vld1q_u16((const uint16_t *)(const void *)(row1 + 8 * c));
         float32x4_t a0 = vcvt_f32_f16(vreinterpret_f16_u16(vget_low_u16(a)));
         float32x4_t a1 = vcvt_f32_f16(vreinterpret_f16_u16(vget_high_u16(a)));
         float32x4_t b0 = vcvt_f32_f16(vreinterpret_f16_u16(vget_low_u16(b)));
         float32x4_t b1 = vcvt_f32_f16(vreinterpret_f16_u16(vget_high_u16(b)));
         float32x4_t h0 = vmlaq_n_f32(a0, vsubq_f32(a1, a0), w), h1 = vmlaq_n_f32(b0, vsubq_f32(b1, b0), w);
         tv = vmlaq_n_f32(h0, vsubq_f32(h1, h0), v);
      } else {
         tv = vcvt_f32_f16(vreinterpret_f16_u16(vld1_u16((const uint16_t *)(const void *)(s1 + 8 * i))));
      }
      tv = vminnmq_f32(vmaxnmq_f32(tv, zero), one);     /* NaN -> 0 */
      if (r->opaque)
         tv = vsetq_lane_f32(1.0f, tv, 3);
      if (nrow) {
         const uint8_t *nz = nrow + 4 * ncx[i];
         const float nzv[4] = {nz[r->npos[0]], nz[r->npos[1]], nz[r->npos[2]], 127.5f};
         float32x4_t a = vdupq_laneq_f32(tv, 3);
         float32x4_t nf = vmulq_n_f32(vsubq_f32(vld1q_f32(nzv), vdupq_n_f32(127.5f)), nk);
         float32x4_t c = vmlaq_f32(tv, nf, a);
         tv = vminq_f32(vmaxq_f32(c, zero), a);          /* alpha: + 0, clamp to itself */
      }
      uint32x4_t q = vcvtnq_u32_f32(vmulq_f32(tv, c255));
      uint16x4_t q16 = vmovn_u32(q);
      uint8x8_t q8 = vmovn_u16(vcombine_u16(q16, q16));
      vst1_lane_u32((uint32_t *)(void *)(out + 4 * i), vreinterpret_u32_u8(q8), 0);
   }
#else
   for (int i = 0; i < n; i++) {
      float t[4];
      if (r->swap1) {
         memcpy(t, rowc, sizeof(t));
      } else if (r->scaled) {
         const int c = r->cx[k0 + i];
         const float w = r->wx[k0 + i] * (1.0f / 128);
         float a0[4], a1[4], b0[4], b1[4];
         sweet_texel_f(r, row0, c, a0), sweet_texel_f(r, row0, c + 1, a1);
         sweet_texel_f(r, row1, c, b0), sweet_texel_f(r, row1, c + 1, b1);
         for (int ch = 0; ch < 4; ch++) {
            float h0 = a0[ch] + (a1[ch] - a0[ch]) * w, h1 = b0[ch] + (b1[ch] - b0[ch]) * w;
            t[ch] = h0 + (h1 - h0) * v;
         }
      } else {
         sweet_texel_f(r, s1, i, t);
      }
      for (int ch = 0; ch < 4; ch++)
         t[ch] = t[ch] > 0.0f ? (t[ch] < 1.0f ? t[ch] : 1.0f) : 0.0f;   /* also NaN -> 0 */
      if (r->opaque)
         t[3] = 1.0f;
      if (nrow) {
         /* CA's dither in float, like the shader */
         const uint8_t *nz = nrow + 4 * ncx[i];
         for (int ch = 0; ch < 3; ch++) {
            float c = t[ch] + ((float)nz[r->npos[ch]] - 127.5f) * r->nk * t[3];
            t[ch] = c < 0.0f ? 0.0f : c > t[3] ? t[3] : c;
         }
      }
      for (int ch = 0; ch < 4; ch++)
         out[4 * i + ch] = (uint8_t)(t[ch] * 255.0f + 0.5f);
   }
#endif
}

static void
lp_rast_sweet_fast(struct lp_rasterizer_task *task,
                   const union lp_rast_cmd_arg arg)
{
   const struct sweet_fast_rect *r = arg.sweet;
   const struct lp_scene *scene = task->scene;
   if (r->disable)
      return;
   int x0 = MAX2(r->x0, (int)task->x), x1 = MIN2(r->x1, (int)(task->x + task->width));
   int y0 = MAX2(r->y0, (int)task->y), y1 = MIN2(r->y1, (int)(task->y + task->height));
   if (x0 >= x1 || y0 >= y1)
      return;
   const unsigned stride = scene->cbufs[0].stride;
   uint8_t *d = scene->cbufs[0].map + (size_t)y0 * stride + (size_t)x0 * 4;
   const int32_t *ncx = r->noise ? r->ncx + (x0 - r->x0) : NULL;
#define SWEET_NROW(y) (r->noise ? r->nbase + (ptrdiff_t)r->nry[(y) - r->y0] * r->nstride : NULL)
   if (r->f16 || r->swap1) {
      /* filtered, dithered and quantized here; the 8-bit row then orders and blends */
      uint8_t tmp[TILE_SIZE * 4] __attribute__((aligned(16)));
      const uint8_t *s1 = r->src ? r->src + (ptrdiff_t)(y0 - r->y0) * r->src_dy + (ptrdiff_t)(x0 - r->x0) * 8 : NULL;
      for (int y = y0; y < y1; y++) {
         sweet_f16_row(r, s1, x0 - r->x0, x1 - x0, y - r->y0, SWEET_NROW(y), ncx, tmp);
         sweet_fast_row(r, d, tmp, x1 - x0, NULL, NULL);
         d += stride;
         if (s1)
            s1 += r->src_dy;
      }
      return;
   }
   if (r->scaled) {
      uint8_t tmp[TILE_SIZE * 4] __attribute__((aligned(16)));
      for (int y = y0; y < y1; y++) {
         sweet_resample_row(r, x0 - r->x0, x1 - x0, y - r->y0, tmp);
         sweet_fast_row(r, d, tmp, x1 - x0, SWEET_NROW(y), ncx);
         d += stride;
      }
      return;
   }
   const uint8_t *s = r->src ? r->src + (ptrdiff_t)(y0 - r->y0) * r->src_dy + (ptrdiff_t)(x0 - r->x0) * 4 : NULL;
   for (int y = y0; y < y1; y++) {
      sweet_fast_row(r, d, s, x1 - x0, SWEET_NROW(y), ncx);
      d += stride;
      if (s)
         s += r->src_dy;
   }
#undef SWEET_NROW
}
'''
    patch('lp_rast.c', [
        ('''   RECT,                        /* rectangle */
   BLIT,                        /* blit */
};''', '''   RECT,                        /* rectangle */
   BLIT,                        /* blit */
   TRI,                         /* sweet_fast */
};'''),
        ('''   NULL,                        /* rectangle */
   lp_rast_blit_tile_to_dest,
};''', '''   NULL,                        /* rectangle */
   lp_rast_blit_tile_to_dest,
   NULL,                        /* sweet_fast: TRI bins only */
};'''),
        ('''   lp_rast_rectangle,
   lp_rast_blit_tile,
};''', '''   lp_rast_rectangle,
   lp_rast_blit_tile,
   lp_rast_sweet_fast,
};'''),
        ('''   lp_rast_rectangle,
   lp_rast_shade_tile,
};''', '''   lp_rast_rectangle,
   lp_rast_shade_tile,
   lp_rast_sweet_fast,
};'''),
        ('''/* Currently have two rendering paths only - the general case triangle''', RAST_CMD.lstrip('\n') + '''

/* Currently have two rendering paths only - the general case triangle'''),
    ])
    patch('lp_rast_linear.c', [
        ('''   lp_rast_linear_rect,         /* rect */
   lp_rast_linear_tile,         /* blit */
};''', '''   lp_rast_linear_rect,         /* rect */
   lp_rast_linear_tile,         /* blit */
   NULL,                        /* sweet_fast: TRI bins only */
};'''),
    ])
    patch('lp_rast_debug.c', [
        ('''   "rectangle",
   "blit_tile",
};''', '''   "rectangle",
   "blit_tile",
   "sweet_fast",
};'''),
    ])

    # ---- lp_setup.c: take a pending draw and bin it
    SETUP_FN = r'''

/* sweetgl (lp-fast.py): Core Animation's plain draws, binned directly */
#include <math.h>
#include "lp_sweet_fast.h"
struct sweet_lpf sweet_lpf_pending;
enum { SLPF_DRAWS, SLPF_RECTS, SLPF_PIXELS, SLPF_FB, SLPF_SRC, SLPF_SCALE, SLPF_OFFSET, SLPF_BOUNDS,
       SLPF_SWAP, SLPF_CLAMP, SLPF_OOM, SLPF_KIND, SLPF_SCALED, SLPF_N };
uint64_t sweet_lpf_stat[SLPF_N];
bool sweet_lp_fast_draw(struct llvmpipe_context *lp);

/* byte position of R, G, B, A in a pixel; false for formats not handled */
static bool
sweet_lpf_order(enum pipe_format f, int pos[4], int *x)
{
   switch (f) {
   case PIPE_FORMAT_B8G8R8A8_UNORM: case PIPE_FORMAT_B8G8R8X8_UNORM:
      pos[0] = 2, pos[1] = 1, pos[2] = 0, pos[3] = 3;
      *x = f == PIPE_FORMAT_B8G8R8X8_UNORM;
      return true;
   case PIPE_FORMAT_R8G8B8A8_UNORM: case PIPE_FORMAT_R8G8B8X8_UNORM:
      pos[0] = 0, pos[1] = 1, pos[2] = 2, pos[3] = 3;
      *x = f == PIPE_FORMAT_R8G8B8X8_UNORM;
      return true;
   default:
      return false;
   }
}

extern void sweet_drm_log(const char *fmt, ...);
static unsigned sweet_lpf_logged;

/* the first 40 draws not taken, then one in 500: what they looked like */
static void
sweet_lpf_why(const char *what, const struct sweet_lpf *p, const struct sweet_lpf_job *j, int fmt, int a, int b,
              int is_map)
{
   unsigned n = sweet_lpf_logged++;
   if (n >= 40 && n % 500 != 0)
      return;
   if (j == NULL) {
      sweet_drm_log("sweetgl: lp fast no (%s): kind %d blend %d rects %d, format %d dt %d", what, p->kind, p->blend,
                    p->n, fmt, a);
      return;
   }
   double dx = (double)j->xb - j->xa, dyw = (double)j->yb - j->ya;
   (void)is_map;
   sweet_drm_log("sweetgl: lp fast no (%s): kind %d blend %d, tex %dx%d fmt %d, px %d,%d-%d,%d win %.2f,%.2f-%.2f,%.2f "
                 "uv %.5f,%.5f-%.5f,%.5f scale %.4f,%.4f swap %d clamp %d", what, p->kind, p->blend, a, b, fmt,
                 j->x0, j->y0, j->x1, j->y1, j->xa, j->ya, j->xb, j->yb, j->ua, j->va, j->ub, j->vb,
                 ((double)j->ub - j->ua) * a / dx, ((double)j->vb - j->va) * b / dyw, j->swap, j->tclamp);
}

static int sweet_lpf_last;
static int
sweet_lpf_no(int why)
{
   sweet_lpf_stat[why]++;
   sweet_lpf_last = why;
   return -1;
}

/* the source texel of pixel (x0, y0) and the row step, or -1 (counted) */
static int
sweet_lpf_map(const struct sweet_lpf_job *j, int W, int H, int *s0, int *t0, int *dy)
{
   if (j->swap)
      return sweet_lpf_no(SLPF_SWAP);
   double dx = (double)j->xb - j->xa, dyw = (double)j->yb - j->ya;
   double sx = ((double)j->ub - j->ua) * W / dx, sy = ((double)j->vb - j->va) * H / dyw;
   if (fabs(sx - 1.0) > 1e-4 || fabs(fabs(sy) - 1.0) > 1e-4)
      return sweet_lpf_no(SLPF_SCALE);
   double u = j->ua + ((double)j->ub - j->ua) * ((j->x0 + 0.5) - j->xa) / dx;
   double v = j->va + ((double)j->vb - j->va) * ((j->y0 + 0.5) - j->ya) / dyw;
   double s = u * W - 0.5, t = v * H - 0.5;
   double si = floor(s + 0.5), ti = floor(t + 0.5);
   if (fabs(s - si) > 1.0 / 512 || fabs(t - ti) > 1.0 / 512)
      return sweet_lpf_no(SLPF_OFFSET);
   *s0 = (int)si, *t0 = (int)ti, *dy = sy > 0 ? 1 : -1;
   int nx = j->x1 - j->x0, ny = j->y1 - j->y0;
   int tl = *t0 + *dy * (ny - 1);
   if (*s0 < 0 || *s0 + nx > W || *t0 < 0 || *t0 >= H || tl < 0 || tl >= H)
      return sweet_lpf_no(SLPF_BOUNDS);
   if (j->tclamp) {
      /* the clamp must not change any sample: every texel centre inside it */
      double ulo = (*s0 + 0.5) / W, uhi = (*s0 + nx - 0.5) / W;
      double vlo = ((*dy > 0 ? *t0 : tl) + 0.5) / H, vhi = ((*dy > 0 ? tl : *t0) + 0.5) / H;
      if (ulo < j->cu0 - 1e-6 || uhi > j->cu1 + 1e-6 || vlo < j->cv0 - 1e-6 || vhi > j->cv1 + 1e-6)
         return sweet_lpf_no(SLPF_CLAMP);
   }
   return 0;
}

/*
 * A scaled copy: the 2x2 texel pair and weights of every destination column
 * and row, as GL samples it (texel centres at (i + .5) / size, clamp to
 * edge, the texture's clamp rectangle first). Linear: weights in 128ths;
 * nearest: the texel itself. The tables go into scene memory with the
 * rectangle. Returns 0, or -1 when the texture or sampler does not fit.
 */
static int
sweet_lpf_axis(double a0, double a1, double ta, double tb, int p0, int n, int size, bool linear, bool clamp,
               double c0, double c1, int32_t *idx, uint8_t *wt)
{
   for (int k = 0; k < n; k++) {
      double t = ta + (tb - ta) * ((p0 + k + 0.5) - a0) / (a1 - a0);
      if (clamp)
         t = t < c0 ? c0 : t > c1 ? c1 : t;
      int i;
      unsigned w;
      if (linear) {
         double sc = t * size - 0.5, fl = floor(sc);
         i = (int)fl;
         w = (unsigned)floor((sc - fl) * 128 + 0.5);
         if (w == 128)
            i++, w = 0;
         if (i < 0)
            i = 0, w = 0;
      } else {
         i = (int)floor(t * size);
         w = 0;
         if (i < 0)
            i = 0;
      }
      if (i >= size - 1)
         i = size - 2, w = 128;       /* the last texel, as the right half of the pair */
      idx[k] = i;
      wt[k] = (uint8_t)w;
   }
   return 0;
}

/* a scaled rectangle's tables into the current scene's memory; false: out of memory */
static bool
sweet_lpf_tables(struct lp_setup_context *setup, struct sweet_fast_rect *r, const int32_t *cx, const uint8_t *wx,
                 const int32_t *nc, int nx, int ny)
{
   size_t ni = sizeof(int32_t) * (size_t)(nx + ny), nw = (size_t)(nx + ny);
   if (ni > DATA_BLOCK_SIZE)
      return false;
   if (cx) {
      int32_t *ci = lp_scene_alloc(setup->scene, (unsigned)ni);
      uint8_t *cw = ci ? lp_scene_alloc(setup->scene, (unsigned)nw) : NULL;
      if (cw == NULL)
         return false;
      memcpy(ci, cx, ni);
      memcpy(cw, wx, nw);
      r->cx = ci, r->ry = ci + nx;
      r->wx = cw, r->wy = cw + nx;
   }
   if (nc) {
      int32_t *ni2 = lp_scene_alloc(setup->scene, (unsigned)ni);
      if (ni2 == NULL)
         return false;
      memcpy(ni2, nc, ni);
      r->ncx = ni2, r->nry = ni2 + nx;
   }
   return true;
}

/* bin one rectangle into every tile it covers; false: out of scene memory */
static bool
sweet_lpf_bin(struct lp_setup_context *setup, const struct sweet_fast_rect *proto, struct sweet_fast_rect **made)
{
   struct lp_scene *scene = setup->scene;
   struct sweet_fast_rect *r = lp_scene_alloc(scene, sizeof(*r));
   *made = r;
   if (r == NULL)
      return false;
   *r = *proto;
   union lp_rast_cmd_arg arg;
   arg.sweet = r;
   int tx0 = r->x0 / TILE_SIZE, tx1 = (r->x1 - 1) / TILE_SIZE;
   int ty0 = r->y0 / TILE_SIZE, ty1 = (r->y1 - 1) / TILE_SIZE;
   for (int ty = ty0; ty <= ty1; ty++) {
      for (int tx = tx0; tx <= tx1; tx++) {
         if (!lp_scene_bin_command(scene, tx, ty, LP_RAST_OP_SWEET_FAST, arg))
            return false;
      }
   }
   return true;
}

bool
sweet_lp_fast_draw(struct llvmpipe_context *lp)
{
   struct sweet_lpf *p = &sweet_lpf_pending;
   struct lp_setup_context *setup = lp->setup;
   const struct pipe_framebuffer_state *fb = &lp->framebuffer;
   int dpos[4], dx, spos[4] = {0, 1, 2, 3}, sx = 0, W = 0, H = 0;
   struct pipe_resource *res = p->src;
   struct llvmpipe_resource *lpr = NULL;

   if (p->n <= 0 || p->n > SWEET_LPF_MAX)
      return false;
   if (!((p->kind == SWEET_LPF_FILL || p->kind == SWEET_LPF_COPY || p->kind == SWEET_LPF_OPAQUE ||
          p->kind == SWEET_LPF_COPYN) &&
         (p->blend == SWEET_LPF_BLEND_NONE || p->blend == SWEET_LPF_BLEND_PREMUL)))
      return sweet_lpf_no(SLPF_KIND), false;
   if (fb->nr_cbufs != 1 || !fb->cbufs[0].texture || fb->layers > 1 ||
       !sweet_lpf_order(fb->cbufs[0].format, dpos, &dx) ||
       lp->active_occlusion_queries || lp->active_statistics_queries || lp->active_primgen_queries)
      return sweet_lpf_no(SLPF_FB), false;
   const uint8_t *sbase = NULL;
   size_t sstride = 0;
   int sf16 = 0;      /* half-float RGBA source: 8 bytes a texel, spos stays R,G,B,A */
   const struct pipe_sampler_state *samp = NULL;
   struct sweet_lpf_tab { int32_t *cx, *ry; uint8_t *wx, *wy; int nx, ny; int32_t *nc; } tab[SWEET_LPF_MAX];
   memset(tab, 0, sizeof(tab));
   if (p->kind != SWEET_LPF_FILL) {
      lpr = res ? llvmpipe_resource(res) : NULL;
      if (lpr == NULL || res->nr_samples > 1 || res->array_size > 1 ||
          (res->target != PIPE_TEXTURE_2D && res->target != PIPE_TEXTURE_RECT) || res == fb->cbufs[0].texture) {
         sweet_lpf_why("src layout", p, NULL, res ? (int)res->format : -1, lpr ? lpr->dt != NULL : -1, 0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      if (res->format == PIPE_FORMAT_R16G16B16A16_FLOAT || res->format == PIPE_FORMAT_R16G16B16X16_FLOAT) {
         sf16 = 1;
         sx = res->format == PIPE_FORMAT_R16G16B16X16_FLOAT;
      } else if (!sweet_lpf_order(res->format, spos, &sx)) {
         sweet_lpf_why("src format", p, NULL, (int)res->format, lpr->dt != NULL, 0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      /* the texels as the JIT shader reads them for this draw: the bound view (maps display targets) */
      for (unsigned i = 0; i < setup->fs.current_tex_num && i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++) {
         const struct lp_jit_texture *jt = &setup->fs.current.jit_resources.textures[i];
         if (setup->fs.current_tex[i] == res && jt->base != NULL && jt->first_level == 0) {
            sbase = (const uint8_t *)jt->base + jt->mip_offsets[0];
            sstride = jt->row_stride[0];
            break;
         }
      }
      if (sbase != NULL) {
         for (unsigned i = 0; i < setup->fs.current_tex_num && i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++) {
            if (setup->fs.current_tex[i] == res) {
               samp = i < PIPE_MAX_SAMPLERS ? lp->samplers[MESA_SHADER_FRAGMENT][i] : NULL;
               break;
            }
         }
      }
      if (sbase == NULL || sstride == 0) {
         sweet_lpf_why("src not bound", p, NULL, (int)res->format, lpr->dt != NULL, 0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      W = (int)res->width0, H = (int)res->height0;
   }
   /* COPYN: the noise texture's texels as bound for this draw */
   struct pipe_resource *nres = p->kind == SWEET_LPF_COPYN ? p->noise : NULL;
   const uint8_t *nbase = NULL;
   size_t nstride = 0;
   int npos[4] = {0, 1, 2, 3}, nxa = 0, NW = 0, NH = 0;
   if (p->kind == SWEET_LPF_COPYN) {
      if (nres == NULL || nres->nr_samples > 1 || nres->array_size > 1 || nres->target != PIPE_TEXTURE_2D ||
          !sweet_lpf_order(nres->format, npos, &nxa)) {
         sweet_lpf_why("noise texture", p, NULL, nres ? (int)nres->format : -1, 0, 0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      for (unsigned i = 0; i < setup->fs.current_tex_num && i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++) {
         const struct lp_jit_texture *jt = &setup->fs.current.jit_resources.textures[i];
         if (setup->fs.current_tex[i] == nres && jt->base != NULL && jt->first_level == 0) {
            nbase = (const uint8_t *)jt->base + jt->mip_offsets[0];
            nstride = jt->row_stride[0];
            break;
         }
      }
      if (nbase == NULL || nstride == 0) {
         sweet_lpf_why("noise not bound", p, NULL, (int)nres->format, 0, 0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      NW = (int)nres->width0, NH = (int)nres->height0;
   }
   /* every rectangle first: all or nothing */
   struct sweet_fast_rect proto[SWEET_LPF_MAX];
   int fbw = (int)fb->width, fbh = (int)fb->height, nr = 0;
   for (int k = 0; k < p->n; k++) {
      const struct sweet_lpf_job *j = &p->job[k];
      struct sweet_fast_rect *r = &proto[nr];
      memset(r, 0, sizeof(*r));
      r->blend = p->blend == SWEET_LPF_BLEND_PREMUL;
      r->x0 = MAX2(j->x0, 0), r->x1 = MIN2(j->x1, fbw);
      r->y0 = MAX2(j->y0, 0), r->y1 = MIN2(j->y1, fbh);
      if (r->x0 >= r->x1 || r->y0 >= r->y1)
         continue;
      if (p->kind == SWEET_LPF_FILL) {
         uint8_t c[4];
         for (int ch = 0; ch < 4; ch++)
            c[dpos[ch]] = j->color[ch];
         if (dx)
            c[dpos[3]] = 255;
         memcpy(&r->color, c, 4);
      } else {
         /* clipped to the framebuffer: map the clipped corner */
         struct sweet_lpf_job jj = *j;
         jj.x0 = r->x0, jj.x1 = r->x1, jj.y0 = r->y0, jj.y1 = r->y1;
         int s0, t0, dy;
         uint64_t before[SLPF_N];
         memcpy(before, sweet_lpf_stat, sizeof(before));
         if (sweet_lpf_map(&jj, W, H, &s0, &t0, &dy) == 0) {
            r->src = sbase + (size_t)t0 * sstride + (size_t)s0 * (sf16 ? 8 : 4);
            r->src_dy = dy * (ptrdiff_t)sstride;
         } else {
            /* not 1:1: a scaled copy, when the sampler is plain (clamp to edge, level 0) */
            memcpy(sweet_lpf_stat, before, sizeof(before));
            /* turned: only a ramp one texel high (texture x along window y, one colour a row) */
            const bool sw1 = j->swap && H == 1 && W >= 2;
            double scx = sw1 ? 0.0 : fabs(((double)j->ub - j->ua) * W / ((double)j->xb - j->xa));
            double scy = sw1 ? fabs(((double)j->ub - j->ua) * W / ((double)j->yb - j->ya))
                             : fabs(((double)j->vb - j->va) * H / ((double)j->yb - j->ya));
            bool minify = scx > 1.0 + 1e-6 || scy > 1.0 + 1e-6;
            const char *no = NULL;
            if (j->swap && !sw1)
               no = "turned";
            else if (samp == NULL || samp->unnormalized_coords || res->target != PIPE_TEXTURE_2D ||
                     samp->wrap_s != PIPE_TEX_WRAP_CLAMP_TO_EDGE ||
                     (!sw1 && samp->wrap_t != PIPE_TEX_WRAP_CLAMP_TO_EDGE) ||
                     samp->min_lod > 0.0f || samp->lod_bias != 0.0f)
               no = "sampler";
            else if (minify && samp->min_mip_filter != PIPE_TEX_MIPFILTER_NONE && res->last_level > 0)
               no = "mipmaps";
            else if (W < 2 || (H < 2 && !sw1))
               no = "size";
            if (no) {
               sweet_lpf_why(no, p, &jj, (int)res->format, W, H, 1);
               for (int q = 0; q < nr; q++)
                  free(tab[q].cx), free(tab[q].wx), free(tab[q].nc);
               return sweet_lpf_no(no[0] == 't' ? SLPF_SWAP : SLPF_SCALE), false;
            }
            bool linear = (minify ? samp->min_img_filter : samp->mag_img_filter) == PIPE_TEX_FILTER_LINEAR;
            int nx = r->x1 - r->x0, ny = r->y1 - r->y0;
            tab[nr].nx = nx, tab[nr].ny = ny;
            tab[nr].cx = malloc(sizeof(int32_t) * (nx + ny));
            tab[nr].wx = malloc((size_t)nx + ny);
            if (tab[nr].cx == NULL || tab[nr].wx == NULL) {
               free(tab[nr].cx), free(tab[nr].wx);
               tab[nr].cx = NULL, tab[nr].wx = NULL;
               for (int q = 0; q < nr; q++)
                  free(tab[q].cx), free(tab[q].wx), free(tab[q].nc);
               return false;
            }
            tab[nr].ry = tab[nr].cx + nx, tab[nr].wy = tab[nr].wx + nx;
            if (sw1) {
               /* columns unused; per row the texel pair along the ramp (u along window y) */
               memset(tab[nr].cx, 0, sizeof(int32_t) * nx);
               memset(tab[nr].wx, 0, (size_t)nx);
               sweet_lpf_axis(j->ya, j->yb, j->ua, j->ub, r->y0, ny, W, linear, j->tclamp, j->cu0, j->cu1,
                              tab[nr].ry, tab[nr].wy);
               r->swap1 = 1;
            } else {
               sweet_lpf_axis(j->xa, j->xb, j->ua, j->ub, r->x0, nx, W, linear, j->tclamp, j->cu0, j->cu1,
                              tab[nr].cx, tab[nr].wx);
               sweet_lpf_axis(j->ya, j->yb, j->va, j->vb, r->y0, ny, H, linear, j->tclamp, j->cv0, j->cv1,
                              tab[nr].ry, tab[nr].wy);
            }
            r->scaled = 1;
            r->sbase = sbase;
            r->sstride = (ptrdiff_t)sstride;
            sweet_lpf_stat[SLPF_SCALED]++;
         }
         if (nres) {
            int nx = r->x1 - r->x0, ny = r->y1 - r->y0;
            tab[nr].nx = nx, tab[nr].ny = ny;
            tab[nr].nc = malloc(sizeof(int32_t) * (size_t)(nx + ny));
            if (tab[nr].nc == NULL) {
               for (int q = 0; q <= nr; q++)
                  free(tab[q].cx), free(tab[q].wx), free(tab[q].nc);
               return false;
            }
            for (int k2 = 0; k2 < nx; k2++) {
               double u = (double)j->nu0 + (double)j->nud * (r->x0 + k2 + 0.5);
               long ii = (long)floor(u * NW) % NW;
               tab[nr].nc[k2] = (int32_t)(ii < 0 ? ii + NW : ii);
            }
            for (int k2 = 0; k2 < ny; k2++) {
               double v = (double)j->nv0 + (double)j->nvd * (r->y0 + k2 + 0.5);
               long ii = (long)floor(v * NH) % NH;
               tab[nr].nc[nx + k2] = (int32_t)(ii < 0 ? ii + NH : ii);
            }
            r->noise = 1;
            r->nbase = nbase;
            r->nstride = (ptrdiff_t)nstride;
            r->nk = p->nscale / 255.0f;
            for (int ch = 0; ch < 4; ch++)
               r->npos[ch] = (uint8_t)npos[ch];
            for (int px = 0; px < 4; px++) {
               for (int ch = 0; ch < 4; ch++)
                  r->nperm[4 * px + dpos[ch]] = (uint8_t)(4 * px + npos[ch]);
            }
         }
         for (int px = 0; px < 4; px++) {
            for (int ch = 0; ch < 4; ch++) {
               r->perm[4 * px + dpos[ch]] = (uint8_t)(4 * px + spos[ch]);
               r->orm[4 * px + dpos[ch]] = ch == 3 && (sx || dx || p->kind == SWEET_LPF_OPAQUE) ? 0xff : 0;
            }
         }
         r->f16 = sf16;
         r->opaque = sx || p->kind == SWEET_LPF_OPAQUE;
         for (int ch = 0; ch < 4; ch++)
            r->spos[ch] = (uint8_t)spos[ch];
      }
      nr++;
   }
   if (!lp_setup_update_state(setup, true)) {
      for (int k = 0; k < nr; k++)
         free(tab[k].cx), free(tab[k].wx), free(tab[k].nc);
      return false;
   }
   for (int k = 0; k < nr; k++) {
      /* texture reference, a scaled rectangle's tables and the commands, all in one scene */
      struct sweet_fast_rect *made = NULL;
#define SWEET_LPF_PUT() ((!res || lp_scene_add_resource_reference(setup->scene, res, false, false)) && \
                         (!nres || lp_scene_add_resource_reference(setup->scene, nres, false, false)) && \
                         ((!tab[k].cx && !tab[k].nc) || \
                          sweet_lpf_tables(setup, &proto[k], tab[k].cx, tab[k].wx, tab[k].nc, tab[k].nx, tab[k].ny)) && \
                         sweet_lpf_bin(setup, &proto[k], &made))
      if (!SWEET_LPF_PUT()) {
         /* out of scene memory half way: drop what was binned, new scene, again */
         if (made)
            made->disable = 1;
         made = NULL;
         sweet_lpf_stat[SLPF_OOM]++;
         if (!lp_setup_flush_and_restart(setup))
            break;
         if (!SWEET_LPF_PUT()) {
            if (made)
               made->disable = 1;
            continue;
         }
      }
#undef SWEET_LPF_PUT
      sweet_lpf_stat[SLPF_RECTS]++;
      sweet_lpf_stat[SLPF_PIXELS] += (uint64_t)(proto[k].x1 - proto[k].x0) * (uint64_t)(proto[k].y1 - proto[k].y0);
   }
   for (int k = 0; k < nr; k++)
      free(tab[k].cx), free(tab[k].wx), free(tab[k].nc);
   sweet_lpf_stat[SLPF_DRAWS]++;
   return true;
}
'''
    patch('lp_setup.c', [
        ('''/**
 * Called by vbuf code when we're about to draw something.''', SETUP_FN.lstrip('\n') + '''

/**
 * Called by vbuf code when we're about to draw something.'''),
    ])

    # ---- lp_draw_arrays.c: the hook
    patch('lp_draw_arrays.c', [
        ('''   if (lp->fs) {
      lp->fs->sweet_kind = sweet_lp_draw_kind & 0xff;''', '''   /* sweetgl (lp-fast.py): drawn by the rasterizer threads directly? */
   if (sweet_lpf_pending.n > 0) {
      bool taken = sweet_lp_fast_draw(lp);
      sweet_lpf_pending.n = 0;
      if (taken)
         return;
   }

   if (lp->fs) {
      lp->fs->sweet_kind = sweet_lp_draw_kind & 0xff;'''),
        ('''extern unsigned sweet_lp_draw_kind;''', '''extern unsigned sweet_lp_draw_kind;
#include "lp_sweet_fast.h"
bool sweet_lp_fast_draw(struct llvmpipe_context *lp);   /* lp_setup.c */'''),
    ])

    # ---- lp_flush.c: the counts, in the 10 s report
    patch('lp_flush.c', [
        ('''   ps[3] = swp_nw_stock;
   lg("%s", l);''', '''   ps[3] = swp_nw_stock;
   lg("%s", l);
   {
      extern uint64_t sweet_lpf_stat[];
      static uint64_t pl[13];
      uint64_t d[13];
      for (int i = 0; i < 13; i++) {
         d[i] = sweet_lpf_stat[i] - pl[i];
         pl[i] = sweet_lpf_stat[i];
      }
      lg("sweetgl: lp fast: %llu draws, %llu rects (%llu scaled), %llu kpx; not taken: kind %llu fb %llu src %llu scale %llu "
         "offset %llu bounds %llu turned %llu clamp %llu; out of scene memory %llu",
         (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[12], (unsigned long long)(d[2] / 1000),
         (unsigned long long)d[11], (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5],
         (unsigned long long)d[6], (unsigned long long)d[7], (unsigned long long)d[8], (unsigned long long)d[9],
         (unsigned long long)d[10]);
   }'''),
    ])
