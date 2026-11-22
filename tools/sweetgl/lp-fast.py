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
   /* TINT / ATINT / TINTN: the texel (ATINT: its alpha in every channel, by perm) times the
    * colour: tcol in destination order for the 8-bit row, tcolf RGBA for the float row */
   int tint;
   uint8_t tcol[16];
   float tcolf[4];
   /* after the float row (RGBA8, already tinted / dithered): order into the destination only */
   uint8_t perm2[16], orm2[16];
   /* blends 2..6 read the destination alpha: 1 for formats without one */
   int dx;
   /* 1: A8 sources, one byte a texel (alpha, rgb 0); 2: L8A8, two (rgb L, alpha A); expanded to RGBA
    * first. The value is also the bytes a texel. */
   int a8;
   /* framebuffer / multi-tap kinds, drawn through float (lp-fast.py FX):
    * 12 lumc, 13 cm4, 14 cm3, 9 atintdk, 10 copydkn, 19 blur */
   int fx;
   uint8_t dpos[4];           /* byte of R, G, B, A in the destination */
   uint8_t permr[16], ormr[16];  /* 8-bit source bytes into RGBA order */
   float fp[16];              /* lumc: farg[0..3], falpha; cm: rows m0..m3 (4 floats each, rgb + 0) */
   float col[4];              /* the colour, RGBA 0..1 (atintdk) */
   int ntap, bdiv;
   float tw[16];
   const int32_t *bcx[16], *bry[16];   /* blur: per tap, the texel pairs per column / row */
   const uint8_t *bwx[16], *bwy[16];     /* MASK / MIXBD: [0] are texture1's pairs */
   /* MASK / MIXBD: texture1 (1 RGBA8 in mpos order, 2 A8, 3 half-float RGBA) */
   const uint8_t *mbase;
   ptrdiff_t mstride;
   int mfmt;
   uint8_t mpos[4];
   /* SUPER / CIRCLE / MASKSUP: CA's shape, d = term(column) + term(row) (circle: sqrt of it);
    * terms for framebuffer columns shx0.. and rows shy0.. (even, the 2x2 quads' pixels included) */
   int shk;                   /* 15 super, 16 circle */
   /* CMB / VCM: color_matrix[0..3] (input r, g, b, a) then [4] (bias), RGBA each */
   float cmv[20];
   /* TRI: one triangle over x0..y1 (its box, clipped), sampled per pixel (lp-fast.py TRI) */
   int tri, tkind;
   int pk;                    /* the draw's SWEET_LPF_ kind (the profile's routes) */
   int64_t ea[3], eb[3], ec[3];  /* pixel (X, Y) covered when ea * X + eb * Y + ec >= 0 for all three (1/256 units) */
   float pl[4][3];            /* u, v, u2, v2 at pixel centre (x0, y0), per pixel along x, along y */
   struct sweet_tsrc {
      const uint8_t *base[2]; /* the two mip levels used (the same when one) */
      ptrdiff_t stride[2];
      int w[2], h[2];
      float frac;             /* weight of level 1 */
      int fmt;                /* 1 RGBA8 (pos), 2 A8, 3 half-float RGBA, 4 L8A8 */
      uint8_t pos[4];
      int xa, linear;         /* xa: alpha 1 (RGBX) */
   } ts[2];                   /* texture0, texture1 */
   int tclamp;
   float tcl[4];              /* texcoord0 clamp: u0, v0, u1, v1 */
   const float *shA, *shB;
   int shx0, shy0;
};'''),
    ])

    # ---- lp_rast.c: tables, the command
    RAST_CMD = r'''

/* sweetgl (lp-fast.py): LP_RAST_OP_SWEET_FAST */
#include "util/half_float.h"
#include "lp_sweet_fast.h"
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

static inline uint8_t
sweet_div255(unsigned t)
{
   t += 128;
   return (uint8_t)((t + (t >> 8)) >> 8);
}

#if defined(__aarch64__)
/* a * b / 255, rounded, per byte */
static inline uint8x16_t
sweet_mul255q(uint8x16_t a, uint8x16_t b)
{
   uint16x8_t lo = vmull_u8(vget_low_u8(a), vget_low_u8(b));
   uint16x8_t hi = vmull_high_u8(a, b);
   return vcombine_u8(vrshrn_n_u16(vrsraq_n_u16(lo, lo, 8), 8), vrshrn_n_u16(vrsraq_n_u16(hi, hi, 8), 8));
}
#endif

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
               const int32_t *ncx, int post)
{
   int i = 0;
   const uint8_t *pt = post ? r->perm2 : r->perm, *ot = post ? r->orm2 : r->orm;
   const int tint = post ? 0 : r->tint;
#if defined(__aarch64__)
   static const uint8_t aidx_b[16] = {3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15};
   const uint8x16_t aidx = vld1q_u8(aidx_b);
   const uint8x16_t perm = vld1q_u8(pt), orm = vld1q_u8(ot);
   const uint8x16_t fill = vreinterpretq_u8_u32(vdupq_n_u32(r->color));
   const uint8x16_t amask = vreinterpretq_u8_u32(vdupq_n_u32(0xff000000u));
   const uint8x16_t dxa = r->dx ? amask : vdupq_n_u8(0);
   const uint8x16_t nperm = vld1q_u8(r->nperm);
   const uint8x16_t tcol = vld1q_u8(r->tcol);
   const float32x4_t half = vdupq_n_f32(127.5f), zero = vdupq_n_f32(0.0f);
   for (; i + 4 <= n; i += 4) {
      uint8x16_t sv = s ? vorrq_u8(vqtbl1q_u8(vld1q_u8(s + 4 * i), perm), orm) : fill;
      if (tint) {
         uint16x8_t lo = vmull_u8(vget_low_u8(sv), vget_low_u8(tcol));
         uint16x8_t hi = vmull_high_u8(sv, tcol);
         sv = vcombine_u8(vrshrn_n_u16(vrsraq_n_u16(lo, lo, 8), 8), vrshrn_n_u16(vrsraq_n_u16(hi, hi, 8), 8));
      }
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
         switch (r->blend) {
         case 1:  /* premultiplied over */
            sv = vqaddq_u8(sv, sweet_mul255q(dv, ia));
            break;
         case 2:  /* straight: s * sa + d * (1 - sa) */
            sv = vqaddq_u8(sweet_mul255q(sv, vqtbl1q_u8(sv, aidx)), sweet_mul255q(dv, ia));
            break;
         case 3: {  /* mixed: rgb straight, alpha premultiplied */
            uint8x16_t st = vqaddq_u8(sweet_mul255q(sv, vqtbl1q_u8(sv, aidx)), sweet_mul255q(dv, ia));
            uint8x16_t pm = vqaddq_u8(sv, sweet_mul255q(dv, ia));
            sv = vbslq_u8(amask, pm, st);
            break;
         }
         case 4: {  /* atop: s * da + d * (1 - sa) */
            uint8x16_t da = vorrq_u8(vqtbl1q_u8(dv, aidx), dxa);
            sv = vqaddq_u8(sweet_mul255q(sv, da), sweet_mul255q(dv, ia));
            break;
         }
         case 5:  /* destination out */
            sv = sweet_mul255q(dv, ia);
            break;
         default: /* add */
            sv = vqaddq_u8(sv, dv);
            break;
         }
      }
      vst1q_u8(d + 4 * i, sv);
   }
#endif
   for (; i < n; i++) {
      uint8_t px[4], *dp = d + 4 * i;
      for (int c = 0; c < 4; c++) {
         px[c] = s ? (uint8_t)((pt[c] < 4 ? s[4 * i + pt[c]] : 0) | ot[c])
                   : (uint8_t)(r->color >> (8 * c));
      }
      if (tint) {
         for (int c = 0; c < 4; c++)
            px[c] = sweet_div255((unsigned)px[c] * r->tcol[c]);
      }
      if (nrow) {
         uint8_t nz[4];
         for (int c = 0; c < 4; c++)
            nz[c] = nrow[4 * ncx[i] + (r->nperm[c] & 3)];
         sweet_dither_px(px, nz, r->nk);
      }
      if (r->blend) {
         unsigned sa = px[3], ia = 255u - sa, da = r->dx ? 255u : dp[3];
         for (int c = 0; c < 4; c++) {
            unsigned v;
            switch (r->blend) {
            case 1: v = px[c] + sweet_div255(dp[c] * ia); break;
            case 2: v = sweet_div255(px[c] * sa) + sweet_div255(dp[c] * ia); break;
            case 3: v = c == 3 ? px[c] + sweet_div255(dp[c] * ia) : sweet_div255(px[c] * sa) + sweet_div255(dp[c] * ia); break;
            case 4: v = sweet_div255(px[c] * da) + sweet_div255(dp[c] * ia); break;
            case 5: v = sweet_div255(dp[c] * ia); break;
            default: v = px[c] + dp[c]; break;
            }
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
              const int32_t *ncx, uint8_t *out, int raw)
{
   const int tint = raw ? 0 : r->tint;
   if (raw)
      nrow = NULL;
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
      if (tint) {
         if (tint == 2)
            tv = vdupq_laneq_f32(tv, 3);
         tv = vmulq_f32(tv, vld1q_f32(r->tcolf));
      }
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
      if (tint) {
         float a = t[3];
         for (int ch = 0; ch < 4; ch++)
            t[ch] = (tint == 2 ? a : t[ch]) * r->tcolf[ch];
      }
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

/* A8 texels of one destination row as RGBA8 (0, 0, 0, a): 1:1 from s1, or bilinear / nearest */
static void
sweet_a8_row(const struct sweet_fast_rect *r, const uint8_t *s1, int k0, int n, int ky, uint8_t *out)
{
   if (!r->scaled) {
      if (r->a8 == 2) {
         for (int i = 0; i < n; i++) {
            out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = s1[2 * i];
            out[4 * i + 3] = s1[2 * i + 1];
         }
         return;
      }
      for (int i = 0; i < n; i++) {
         out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 0;
         out[4 * i + 3] = s1[i];
      }
      return;
   }
   const uint8_t *row0 = r->sbase + (ptrdiff_t)r->ry[ky] * r->sstride, *row1 = row0 + r->sstride;
   const unsigned v = r->wy[ky];
   if (r->a8 == 2) {
      for (int i = 0; i < n; i++) {
         const int c = 2 * r->cx[k0 + i];
         const unsigned w = r->wx[k0 + i];
         for (int ch = 0; ch < 2; ch++) {
            unsigned ta = row0[c + ch] * (128 - w) + row0[c + 2 + ch] * w, tb = row1[c + ch] * (128 - w) + row1[c + 2 + ch] * w;
            uint8_t q = (uint8_t)((ta * (128 - v) + tb * v + 8192) >> 14);
            if (ch == 0)
               out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = q;
            else
               out[4 * i + 3] = q;
         }
      }
      return;
   }
   for (int i = 0; i < n; i++) {
      const int c = r->cx[k0 + i];
      const unsigned w = r->wx[k0 + i];
      unsigned ta = row0[c] * (128 - w) + row0[c + 1] * w, tb = row1[c] * (128 - w) + row1[c + 1] * w;
      out[4 * i] = out[4 * i + 1] = out[4 * i + 2] = 0;
      out[4 * i + 3] = (uint8_t)((ta * (128 - v) + tb * v + 8192) >> 14);
   }
}

/* the texture's texels of one destination row as RGBA8 (no tint, no dither): any source */
static void
sweet_tex_row(const struct sweet_fast_rect *r, const uint8_t *s1, int k0, int n, int ky, uint8_t *out)
{
   if (r->a8) {
      sweet_a8_row(r, s1, k0, n, ky, out);
      return;
   }
   if (r->f16 || r->swap1) {
      sweet_f16_row(r, s1, k0, n, ky, NULL, NULL, out, 1);
      return;
   }
   uint8_t tmp[TILE_SIZE * 4] __attribute__((aligned(16)));
   const uint8_t *src = s1;
   if (r->scaled) {
      sweet_resample_row(r, k0, n, ky, tmp);
      src = tmp;
   }
   for (int i = 0; i < n; i++) {
      for (int c = 0; c < 4; c++)
         out[4 * i + c] = (uint8_t)(src[4 * i + (r->permr[c] & 3)] | r->ormr[c]);
   }
}

/* texture1's texel i of a row, RGBA floats */
static inline void
sweet_texel_m(const struct sweet_fast_rect *r, const uint8_t *row, int i, float t[4])
{
   if (r->mfmt == 3) {
      const uint16_t *h = (const uint16_t *)(const void *)(row + 8 * i);
      for (int ch = 0; ch < 4; ch++)
         t[ch] = _mesa_half_to_float(h[ch]);
   } else if (r->mfmt == 2) {
      t[0] = t[1] = t[2] = 0.0f;
      t[3] = row[i] * (1.0f / 255);
   } else if (r->mfmt == 4) {
      t[0] = t[1] = t[2] = row[2 * i] * (1.0f / 255);
      t[3] = row[2 * i + 1] * (1.0f / 255);
   } else {
      for (int ch = 0; ch < 4; ch++)
         t[ch] = row[4 * i + r->mpos[ch]] * (1.0f / 255);
   }
}

/* texture1 at column table k, row table ky (pairs in bcx[0] / bry[0]), RGBA floats */
static inline void
sweet_tap_m(const struct sweet_fast_rect *r, int k, int ky, float o[4])
{
   const uint8_t *row0 = r->mbase + (ptrdiff_t)r->bry[0][ky] * r->mstride, *row1 = row0 + r->mstride;
   const int c = r->bcx[0][k];
   const float w = r->bwx[0][k] * (1.0f / 128), v = r->bwy[0][ky] * (1.0f / 128);
   float a[4], b[4], e[4], f[4];
   sweet_texel_m(r, row0, c, a), sweet_texel_m(r, row0, c + 1, b);
   sweet_texel_m(r, row1, c, e), sweet_texel_m(r, row1, c + 1, f);
   for (int ch = 0; ch < 4; ch++) {
      float h0 = a[ch] + (b[ch] - a[ch]) * w, h1 = e[ch] + (f[ch] - e[ch]) * w;
      o[ch] = h0 + (h1 - h0) * v;
   }
}

static inline float
sweet_circle_d(float sum)
{
   float d = sum > 1e-4f ? sum : 1e-4f;
   return d * (1.0f / sqrtf(d));
}

/*
 * CA's supercircleImage / circleImage at framebuffer pixel X, Y:
 * l = clamp((w * .5 + (1 - d)) / w, 0, 1), w = max(fwidth(d), 1e-4), with
 * fwidth as llvmpipe's: |d(right) - d(left)| in the pixel's row of its 2x2
 * quad plus |d(bottom) - d(top)| in its column.
 */
static inline float
sweet_shape(const struct sweet_fast_rect *r, int X, int Y)
{
   const float *A = r->shA - r->shx0, *B = r->shB - r->shy0;
   const int xl = X & ~1, xr = X | 1, yt = Y & ~1, yb = Y | 1;
   float d, w;
   if (r->shk == 16) {
      d = sweet_circle_d(A[X] + B[Y]);
      w = fabsf(sweet_circle_d(A[xr] + B[Y]) - sweet_circle_d(A[xl] + B[Y])) +
          fabsf(sweet_circle_d(A[X] + B[yb]) - sweet_circle_d(A[X] + B[yt]));
   } else {
      d = A[X] + B[Y];
      w = fabsf(A[xr] - A[xl]) + fabsf(B[yb] - B[yt]);
   }
   w = w > 1e-4f ? w : 1e-4f;
   float l = (w * 0.5f + (1.0f - d)) / w;
   return l < 0.0f ? 0.0f : l > 1.0f ? 1.0f : l;
}

/* one tap: the bilinear sample at column table k, row table ky, RGBA floats */
static inline void
sweet_tap(const struct sweet_fast_rect *r, int t, int k, int ky, float o[4])
{
   const uint8_t *row0 = r->sbase + (ptrdiff_t)r->bry[t][ky] * r->sstride, *row1 = row0 + r->sstride;
   const int c = r->bcx[t][k];
   const float w = r->bwx[t][k] * (1.0f / 128), v = r->bwy[t][ky] * (1.0f / 128);
   float a[4], b[4], e[4], f[4];
   sweet_texel_f(r, row0, c, a), sweet_texel_f(r, row0, c + 1, b);
   sweet_texel_f(r, row1, c, e), sweet_texel_f(r, row1, c + 1, f);
   for (int ch = 0; ch < 4; ch++) {
      float h0 = a[ch] + (b[ch] - a[ch]) * w, h1 = e[ch] + (f[ch] - e[ch]) * w;
      o[ch] = h0 + (h1 - h0) * v;
   }
}

/*
 * Framebuffer and multi-tap kinds for one destination row into out (RGBA8),
 * in float like CA's shaders; the 8-bit row then orders and blends. d is
 * the destination row before the draw (read here, written later).
 */
static void
sweet_fx_row(const struct sweet_fast_rect *r, const uint8_t *d, const uint8_t *s1, int k0, int n, int ky,
             const uint8_t *nrow, const int32_t *ncx, uint8_t *out)
{
   uint8_t tex[TILE_SIZE * 4] __attribute__((aligned(16)));
   const int textured = r->fx == 9 || r->fx == 10 || r->fx == 11 || r->fx == 17 || r->fx == 18 || r->fx == 22 ||
                        r->fx == 23;
   if (textured)
      sweet_tex_row(r, s1, k0, n, ky, tex);
   for (int i = 0; i < n; i++) {
      float D[4], S[4], R[4];
      for (int ch = 0; ch < 4; ch++)
         D[ch] = d[4 * i + r->dpos[ch]] * (1.0f / 255);
      if (r->dx)
         D[3] = 1.0f;
      switch (r->fx) {
      case 20:
         /* color * d0 */
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * D[ch];
         break;
      case 21: {
         /* colorMatrix4x4Bias(d0): unpremultiply, matrix + bias, premultiply (clamped only when stored) */
         float u[4], m = D[3] > 0.005f ? D[3] : 0.005f;
         u[0] = D[0] / m, u[1] = D[1] / m, u[2] = D[2] / m, u[3] = D[3];
         for (int ch = 0; ch < 4; ch++)
            R[ch] = u[0] * r->cmv[ch] + u[1] * r->cmv[4 + ch] + u[2] * r->cmv[8 + ch] + u[3] * r->cmv[12 + ch] +
                    r->cmv[16 + ch];
         for (int ch = 0; ch < 3; ch++)
            R[ch] *= R[3];
         break;
      }
      case 22: {
         /* sgl_vcm(s0): u = clamp(M * unpremultiplied d0 + bias, 0, 1), premultiplied; mix(d0, d0.a > 0 ? u : 0, s0.a) */
         float u[4], v[4], m = D[3] > 0.005f ? D[3] : 0.005f;
         u[0] = D[0] / m, u[1] = D[1] / m, u[2] = D[2] / m, u[3] = D[3];
         for (int ch = 0; ch < 4; ch++) {
            float c = u[0] * r->cmv[ch] + u[1] * r->cmv[4 + ch] + u[2] * r->cmv[8 + ch] + u[3] * r->cmv[12 + ch] +
                      r->cmv[16 + ch];
            v[ch] = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
         }
         for (int ch = 0; ch < 3; ch++)
            v[ch] *= v[3];
         const float a = tex[4 * i + 3] * (1.0f / 255);
         for (int ch = 0; ch < 4; ch++)
            R[ch] = D[ch] * (1.0f - a) + (D[3] > 0.0f ? v[ch] : 0.0f) * a;
         break;
      }
      case 23: {
         /* color * vec4(t0.rgb, 1) * s1.a */
         float m[4];
         sweet_tap_m(r, k0 + i, ky, m);
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * (ch == 3 ? 1.0f : tex[4 * i + ch] * (1.0f / 255)) * m[3];
         break;
      }
      case 11: {
         /* color * s0 * s1.a */
         float m[4];
         sweet_tap_m(r, k0 + i, ky, m);
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * (tex[4 * i + ch] * (1.0f / 255)) * m[3];
         break;
      }
      case 15:
      case 16:
      case 17: {
         /* color * vec4(l) (* s0) */
         const float l = sweet_shape(r, r->x0 + k0 + i, r->y0 + ky);
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * (r->fx == 17 ? tex[4 * i + ch] * (1.0f / 255) : 1.0f) * l;
         break;
      }
      case 18: {
         /* mix(s1, d0, s0.a) */
         float m[4];
         const float a = tex[4 * i + 3] * (1.0f / 255);
         sweet_tap_m(r, k0 + i, ky, m);
         for (int ch = 0; ch < 4; ch++)
            R[ch] = m[ch] * (1.0f - a) + D[ch] * a;
         break;
      }
      case 13:
      case 14:
         /* vec4(D.r * m0 + D.g * m1 + D.b * m2 (+ D.a * m3), D.a) */
         for (int ch = 0; ch < 3; ch++)
            R[ch] = D[0] * r->fp[ch] + D[1] * r->fp[4 + ch] + D[2] * r->fp[8 + ch] + D[3] * r->fp[12 + ch];
         R[3] = D[3];
         break;
      case 12: {
         /* luminanceCurve(D, p, a): x = curve(luma(D)); vec4(mix(D.rgb, vec3(x), a), D.a) */
         float l = D[0] * 0.2125f + D[1] * 0.7154f + D[2] * 0.0721f;
         float x = ((r->fp[0] * l + r->fp[1]) * l + r->fp[2]) * l + r->fp[3];
         x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
         for (int ch = 0; ch < 3; ch++)
            R[ch] = D[ch] * (1.0f - r->fp[4]) + x * r->fp[4];
         R[3] = D[3];
         break;
      }
      case 9:
      case 10: {
         /* darkenSover(S, D): y = luma(D); S *= y^4; R = D * (1 - S.a) + S */
         for (int ch = 0; ch < 4; ch++)
            S[ch] = r->fx == 9 ? r->col[ch] * (tex[4 * i + 3] * (1.0f / 255)) : tex[4 * i + ch] * (1.0f / 255);
         float y = D[0] * 0.2125f + D[1] * 0.7154f + D[2] * 0.0721f, y4 = (y * y) * (y * y);
         for (int ch = 0; ch < 4; ch++)
            R[ch] = D[ch] * (1.0f - S[3] * y4) + S[ch] * y4;
         break;
      }
      default: {
         /* blur: acc = sum of tap * weight; / max(.001, acc.a) when bdiv */
         float acc[4] = {0, 0, 0, 0}, q[4];
         for (int t = 0; t < r->ntap; t++) {
            sweet_tap(r, t, k0 + i, ky, q);
            for (int ch = 0; ch < 4; ch++)
               acc[ch] = q[ch] * r->tw[t] + acc[ch];
         }
         if (r->bdiv) {
            float m = acc[3] > 0.001f ? acc[3] : 0.001f;
            for (int ch = 0; ch < 4; ch++)
               acc[ch] /= m;
         }
         memcpy(R, acc, sizeof(R));
         break;
      }
      }
      if (nrow) {
         /* CA's dither on the result, in float like the shader */
         const uint8_t *nz = nrow + 4 * ncx[i];
         for (int ch = 0; ch < 3; ch++) {
            float c = R[ch] + ((float)nz[r->npos[ch]] - 127.5f) * r->nk * R[3];
            R[ch] = c < 0.0f ? 0.0f : c > R[3] ? R[3] : c;
         }
      }
      for (int ch = 0; ch < 4; ch++) {
         float c = R[ch] > 0.0f ? (R[ch] < 1.0f ? R[ch] : 1.0f) : 0.0f;
         out[4 * i + ch] = (uint8_t)(c * 255.0f + 0.5f);
      }
   }
}

/* ---- TRI: triangles sampled per pixel (lp-fast.py) */

static inline void
sweet_tsrc_texel(const struct sweet_tsrc *t, int l, int x, int y, float o[4])
{
   const uint8_t *row = t->base[l] + (ptrdiff_t)y * t->stride[l];
   switch (t->fmt) {
   case 2:
      o[0] = o[1] = o[2] = 0.0f;
      o[3] = row[x] * (1.0f / 255);
      break;
   case 3: {
      const uint16_t *h = (const uint16_t *)(const void *)(row + 8 * x);
      for (int ch = 0; ch < 4; ch++)
         o[ch] = _mesa_half_to_float(h[ch]);
      break;
   }
   case 4:
      o[0] = o[1] = o[2] = row[2 * x] * (1.0f / 255);
      o[3] = row[2 * x + 1] * (1.0f / 255);
      break;
   default:
      for (int ch = 0; ch < 4; ch++)
         o[ch] = row[4 * x + t->pos[ch]] * (1.0f / 255);
      break;
   }
   if (t->xa)
      o[3] = 1.0f;
}

/* GL's sample at (u, v) of level l: clamp to edge, linear or nearest */
static inline void
sweet_tsrc_level(const struct sweet_tsrc *t, int l, float u, float v, float o[4])
{
   const int w = t->w[l], h = t->h[l];
   if (!t->linear) {
      int i = (int)floorf(u * w), j = (int)floorf(v * h);
      i = i < 0 ? 0 : i >= w ? w - 1 : i;
      j = j < 0 ? 0 : j >= h ? h - 1 : j;
      sweet_tsrc_texel(t, l, i, j, o);
      return;
   }
   float x = u * w - 0.5f, y = v * h - 0.5f, fx0 = floorf(x), fy0 = floorf(y);
   float a = x - fx0, b = y - fy0;
   int i0 = (int)fx0, j0 = (int)fy0, i1 = i0 + 1, j1 = j0 + 1;
   i0 = i0 < 0 ? 0 : i0 >= w ? w - 1 : i0, i1 = i1 < 0 ? 0 : i1 >= w ? w - 1 : i1;
   j0 = j0 < 0 ? 0 : j0 >= h ? h - 1 : j0, j1 = j1 < 0 ? 0 : j1 >= h ? h - 1 : j1;
   float p[4], q[4], r2[4], s2[4];
   sweet_tsrc_texel(t, l, i0, j0, p), sweet_tsrc_texel(t, l, i1, j0, q);
   sweet_tsrc_texel(t, l, i0, j1, r2), sweet_tsrc_texel(t, l, i1, j1, s2);
   for (int ch = 0; ch < 4; ch++) {
      float t0 = p[ch] + (q[ch] - p[ch]) * a, t1 = r2[ch] + (s2[ch] - r2[ch]) * a;
      o[ch] = t0 + (t1 - t0) * b;
   }
}

static inline void
sweet_tsrc_sample(const struct sweet_tsrc *t, float u, float v, float o[4])
{
   sweet_tsrc_level(t, 0, u, v, o);
   if (t->frac > 0.0f) {
      float o1[4];
      sweet_tsrc_level(t, 1, u, v, o1);
      for (int ch = 0; ch < 4; ch++)
         o[ch] += (o1[ch] - o[ch]) * t->frac;
   }
}

static inline float
sweet_shape_term(int k, float c)
{
   if (k == 16)
      return c * c;
   float a = fabsf(c) * 1.199f - 0.199f;
   return powf(a > 0.0f ? a : 0.0f, 2.68f);
}

static inline float
sweet_shape_dd(int k, float sum)
{
   return k == 16 ? sweet_circle_d(sum) : sum;
}

/*
 * One row of a triangle: the covered pixels from X = xs to xe - 1 (row Y),
 * RGBA8 into out. D: the destination row (pixel xs first).
 */
static void
sweet_tri_row(const struct sweet_fast_rect *r, const uint8_t *drow, int xs, int xe, int Y, uint8_t *out)
{
   const int k = r->tkind, n = xe - xs;
   const float fy = (float)(Y - r->y0);
   /* CA's shape (SUPER, CIRCLE on texcoord0; MASKSUP on texcoord1): d per pixel of this row and of the
    * pixel's quad partner row, from the quad's left column to its right one */
   float d0[TILE_SIZE + 2], d1[TILE_SIZE + 2];
   const int shp = k == SWEET_LPF_SUPER || k == SWEET_LPF_CIRCLE || k == SWEET_LPF_MASKSUP;
   const int sx0 = xs & ~1, sx1 = (xe - 1) | 1;
   if (shp) {
      const int sk = k == SWEET_LPF_CIRCLE ? 16 : 15, cu = k == SWEET_LPF_MASKSUP ? 2 : 0;
      const float *pu = r->pl[cu], *pv = r->pl[cu + 1];
      const int Yp = Y ^ 1;
      const float fyp = (float)(Yp - r->y0);
      /* along x: a term changes only if its coordinate does */
      const int ux = pu[1] != 0.0f, vx = pv[1] != 0.0f;
      float tu0 = 0, tv0 = 0, tu1 = 0, tv1 = 0;
      if (!ux)
         tu0 = sweet_shape_term(sk, pu[0] + pu[2] * fy), tu1 = sweet_shape_term(sk, pu[0] + pu[2] * fyp);
      if (!vx)
         tv0 = sweet_shape_term(sk, pv[0] + pv[2] * fy), tv1 = sweet_shape_term(sk, pv[0] + pv[2] * fyp);
      for (int X = sx0; X <= sx1; X++) {
         const float fx = (float)(X - r->x0);
         float a0 = tu0, b0 = tv0, a1 = tu1, b1 = tv1;
         if (ux) {
            a0 = sweet_shape_term(sk, pu[0] + pu[1] * fx + pu[2] * fy);
            a1 = pu[2] != 0.0f ? sweet_shape_term(sk, pu[0] + pu[1] * fx + pu[2] * fyp) : a0;
         }
         if (vx) {
            b0 = sweet_shape_term(sk, pv[0] + pv[1] * fx + pv[2] * fy);
            b1 = pv[2] != 0.0f ? sweet_shape_term(sk, pv[0] + pv[1] * fx + pv[2] * fyp) : b0;
         }
         d0[X - sx0] = sweet_shape_dd(sk, a0 + b0);
         d1[X - sx0] = sweet_shape_dd(sk, a1 + b1);
      }
   }
   for (int i = 0; i < n; i++) {
      const int X = xs + i;
      const float fx = (float)(X - r->x0);
      float D[4], t[4] = {0, 0, 0, 1}, m[4] = {0, 0, 0, 1}, R[4], l = 1.0f;
      for (int ch = 0; ch < 4; ch++)
         D[ch] = drow[4 * i + r->dpos[ch]] * (1.0f / 255);
      if (r->dx)
         D[3] = 1.0f;
      if (r->ts[0].base[0]) {
         float u = r->pl[0][0] + r->pl[0][1] * fx + r->pl[0][2] * fy;
         float v = r->pl[1][0] + r->pl[1][1] * fx + r->pl[1][2] * fy;
         if (r->tclamp) {
            u = u < r->tcl[0] ? r->tcl[0] : u > r->tcl[2] ? r->tcl[2] : u;
            v = v < r->tcl[1] ? r->tcl[1] : v > r->tcl[3] ? r->tcl[3] : v;
         }
         sweet_tsrc_sample(&r->ts[0], u, v, t);
      }
      if (r->ts[1].base[0]) {
         float u = r->pl[2][0] + r->pl[2][1] * fx + r->pl[2][2] * fy;
         float v = r->pl[3][0] + r->pl[3][1] * fx + r->pl[3][2] * fy;
         sweet_tsrc_sample(&r->ts[1], u, v, m);
      }
      if (shp) {
         const int j = X - sx0, jl = (X & ~1) - sx0, jr = (X | 1) - sx0;
         float d = d0[j], w = fabsf(d0[jr] - d0[jl]) + fabsf(d1[j] - d0[j]);
         w = w > 1e-4f ? w : 1e-4f;
         l = (w * 0.5f + (1.0f - d)) / w;
         l = l < 0.0f ? 0.0f : l > 1.0f ? 1.0f : l;
      }
      switch (k) {
      case SWEET_LPF_FILL:
         memcpy(R, r->col, sizeof(R));
         break;
      case SWEET_LPF_COPY:
         memcpy(R, t, sizeof(R));
         break;
      case SWEET_LPF_OPAQUE:
         R[0] = t[0], R[1] = t[1], R[2] = t[2], R[3] = 1.0f;
         break;
      case SWEET_LPF_ALPHA:
         R[0] = R[1] = R[2] = R[3] = t[3];
         break;
      case SWEET_LPF_TINT:
      case SWEET_LPF_MASK:
      case SWEET_LPF_MASKSUP:
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * t[ch] * (k == SWEET_LPF_MASK ? m[3] : k == SWEET_LPF_MASKSUP ? l : 1.0f);
         break;
      case SWEET_LPF_ATINT:
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * t[3];
         break;
      case SWEET_LPF_OTINT:
      case SWEET_LPF_OMASK:
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * (ch == 3 ? 1.0f : t[ch]) * (k == SWEET_LPF_OMASK ? m[3] : 1.0f);
         break;
      case SWEET_LPF_SUPER:
      case SWEET_LPF_CIRCLE:
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * l;
         break;
      case SWEET_LPF_MIXBD:
         for (int ch = 0; ch < 4; ch++)
            R[ch] = m[ch] * (1.0f - t[3]) + D[ch] * t[3];
         break;
      default: /* CFB */
         for (int ch = 0; ch < 4; ch++)
            R[ch] = r->col[ch] * D[ch];
         break;
      }
      for (int ch = 0; ch < 4; ch++) {
         float c = R[ch] > 0.0f ? (R[ch] < 1.0f ? R[ch] : 1.0f) : 0.0f;
         out[4 * i + ch] = (uint8_t)(c * 255.0f + 0.5f);
      }
   }
}

/* the triangle's pixels in this tile; returns how many */
static uint64_t
sweet_tri_tile(const struct sweet_fast_rect *r, uint8_t *d, unsigned stride, int x0, int y0, int x1, int y1)
{
   uint8_t tmp[TILE_SIZE * 4] __attribute__((aligned(16)));
   uint64_t npx = 0;
   for (int y = y0; y < y1; y++, d += stride) {
      const int64_t Py = (int64_t)y * 256 + 128;
      int xs = -1, xe = -1;
      for (int x = x0; x < x1; x++) {
         const int64_t Px = (int64_t)x * 256 + 128;
         const bool in = r->ea[0] * Px + r->eb[0] * Py + r->ec[0] >= 0 && r->ea[1] * Px + r->eb[1] * Py + r->ec[1] >= 0 &&
                         r->ea[2] * Px + r->eb[2] * Py + r->ec[2] >= 0;
         if (in) {
            if (xs < 0)
               xs = x;
            xe = x + 1;
         } else if (xs >= 0) {
            break;        /* convex: one span a row */
         }
      }
      if (xs < 0)
         continue;
      uint8_t *dr = d + (size_t)(xs - x0) * 4;
      sweet_tri_row(r, dr, xs, xe, y, tmp);
      sweet_fast_row(r, dr, tmp, xe - xs, NULL, NULL, 1);
      npx += (uint64_t)(xe - xs);
   }
   return npx;
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
   if (r->tri) {
      sweet_fk_px = sweet_tri_tile(r, d, stride, x0, y0, x1, y1);
      sweet_fk_key = 5 * 32 + (r->tkind & 31);
      return;
   }
   sweet_fk_px = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
   sweet_fk_key = (r->fx ? 4 * 32 + (r->fx & 31) : (r->a8 ? 3 : r->f16 || r->swap1 ? 2 : r->scaled ? 1 : 0) * 32 + (r->pk & 31));
   const int32_t *ncx = r->noise ? r->ncx + (x0 - r->x0) : NULL;
#define SWEET_NROW(y) (r->noise ? r->nbase + (ptrdiff_t)r->nry[(y) - r->y0] * r->nstride : NULL)
   if (r->fx) {
      /* framebuffer / multi-tap kinds: computed here from the destination, then ordered and blended */
      uint8_t tmp[TILE_SIZE * 4] __attribute__((aligned(16)));
      const int bpp = r->a8 ? r->a8 : r->f16 ? 8 : 4;
      const uint8_t *s1 = r->src ? r->src + (ptrdiff_t)(y0 - r->y0) * r->src_dy + (ptrdiff_t)(x0 - r->x0) * bpp : NULL;
      for (int y = y0; y < y1; y++) {
         sweet_fx_row(r, d, s1, x0 - r->x0, x1 - x0, y - r->y0, SWEET_NROW(y), ncx, tmp);
         sweet_fast_row(r, d, tmp, x1 - x0, NULL, NULL, 1);
         d += stride;
         if (s1)
            s1 += r->src_dy;
      }
      return;
   }
   if (r->a8) {
      /* A8: expanded to RGBA (0, 0, 0, a), then the 8-bit row (order, tint, dither, blend) */
      uint8_t tmp[TILE_SIZE * 4] __attribute__((aligned(16)));
      const uint8_t *s1 = r->src ? r->src + (ptrdiff_t)(y0 - r->y0) * r->src_dy + (ptrdiff_t)(x0 - r->x0) * r->a8 : NULL;
      for (int y = y0; y < y1; y++) {
         sweet_a8_row(r, s1, x0 - r->x0, x1 - x0, y - r->y0, tmp);
         sweet_fast_row(r, d, tmp, x1 - x0, SWEET_NROW(y), ncx, 0);
         d += stride;
         if (s1)
            s1 += r->src_dy;
      }
      return;
   }
   if (r->f16 || r->swap1) {
      /* filtered, dithered and quantized here; the 8-bit row then orders and blends */
      uint8_t tmp[TILE_SIZE * 4] __attribute__((aligned(16)));
      const uint8_t *s1 = r->src ? r->src + (ptrdiff_t)(y0 - r->y0) * r->src_dy + (ptrdiff_t)(x0 - r->x0) * 8 : NULL;
      for (int y = y0; y < y1; y++) {
         sweet_f16_row(r, s1, x0 - r->x0, x1 - x0, y - r->y0, SWEET_NROW(y), ncx, tmp, 0);
         sweet_fast_row(r, d, tmp, x1 - x0, NULL, NULL, 1);
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
         sweet_fast_row(r, d, tmp, x1 - x0, SWEET_NROW(y), ncx, 0);
         d += stride;
      }
      return;
   }
   const uint8_t *s = r->src ? r->src + (ptrdiff_t)(y0 - r->y0) * r->src_dy + (ptrdiff_t)(x0 - r->x0) * 4 : NULL;
   for (int y = y0; y < y1; y++) {
      sweet_fast_row(r, d, s, x1 - x0, SWEET_NROW(y), ncx, 0);
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
       SLPF_SWAP, SLPF_CLAMP, SLPF_OOM, SLPF_KIND, SLPF_SCALED, SLPF_TRIS, SLPF_TRIDRAWS, SLPF_MIPTRI, SLPF_N };
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

/* a blur rectangle's per-tap tables into the current scene's memory; false: out of memory */
static bool
sweet_lpf_btables(struct lp_setup_context *setup, struct sweet_fast_rect *r, const int32_t *bt, const uint8_t *bw,
                  int nt, int nx, int ny)
{
   size_t n = (size_t)(nx + ny);
   if (sizeof(int32_t) * n > DATA_BLOCK_SIZE)
      return false;
   for (int t = 0; t < nt; t++) {
      int32_t *ci = lp_scene_alloc(setup->scene, (unsigned)(sizeof(int32_t) * n));
      uint8_t *cw = ci ? lp_scene_alloc(setup->scene, (unsigned)n) : NULL;
      if (cw == NULL)
         return false;
      memcpy(ci, bt + (size_t)t * n, sizeof(int32_t) * n);
      memcpy(cw, bw + (size_t)t * n, n);
      r->bcx[t] = ci, r->bry[t] = ci + nx;
      r->bwx[t] = cw, r->bwy[t] = cw + nx;
   }
   return true;
}

/* a shape's per-column / per-row terms into the current scene's memory; false: out of memory */
static bool
sweet_lpf_shtables(struct lp_setup_context *setup, struct sweet_fast_rect *r, const float *sh, int nx, int ny)
{
   size_t n = sizeof(float) * (size_t)(nx + ny);
   if (n > DATA_BLOCK_SIZE)
      return false;
   float *t = lp_scene_alloc(setup->scene, (unsigned)n);
   if (t == NULL)
      return false;
   memcpy(t, sh, n);
   r->shA = t, r->shB = t + nx;
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

/* ---- TRI (lp-fast.py): triangles sampled per pixel */

/*
 * A texture for per-pixel sampling: the bound view, its sampler, the mip
 * level(s) llvmpipe would use for the derivatives (du/dx..dv/dy, normalized):
 * rho = max(|du/dx| W, |du/dy| W, |dv/dx| H, |dv/dy| H); rho <= 1 magnifies
 * (level 0); nearest mip: level floor(log2 rho + .5); linear mip: llvmpipe's
 * brilinear split (factor 2). False when not usable.
 */
static bool
sweet_tsrc_make(struct llvmpipe_context *lp, struct pipe_resource *res, const float g[4], struct sweet_tsrc *t)
{
   struct lp_setup_context *setup = lp->setup;
   const struct lp_jit_texture *jt = NULL;
   const struct pipe_sampler_state *samp = NULL;
   int xa = 0;
   memset(t, 0, sizeof(*t));
   if (res == NULL || res->nr_samples > 1 || res->array_size > 1 || res->target != PIPE_TEXTURE_2D ||
       res == lp->framebuffer.cbufs[0].texture)
      return false;
   for (unsigned i = 0; i < setup->fs.current_tex_num && i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++) {
      const struct lp_jit_texture *c = &setup->fs.current.jit_resources.textures[i];
      if (setup->fs.current_tex[i] == res && c->base != NULL && c->first_level == 0) {
         jt = c;
         samp = i < PIPE_MAX_SAMPLERS ? lp->samplers[MESA_SHADER_FRAGMENT][i] : NULL;
         break;
      }
   }
   if (jt == NULL || samp == NULL || samp->unnormalized_coords || samp->wrap_s != PIPE_TEX_WRAP_CLAMP_TO_EDGE ||
       samp->wrap_t != PIPE_TEX_WRAP_CLAMP_TO_EDGE || samp->min_lod > 0.0f || samp->lod_bias != 0.0f)
      return false;
   if (res->format == PIPE_FORMAT_R16G16B16A16_FLOAT || res->format == PIPE_FORMAT_R16G16B16X16_FLOAT)
      t->fmt = 3, xa = res->format == PIPE_FORMAT_R16G16B16X16_FLOAT;
   else if (res->format == PIPE_FORMAT_A8_UNORM)
      t->fmt = 2;
   else if (res->format == PIPE_FORMAT_L8A8_UNORM)
      t->fmt = 4;
   else {
      int pos[4];
      if (!sweet_lpf_order(res->format, pos, &xa))
         return false;
      t->fmt = 1;
      for (int ch = 0; ch < 4; ch++)
         t->pos[ch] = (uint8_t)pos[ch];
   }
   t->xa = xa;
   const int W = (int)res->width0, H = (int)res->height0, last = MIN2((int)res->last_level, (int)jt->last_level);
   double rho = MAX2(MAX2(fabs(g[0]) * W, fabs(g[1]) * W), MAX2(fabs(g[2]) * H, fabs(g[3]) * H));
   int l0 = 0, l1 = 0;
   float frac = 0.0f;
   const bool minify = rho > 1.0;
   t->linear = (minify ? samp->min_img_filter : samp->mag_img_filter) == PIPE_TEX_FILTER_LINEAR;
   if (minify && last > 0 && samp->min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST) {
      l0 = l1 = (int)floor(log2(rho) + 0.5);
   } else if (minify && last > 0 && samp->min_mip_filter == PIPE_TEX_MIPFILTER_LINEAR) {
      const double factor = 2.0, pre = (2 * factor - 0.5) / (M_SQRT2 * factor);
      int e;
      double m = frexp(rho * pre, &e) * 2.0;     /* rho * pre = m * 2^(e - 1), m in [1, 2) */
      l0 = e - 1, l1 = l0 + 1;
      frac = (float)(m * factor + (1 - 2 * factor));
      if (frac <= 0.0f)
         frac = 0.0f, l1 = l0;
      frac = frac > 1.0f ? 1.0f : frac;
   }
   if (l0 < 0)
      l0 = 0, l1 = 0, frac = 0.0f;
   if (l0 >= last)
      l0 = last, l1 = last, frac = 0.0f;
   if (l1 > last)
      l1 = last;
   const int lv[2] = {l0, l1};
   for (int k = 0; k < 2; k++) {
      t->base[k] = (const uint8_t *)jt->base + jt->mip_offsets[lv[k]];
      t->stride[k] = (ptrdiff_t)jt->row_stride[lv[k]];
      t->w[k] = MAX2(W >> lv[k], 1);
      t->h[k] = MAX2(H >> lv[k], 1);
   }
   t->frac = l1 != l0 ? frac : 0.0f;
   return t->stride[0] != 0;
}

/* the pending draw's triangles into sweet_fast_rects, binned; false: drawn normally */
static bool
sweet_lp_tri_draw(struct llvmpipe_context *lp)
{
   struct sweet_lpf *p = &sweet_lpf_pending;
   struct lp_setup_context *setup = lp->setup;
   const struct pipe_framebuffer_state *fb = &lp->framebuffer;
   const int kd = p->kind;
   int dpos[4], dx;
   if (p->ntri <= 0 || p->ntri > SWEET_LPF_TRIS || p->blend < 0 || p->blend > 6)
      return false;
   if (!(kd == SWEET_LPF_FILL || kd == SWEET_LPF_COPY || kd == SWEET_LPF_TINT || kd == SWEET_LPF_ATINT ||
         kd == SWEET_LPF_OPAQUE || kd == SWEET_LPF_ALPHA || kd == SWEET_LPF_MASK || kd == SWEET_LPF_SUPER ||
         kd == SWEET_LPF_CIRCLE || kd == SWEET_LPF_MASKSUP || kd == SWEET_LPF_MIXBD || kd == SWEET_LPF_CFB ||
         kd == SWEET_LPF_OMASK || kd == SWEET_LPF_OTINT))
      return sweet_lpf_no(SLPF_KIND), false;
   if (fb->nr_cbufs != 1 || !fb->cbufs[0].texture || fb->layers > 1 ||
       !sweet_lpf_order(fb->cbufs[0].format, dpos, &dx) ||
       lp->active_occlusion_queries || lp->active_statistics_queries || lp->active_primgen_queries)
      return sweet_lpf_no(SLPF_FB), false;
   const bool tex0 = kd != SWEET_LPF_FILL && kd != SWEET_LPF_SUPER && kd != SWEET_LPF_CIRCLE && kd != SWEET_LPF_CFB;
   const bool tex1 = kd == SWEET_LPF_MASK || kd == SWEET_LPF_MIXBD || kd == SWEET_LPF_OMASK;
   struct pipe_resource *res = tex0 ? p->src : NULL, *res2 = tex1 ? p->src2 : NULL;
   if ((tex0 && res == NULL) || (tex1 && res2 == NULL))
      return sweet_lpf_no(SLPF_SRC), false;
   struct sweet_fast_rect proto[SWEET_LPF_TRIS];
   const int fbw = (int)fb->width, fbh = (int)fb->height;
   int nr = 0;
   for (int t = 0; t < p->ntri; t++) {
      const struct sweet_lpf_tri *q = &p->tri[t];
      struct sweet_fast_rect *r = &proto[nr];
      memset(r, 0, sizeof(*r));
      /* covered pixels: the box of the vertices, clipped */
      double bx0 = MIN3(q->x[0], q->x[1], q->x[2]), bx1 = MAX3(q->x[0], q->x[1], q->x[2]);
      double by0 = MIN3(q->y[0], q->y[1], q->y[2]), by1 = MAX3(q->y[0], q->y[1], q->y[2]);
      r->x0 = MAX3((int)floor(bx0), q->clip[0], 0), r->x1 = MIN3((int)ceil(bx1) + 1, q->clip[2], fbw);
      r->y0 = MAX3((int)floor(by0), q->clip[1], 0), r->y1 = MIN3((int)ceil(by1) + 1, q->clip[3], fbh);
      if (r->x0 >= r->x1 || r->y0 >= r->y1)
         continue;
      /* edges in 1/256 pixel, the inside on the left of each (counter-clockwise order), shared edges
       * given to one of their two triangles (E == 0 kept on edges going down, or right when flat) */
      int64_t X[3], Y[3];
      for (int k = 0; k < 3; k++)
         X[k] = llrint((double)q->x[k] * 256.0), Y[k] = llrint((double)q->y[k] * 256.0);
      int64_t area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
      if (area == 0)
         continue;
      int o[3] = {0, 1, 2};
      if (area < 0)
         o[1] = 2, o[2] = 1;
      for (int e = 0; e < 3; e++) {
         const int i = o[e], j = o[(e + 1) % 3];
         const int64_t ddx = X[j] - X[i], ddy = Y[j] - Y[i];
         /* E(P) = ddx * (Py - Yi) - ddy * (Px - Xi): > 0 inside */
         r->ea[e] = -ddy, r->eb[e] = ddx, r->ec[e] = ddy * X[i] - ddx * Y[i];
         if (!(ddy < 0 || (ddy == 0 && ddx > 0)))
            r->ec[e] -= 1;
      }
      /* planes: a(px, py) at pixel centres, from the unsnapped vertices */
      const double x0c = r->x0 + 0.5, y0c = r->y0 + 0.5;
      const double e1x = (double)q->x[1] - q->x[0], e1y = (double)q->y[1] - q->y[0];
      const double e2x = (double)q->x[2] - q->x[0], e2y = (double)q->y[2] - q->y[0];
      const double det = e1x * e2y - e2x * e1y;
      const float *at[4] = {q->u, q->v, q->u2, q->v2};
      float g0[4], g1[4];
      for (int a = 0; a < 4; a++) {
         const double d1 = (double)at[a][1] - at[a][0], d2 = (double)at[a][2] - at[a][0];
         const double gx = (d1 * e2y - d2 * e1y) / det, gy = (d2 * e1x - d1 * e2x) / det;
         r->pl[a][0] = (float)(at[a][0] + gx * (x0c - q->x[0]) + gy * (y0c - q->y[0]));
         r->pl[a][1] = (float)gx, r->pl[a][2] = (float)gy;
      }
      g0[0] = r->pl[0][1], g0[1] = r->pl[0][2], g0[2] = r->pl[1][1], g0[3] = r->pl[1][2];
      g1[0] = r->pl[2][1], g1[1] = r->pl[2][2], g1[2] = r->pl[3][1], g1[3] = r->pl[3][2];
      if ((tex0 && !sweet_tsrc_make(lp, res, g0, &r->ts[0])) || (tex1 && !sweet_tsrc_make(lp, res2, g1, &r->ts[1]))) {
         sweet_lpf_why("tri texture", p, NULL, tex0 && res ? (int)res->format : -1, tex1 && res2 ? (int)res2->format : -1,
                       0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      r->tri = 1;
      r->tkind = kd;
      r->pk = kd;
      r->blend = p->blend;
      r->dx = dx;
      r->tclamp = p->tclamp;
      memcpy(r->tcl, p->cl, sizeof(r->tcl));
      for (int ch = 0; ch < 4; ch++) {
         r->dpos[ch] = (uint8_t)dpos[ch];
         r->col[ch] = q->color[ch] * (1.0f / 255);
      }
      for (int px = 0; px < 4; px++) {
         for (int ch = 0; ch < 4; ch++) {
            r->perm2[4 * px + dpos[ch]] = (uint8_t)(4 * px + ch);
            r->orm2[4 * px + dpos[ch]] = ch == 3 && dx ? 0xff : 0;
         }
      }
      nr++;
   }
   if (nr == 0)
      return true;      /* nothing covered: done */
   if (!lp_setup_update_state(setup, true))
      return false;
   for (int k = 0; k < nr; k++) {
      struct sweet_fast_rect *made = NULL;
#define SWEET_TRI_PUT() ((!res || lp_scene_add_resource_reference(setup->scene, res, false, false)) && \
                         (!res2 || lp_scene_add_resource_reference(setup->scene, res2, false, false)) && \
                         sweet_lpf_bin(setup, &proto[k], &made))
      if (!SWEET_TRI_PUT()) {
         if (made)
            made->disable = 1;
         made = NULL;
         sweet_lpf_stat[SLPF_OOM]++;
         if (!lp_setup_flush_and_restart(setup))
            break;
         if (!SWEET_TRI_PUT()) {
            if (made)
               made->disable = 1;
            continue;
         }
      }
#undef SWEET_TRI_PUT
      sweet_lpf_stat[SLPF_TRIS]++;
      sweet_lpf_stat[SLPF_PIXELS] += (uint64_t)(proto[k].x1 - proto[k].x0) * (uint64_t)(proto[k].y1 - proto[k].y0) / 2;
   }
   sweet_lpf_stat[SLPF_DRAWS]++;
   sweet_lpf_stat[SLPF_TRIDRAWS]++;
   return true;
}

/* rectangle jobs that need mipmaps: the same draw as two triangles each */
static bool
sweet_lp_rects_as_tris(struct llvmpipe_context *lp)
{
   struct sweet_lpf *p = &sweet_lpf_pending;
   const int kd = p->kind;
   if (p->n <= 0 || 2 * p->n > SWEET_LPF_TRIS ||
       !(kd == SWEET_LPF_COPY || kd == SWEET_LPF_TINT || kd == SWEET_LPF_ATINT || kd == SWEET_LPF_OPAQUE ||
         kd == SWEET_LPF_ALPHA || kd == SWEET_LPF_MASK || kd == SWEET_LPF_MASKSUP || kd == SWEET_LPF_MIXBD ||
         kd == SWEET_LPF_OMASK || kd == SWEET_LPF_OTINT))
      return false;
   int nt = 0;
   for (int k = 0; k < p->n; k++) {
      const struct sweet_lpf_job *j = &p->job[k];
      if (j->x0 >= j->x1 || j->y0 >= j->y1)
         continue;
      /* corners: (xa, ya), (xb, ya), (xb, yb), (xa, yb) */
      const float cx[4] = {j->xa, j->xb, j->xb, j->xa}, cy[4] = {j->ya, j->ya, j->yb, j->yb};
      float u[4], v[4], u2[4], v2[4];
      for (int c = 0; c < 4; c++) {
         const int hx = c == 1 || c == 2, hy = c >= 2;
         u[c] = j->swap ? (hy ? j->ub : j->ua) : (hx ? j->ub : j->ua);
         v[c] = j->swap ? (hx ? j->vb : j->va) : (hy ? j->vb : j->va);
         u2[c] = j->swap2 ? (hy ? j->u2b : j->u2a) : (hx ? j->u2b : j->u2a);
         v2[c] = j->swap2 ? (hx ? j->v2b : j->v2a) : (hy ? j->v2b : j->v2a);
      }
      static const int tv[2][3] = {{0, 1, 2}, {0, 2, 3}};
      for (int t = 0; t < 2; t++) {
         struct sweet_lpf_tri *q = &p->tri[nt++];
         for (int c = 0; c < 3; c++) {
            const int i = tv[t][c];
            q->x[c] = cx[i], q->y[c] = cy[i], q->u[c] = u[i], q->v[c] = v[i], q->u2[c] = u2[i], q->v2[c] = v2[i];
         }
         q->clip[0] = j->x0, q->clip[1] = j->y0, q->clip[2] = j->x1, q->clip[3] = j->y1;
         memcpy(q->color, j->color, 4);
      }
   }
   p->tclamp = p->job[0].tclamp;
   p->cl[0] = p->job[0].cu0, p->cl[1] = p->job[0].cv0, p->cl[2] = p->job[0].cu1, p->cl[3] = p->job[0].cv1;
   p->ntri = nt;
   if (nt == 0)
      return false;
   sweet_lpf_stat[SLPF_MIPTRI]++;
   return sweet_lp_tri_draw(lp);
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

   if (p->ntri > 0)
      return sweet_lp_tri_draw(lp);
   if (p->n <= 0 || p->n > SWEET_LPF_MAX)
      return false;
   const int kd = p->kind;
   if (!((kd == SWEET_LPF_FILL || kd == SWEET_LPF_COPY || kd == SWEET_LPF_OPAQUE || kd == SWEET_LPF_COPYN ||
          kd == SWEET_LPF_TINT || kd == SWEET_LPF_ATINT || kd == SWEET_LPF_TINTN || kd == SWEET_LPF_ALPHA ||
          kd == SWEET_LPF_ATINTDK || kd == SWEET_LPF_COPYDKN || kd == SWEET_LPF_LUMC || kd == SWEET_LPF_CM4 ||
          kd == SWEET_LPF_CM3 || (kd == SWEET_LPF_BLUR && p->ntap > 0 && p->ntap <= SWEET_LPF_TAPS) ||
          kd == SWEET_LPF_MASK || kd == SWEET_LPF_SUPER || kd == SWEET_LPF_CIRCLE || kd == SWEET_LPF_MASKSUP ||
          kd == SWEET_LPF_MIXBD || kd == SWEET_LPF_CFB || kd == SWEET_LPF_CMB || kd == SWEET_LPF_VCM ||
          kd == SWEET_LPF_OMASK || kd == SWEET_LPF_OTINT) &&
         p->blend >= 0 && p->blend <= 6))
      return sweet_lpf_no(SLPF_KIND), false;
   /* drawn through float from the framebuffer (and texture): lp_rast's sweet_fx_row */
   const int fxk = kd == SWEET_LPF_ATINTDK || kd == SWEET_LPF_COPYDKN || kd == SWEET_LPF_LUMC || kd == SWEET_LPF_CM4 ||
                   kd == SWEET_LPF_CM3 || kd == SWEET_LPF_BLUR || kd == SWEET_LPF_MASK || kd == SWEET_LPF_SUPER ||
                   kd == SWEET_LPF_CIRCLE || kd == SWEET_LPF_MASKSUP || kd == SWEET_LPF_MIXBD || kd == SWEET_LPF_CFB ||
                   kd == SWEET_LPF_CMB || kd == SWEET_LPF_VCM || kd == SWEET_LPF_OMASK ? kd : 0;
   const bool needs_tex = kd != SWEET_LPF_FILL && kd != SWEET_LPF_LUMC && kd != SWEET_LPF_CM4 && kd != SWEET_LPF_CM3 &&
                          kd != SWEET_LPF_SUPER && kd != SWEET_LPF_CIRCLE && kd != SWEET_LPF_CFB && kd != SWEET_LPF_CMB;
   const bool needs_tex1 = kd == SWEET_LPF_MASK || kd == SWEET_LPF_MIXBD || kd == SWEET_LPF_OMASK;
   const bool shaped = kd == SWEET_LPF_SUPER || kd == SWEET_LPF_CIRCLE || kd == SWEET_LPF_MASKSUP;
   if (fb->nr_cbufs != 1 || !fb->cbufs[0].texture || fb->layers > 1 ||
       !sweet_lpf_order(fb->cbufs[0].format, dpos, &dx) ||
       lp->active_occlusion_queries || lp->active_statistics_queries || lp->active_primgen_queries)
      return sweet_lpf_no(SLPF_FB), false;
   const uint8_t *sbase = NULL;
   size_t sstride = 0;
   int sf16 = 0;      /* half-float RGBA source: 8 bytes a texel, spos stays R,G,B,A */
   int sa8 = 0;       /* A8 source: 1 byte a texel, expanded to RGBA (spos stays R,G,B,A) */
   const struct pipe_sampler_state *samp = NULL;
   struct sweet_lpf_tab { int32_t *cx, *ry; uint8_t *wx, *wy; int nx, ny; int32_t *nc; int32_t *bt; uint8_t *bw;
                          float *sh; int shnx, shny; } tab[SWEET_LPF_MAX];
   memset(tab, 0, sizeof(tab));
#define SWEET_TAB_FREE(q) (free(tab[q].cx), free(tab[q].wx), free(tab[q].nc), free(tab[q].bt), free(tab[q].bw), free(tab[q].sh))
   if (needs_tex) {
      lpr = res ? llvmpipe_resource(res) : NULL;
      if (lpr == NULL || res->nr_samples > 1 || res->array_size > 1 ||
          (res->target != PIPE_TEXTURE_2D && res->target != PIPE_TEXTURE_RECT) || res == fb->cbufs[0].texture) {
         sweet_lpf_why("src layout", p, NULL, res ? (int)res->format : -1, lpr ? lpr->dt != NULL : -1, 0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      if (res->format == PIPE_FORMAT_R16G16B16A16_FLOAT || res->format == PIPE_FORMAT_R16G16B16X16_FLOAT) {
         sf16 = 1;
         sx = res->format == PIPE_FORMAT_R16G16B16X16_FLOAT;
      } else if (res->format == PIPE_FORMAT_A8_UNORM && kd != SWEET_LPF_BLUR) {
         sa8 = 1;
      } else if (res->format == PIPE_FORMAT_L8A8_UNORM && kd != SWEET_LPF_BLUR) {
         sa8 = 2;
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
   const bool noisy = kd == SWEET_LPF_COPYN || kd == SWEET_LPF_TINTN || kd == SWEET_LPF_COPYDKN;
   struct pipe_resource *nres = noisy ? p->noise : NULL;
   const uint8_t *nbase = NULL;
   size_t nstride = 0;
   int npos[4] = {0, 1, 2, 3}, nxa = 0, NW = 0, NH = 0;
   if (noisy) {
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
   /* MASK / MIXBD: texture1 as bound for this draw, and its sampler */
   struct pipe_resource *res2 = needs_tex1 ? p->src2 : NULL;
   const uint8_t *mbase = NULL;
   size_t mstride = 0;
   int mfmt = 0, mpos[4] = {0, 1, 2, 3}, mxa = 0, MW = 0, MH = 0;
   const struct pipe_sampler_state *msamp = NULL;
   if (needs_tex1) {
      if (res2 == NULL || res2->nr_samples > 1 || res2->array_size > 1 || res2->target != PIPE_TEXTURE_2D ||
          res2 == fb->cbufs[0].texture) {
         sweet_lpf_why("mask layout", p, NULL, res2 ? (int)res2->format : -1, 0, 0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      if (res2->format == PIPE_FORMAT_R16G16B16A16_FLOAT)
         mfmt = 3;
      else if (res2->format == PIPE_FORMAT_A8_UNORM)
         mfmt = 2;
      else if (res2->format == PIPE_FORMAT_L8A8_UNORM)
         mfmt = 4;
      else if (sweet_lpf_order(res2->format, mpos, &mxa) && !mxa)
         mfmt = 1;
      if (mfmt == 0) {
         sweet_lpf_why("mask format", p, NULL, (int)res2->format, 0, 0, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
      for (unsigned i = 0; i < setup->fs.current_tex_num && i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++) {
         const struct lp_jit_texture *jt = &setup->fs.current.jit_resources.textures[i];
         if (setup->fs.current_tex[i] == res2 && jt->base != NULL && jt->first_level == 0) {
            mbase = (const uint8_t *)jt->base + jt->mip_offsets[0];
            mstride = jt->row_stride[0];
            msamp = i < PIPE_MAX_SAMPLERS ? lp->samplers[MESA_SHADER_FRAGMENT][i] : NULL;
            break;
         }
      }
      MW = (int)res2->width0, MH = (int)res2->height0;
      if (mbase == NULL || mstride == 0 || msamp == NULL || msamp->unnormalized_coords ||
          msamp->wrap_s != PIPE_TEX_WRAP_CLAMP_TO_EDGE || msamp->wrap_t != PIPE_TEX_WRAP_CLAMP_TO_EDGE ||
          msamp->min_lod > 0.0f || msamp->lod_bias != 0.0f || MW < 2 || MH < 2) {
         sweet_lpf_why("mask not bound", p, NULL, (int)res2->format, MW, MH, 0);
         return sweet_lpf_no(SLPF_SRC), false;
      }
   }
   /* every rectangle first: all or nothing */
   struct sweet_fast_rect proto[SWEET_LPF_MAX];
   int fbw = (int)fb->width, fbh = (int)fb->height, nr = 0;
   for (int k = 0; k < p->n; k++) {
      const struct sweet_lpf_job *j = &p->job[k];
      struct sweet_fast_rect *r = &proto[nr];
      memset(r, 0, sizeof(*r));
      r->blend = p->blend;
      r->dx = dx;
      r->fx = fxk;
      r->pk = kd;
      for (int ch = 0; ch < 4; ch++)
         r->col[ch] = j->color[ch] * (1.0f / 255);
      if (kd == SWEET_LPF_CMB || kd == SWEET_LPF_VCM)
         memcpy(r->cmv, p->cm5, sizeof(r->cmv));
      for (int ch = 0; ch < 4; ch++)
         r->dpos[ch] = (uint8_t)dpos[ch];
      for (int px = 0; px < 4; px++) {
         for (int ch = 0; ch < 4; ch++) {
            r->perm2[4 * px + dpos[ch]] = (uint8_t)(4 * px + ch);
            r->orm2[4 * px + dpos[ch]] = ch == 3 && dx ? 0xff : 0;
         }
      }
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
      } else if (!needs_tex) {
         /* LUMC / CM4 / CM3: from the framebuffer only; SUPER / CIRCLE: the shape tables below */
         if (shaped || kd == SWEET_LPF_CFB || kd == SWEET_LPF_CMB) {
         } else if (kd == SWEET_LPF_LUMC) {
            memcpy(r->fp, p->farg, sizeof(p->farg));
            r->fp[4] = j->falpha;
         } else {
            for (int m = 0; m < (kd == SWEET_LPF_CM4 ? 4 : 3); m++) {
               for (int ch = 0; ch < 3; ch++)
                  r->fp[4 * m + ch] = p->cm[m][ch];
            }
         }
      } else if (kd == SWEET_LPF_BLUR) {
         /* taps: per tap the texel pairs of (u, v) + toff along columns and rows */
         double scx = fabs(((double)j->ub - j->ua) * W / ((double)j->xb - j->xa));
         double scy = fabs(((double)j->vb - j->va) * H / ((double)j->yb - j->ya));
         bool minify = scx > 1.0 + 1e-6 || scy > 1.0 + 1e-6;
         const char *no = NULL;
         if (j->swap)
            no = "turned";
         else if (samp == NULL || samp->unnormalized_coords || res->target != PIPE_TEXTURE_2D ||
                  samp->wrap_s != PIPE_TEX_WRAP_CLAMP_TO_EDGE || samp->wrap_t != PIPE_TEX_WRAP_CLAMP_TO_EDGE ||
                  samp->min_lod > 0.0f || samp->lod_bias != 0.0f)
            no = "sampler";
         else if (minify && samp->min_mip_filter != PIPE_TEX_MIPFILTER_NONE && res->last_level > 0)
            no = "mipmaps";
         else if (W < 2 || H < 2)
            no = "size";
         if (no) {
            sweet_lpf_why(no, p, j, (int)res->format, W, H, 1);
            for (int q = 0; q < nr; q++)
               SWEET_TAB_FREE(q);
            return sweet_lpf_no(no[0] == 't' ? SLPF_SWAP : SLPF_SCALE), false;
         }
         bool linear = (minify ? samp->min_img_filter : samp->mag_img_filter) == PIPE_TEX_FILTER_LINEAR;
         int nx = r->x1 - r->x0, ny = r->y1 - r->y0, nt = p->ntap;
         tab[nr].nx = nx, tab[nr].ny = ny;
         tab[nr].bt = malloc(sizeof(int32_t) * (size_t)nt * (size_t)(nx + ny));
         tab[nr].bw = malloc((size_t)nt * (size_t)(nx + ny));
         if (tab[nr].bt == NULL || tab[nr].bw == NULL) {
            for (int q = 0; q <= nr; q++)
               SWEET_TAB_FREE(q);
            return false;
         }
         for (int t = 0; t < nt; t++) {
            int32_t *bi = tab[nr].bt + (size_t)t * (nx + ny);
            uint8_t *bw = tab[nr].bw + (size_t)t * (nx + ny);
            sweet_lpf_axis(j->xa, j->xb, j->ua + p->toff[t][0], j->ub + p->toff[t][0], r->x0, nx, W, linear,
                           false, 0, 1, bi, bw);
            sweet_lpf_axis(j->ya, j->yb, j->va + p->toff[t][1], j->vb + p->toff[t][1], r->y0, ny, H, linear,
                           false, 0, 1, bi + nx, bw + nx);
         }
         r->ntap = nt, r->bdiv = p->bdiv;
         memcpy(r->tw, p->tw, sizeof(r->tw));
         r->sbase = sbase;
         r->sstride = (ptrdiff_t)sstride;
         r->f16 = sf16;
         for (int ch = 0; ch < 4; ch++)
            r->spos[ch] = (uint8_t)spos[ch];
      } else {
         /* clipped to the framebuffer: map the clipped corner */
         struct sweet_lpf_job jj = *j;
         jj.x0 = r->x0, jj.x1 = r->x1, jj.y0 = r->y0, jj.y1 = r->y1;
         int s0, t0, dy;
         uint64_t before[SLPF_N];
         memcpy(before, sweet_lpf_stat, sizeof(before));
         if (sweet_lpf_map(&jj, W, H, &s0, &t0, &dy) == 0) {
            r->src = sbase + (size_t)t0 * sstride + (size_t)s0 * (sf16 ? 8 : sa8 ? sa8 : 4);
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
            else if (sw1 && sa8)
               no = "turned";
            if (no) {
               sweet_lpf_why(no, p, &jj, (int)res->format, W, H, 1);
               for (int q = 0; q < nr; q++)
                  SWEET_TAB_FREE(q);
               /* turned or mipmapped: per-pixel triangles can */
               if ((no[0] == 't' || no[0] == 'm') && sweet_lp_rects_as_tris(lp))
                  return true;
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
                  SWEET_TAB_FREE(q);
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
                  SWEET_TAB_FREE(q);
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
               r->orm[4 * px + dpos[ch]] =
                  ch == 3 && (sx || dx || p->kind == SWEET_LPF_OPAQUE || p->kind == SWEET_LPF_OTINT) ? 0xff : 0;
            }
         }
         if (p->kind == SWEET_LPF_ATINT || p->kind == SWEET_LPF_ALPHA) {
            /* (colour *) texel alpha: the source alpha into every byte (X sources: 255 via orm) */
            for (int px = 0; px < 4; px++) {
               for (int ch = 0; ch < 4; ch++) {
                  r->perm[4 * px + dpos[ch]] = (uint8_t)(4 * px + spos[3]);
                  r->orm[4 * px + dpos[ch]] = sx ? 0xff : 0;
               }
            }
         }
         if (p->kind == SWEET_LPF_TINT || p->kind == SWEET_LPF_ATINT || p->kind == SWEET_LPF_TINTN ||
             p->kind == SWEET_LPF_OTINT) {
            r->tint = p->kind == SWEET_LPF_ATINT ? 2 : 1;
            for (int px = 0; px < 4; px++) {
               for (int ch = 0; ch < 4; ch++)
                  r->tcol[4 * px + dpos[ch]] = j->color[ch];
            }
            for (int ch = 0; ch < 4; ch++)
               r->tcolf[ch] = j->color[ch] * (1.0f / 255);
         }
         for (int px = 0; px < 4; px++) {
            for (int ch = 0; ch < 4; ch++) {
               r->perm2[4 * px + dpos[ch]] = (uint8_t)(4 * px + ch);
               r->orm2[4 * px + dpos[ch]] = ch == 3 && dx ? 0xff : 0;
            }
         }
         r->f16 = sf16;
         r->a8 = sa8;
         r->opaque = sx || p->kind == SWEET_LPF_OPAQUE || p->kind == SWEET_LPF_OTINT;
         for (int ch = 0; ch < 4; ch++) {
            r->spos[ch] = (uint8_t)spos[ch];
            r->permr[ch] = (uint8_t)spos[ch];
            r->ormr[ch] = ch == 3 && sx ? 0xff : 0;
            r->col[ch] = j->color[ch] * (1.0f / 255);
         }
         if (fxk) {
            /* the 8-bit row only orders and blends what sweet_fx_row computed */
            r->tint = 0;
         }
      }
      if (shaped) {
         /* CA's shape terms per column and row, from x0 & ~1 to (x1 - 1) | 1 (whole quads) */
         const int two = kd == SWEET_LPF_MASKSUP, sw = two ? j->swap2 : j->swap;
         const double ua = two ? j->u2a : j->ua, ub = two ? j->u2b : j->ub, va = two ? j->v2a : j->va,
                      vb = two ? j->v2b : j->vb;
         const double xa0 = sw ? va : ua, xa1 = sw ? vb : ub, ya0 = sw ? ua : va, ya1 = sw ? ub : vb;
         const double dxs = (xa1 - xa0) / ((double)j->xb - j->xa), dys = (ya1 - ya0) / ((double)j->yb - j->ya);
         const int sx0 = r->x0 & ~1, sy0 = r->y0 & ~1, snx = ((r->x1 - 1) | 1) - sx0 + 1, sny = ((r->y1 - 1) | 1) - sy0 + 1;
         tab[nr].sh = malloc(sizeof(float) * (size_t)(snx + sny));
         if (tab[nr].sh == NULL) {
            for (int q = 0; q <= nr; q++)
               SWEET_TAB_FREE(q);
            return false;
         }
         for (int k2 = 0; k2 < snx + sny; k2++) {
            double c = k2 < snx ? xa0 + ((sx0 + k2) + 0.5 - j->xa) * dxs : ya0 + ((sy0 + k2 - snx) + 0.5 - j->ya) * dys;
            float t;
            if (kd == SWEET_LPF_CIRCLE) {
               t = (float)(c * c);
            } else {
               float a = fabsf((float)c) * 1.199f - 0.199f;
               t = powf(a > 0.0f ? a : 0.0f, 2.68f);
            }
            tab[nr].sh[k2] = t;
         }
         tab[nr].shnx = snx, tab[nr].shny = sny;
         r->shk = kd == SWEET_LPF_CIRCLE ? 16 : 15;
         r->shx0 = sx0, r->shy0 = sy0;
      }
      if (needs_tex1) {
         /* texture1's texel pairs per column and row (in the tap tables' slot 0) */
         double scx = fabs(((double)j->u2b - j->u2a) * MW / ((double)j->xb - j->xa));
         double scy = fabs(((double)j->v2b - j->v2a) * MH / ((double)j->yb - j->ya));
         bool minify = scx > 1.0 + 1e-6 || scy > 1.0 + 1e-6;
         if (j->swap2 || (minify && msamp->min_mip_filter != PIPE_TEX_MIPFILTER_NONE && res2->last_level > 0)) {
            sweet_lpf_why(j->swap2 ? "mask turned" : "mask mipmaps", p, j, (int)res2->format, MW, MH, 1);
            for (int q = 0; q <= nr; q++)
               SWEET_TAB_FREE(q);
            if (sweet_lp_rects_as_tris(lp))
               return true;
            return sweet_lpf_no(j->swap2 ? SLPF_SWAP : SLPF_SCALE), false;
         }
         bool linear = (minify ? msamp->min_img_filter : msamp->mag_img_filter) == PIPE_TEX_FILTER_LINEAR;
         int nx = r->x1 - r->x0, ny = r->y1 - r->y0;
         tab[nr].nx = nx, tab[nr].ny = ny;
         tab[nr].bt = malloc(sizeof(int32_t) * (size_t)(nx + ny));
         tab[nr].bw = malloc((size_t)(nx + ny));
         if (tab[nr].bt == NULL || tab[nr].bw == NULL) {
            for (int q = 0; q <= nr; q++)
               SWEET_TAB_FREE(q);
            return false;
         }
         sweet_lpf_axis(j->xa, j->xb, j->u2a, j->u2b, r->x0, nx, MW, linear, false, 0, 1, tab[nr].bt, tab[nr].bw);
         sweet_lpf_axis(j->ya, j->yb, j->v2a, j->v2b, r->y0, ny, MH, linear, false, 0, 1, tab[nr].bt + nx,
                        tab[nr].bw + nx);
         r->ntap = 1;
         r->mbase = mbase;
         r->mstride = (ptrdiff_t)mstride;
         r->mfmt = mfmt;
         for (int ch = 0; ch < 4; ch++)
            r->mpos[ch] = (uint8_t)mpos[ch];
      }
      nr++;
   }
   if (!lp_setup_update_state(setup, true)) {
      for (int k = 0; k < nr; k++)
         SWEET_TAB_FREE(k);
      return false;
   }
   for (int k = 0; k < nr; k++) {
      /* texture reference, a scaled rectangle's tables and the commands, all in one scene */
      struct sweet_fast_rect *made = NULL;
#define SWEET_LPF_PUT() ((!res || lp_scene_add_resource_reference(setup->scene, res, false, false)) && \
                         (!nres || lp_scene_add_resource_reference(setup->scene, nres, false, false)) && \
                         ((!tab[k].cx && !tab[k].nc) || \
                          sweet_lpf_tables(setup, &proto[k], tab[k].cx, tab[k].wx, tab[k].nc, tab[k].nx, tab[k].ny)) && \
                         (!tab[k].bt || sweet_lpf_btables(setup, &proto[k], tab[k].bt, tab[k].bw, proto[k].ntap, tab[k].nx, tab[k].ny)) && \
                         (!tab[k].sh || sweet_lpf_shtables(setup, &proto[k], tab[k].sh, tab[k].shnx, tab[k].shny)) && \
                         (!res2 || lp_scene_add_resource_reference(setup->scene, res2, false, false)) && \
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
      SWEET_TAB_FREE(k);
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
      lp->fs->sweet_kind = (sweet_lp_draw_kind & 0xff) | (sweet_lp_draw_kind >> 16) << 8;''', '''   /* sweetgl (lp-fast.py): drawn by the rasterizer threads directly? */
   if (sweet_lpf_pending.n > 0) {
      bool taken = sweet_lp_fast_draw(lp);
      sweet_lpf_pending.n = 0;
      if (taken)
         return;
   }

   if (lp->fs) {
      lp->fs->sweet_kind = (sweet_lp_draw_kind & 0xff) | (sweet_lp_draw_kind >> 16) << 8;'''),
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
      static uint64_t pl[16];
      uint64_t d[16];
      for (int i = 0; i < 16; i++) {
         d[i] = sweet_lpf_stat[i] - pl[i];
         pl[i] = sweet_lpf_stat[i];
      }
      lg("sweetgl: lp fast: %llu draws, %llu rects (%llu scaled), %llu kpx; not taken: kind %llu fb %llu src %llu scale %llu "
         "offset %llu bounds %llu turned %llu clamp %llu; out of scene memory %llu; as triangles %llu draws "
         "(%llu of them mipmapped / turned rectangles), %llu triangles",
         (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[12], (unsigned long long)(d[2] / 1000),
         (unsigned long long)d[11], (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5],
         (unsigned long long)d[6], (unsigned long long)d[7], (unsigned long long)d[8], (unsigned long long)d[9],
         (unsigned long long)d[10], (unsigned long long)d[14], (unsigned long long)d[15],
         (unsigned long long)d[13]);
   }'''),
    ])
