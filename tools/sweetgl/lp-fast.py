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
};'''),
    ])

    # ---- lp_rast.c: tables, the command
    RAST_CMD = r'''

/* sweetgl (lp-fast.py): LP_RAST_OP_SWEET_FAST */
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

static inline uint8_t
sweet_div255(unsigned t)
{
   t += 128;
   return (uint8_t)((t + (t >> 8)) >> 8);
}

static void
sweet_fast_row(const struct sweet_fast_rect *r, uint8_t *d, const uint8_t *s, int n)
{
   int i = 0;
#if defined(__aarch64__)
   static const uint8_t aidx_b[16] = {3, 3, 3, 3, 7, 7, 7, 7, 11, 11, 11, 11, 15, 15, 15, 15};
   const uint8x16_t aidx = vld1q_u8(aidx_b);
   const uint8x16_t perm = vld1q_u8(r->perm), orm = vld1q_u8(r->orm);
   const uint8x16_t fill = vreinterpretq_u8_u32(vdupq_n_u32(r->color));
   for (; i + 4 <= n; i += 4) {
      uint8x16_t sv = s ? vorrq_u8(vqtbl1q_u8(vld1q_u8(s + 4 * i), perm), orm) : fill;
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
   const uint8_t *s = r->src ? r->src + (ptrdiff_t)(y0 - r->y0) * r->src_dy + (ptrdiff_t)(x0 - r->x0) * 4 : NULL;
   for (int y = y0; y < y1; y++) {
      sweet_fast_row(r, d, s, x1 - x0);
      d += stride;
      if (s)
         s += r->src_dy;
   }
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
       SLPF_SWAP, SLPF_CLAMP, SLPF_OOM, SLPF_KIND, SLPF_N };
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

static int
sweet_lpf_no(int why)
{
   sweet_lpf_stat[why]++;
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
   if (!((p->kind == SWEET_LPF_FILL || p->kind == SWEET_LPF_COPY || p->kind == SWEET_LPF_OPAQUE) &&
         (p->blend == SWEET_LPF_BLEND_NONE || p->blend == SWEET_LPF_BLEND_PREMUL)))
      return sweet_lpf_no(SLPF_KIND), false;
   if (fb->nr_cbufs != 1 || !fb->cbufs[0].texture || fb->layers > 1 ||
       !sweet_lpf_order(fb->cbufs[0].format, dpos, &dx) ||
       lp->active_occlusion_queries || lp->active_statistics_queries || lp->active_primgen_queries)
      return sweet_lpf_no(SLPF_FB), false;
   if (p->kind != SWEET_LPF_FILL) {
      lpr = res ? llvmpipe_resource(res) : NULL;
      if (lpr == NULL || lpr->dt || lpr->tex_data == NULL || res->nr_samples > 1 || res->array_size > 1 ||
          (res->target != PIPE_TEXTURE_2D && res->target != PIPE_TEXTURE_RECT) || res == fb->cbufs[0].texture ||
          !sweet_lpf_order(res->format, spos, &sx))
         return sweet_lpf_no(SLPF_SRC), false;
      W = (int)res->width0, H = (int)res->height0;
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
         if (sweet_lpf_map(&jj, W, H, &s0, &t0, &dy) != 0)
            return false;
         r->src = (const uint8_t *)lpr->tex_data + lpr->mip_offsets[0] +
                  (size_t)t0 * lpr->row_stride[0] + (size_t)s0 * 4;
         r->src_dy = dy * (ptrdiff_t)lpr->row_stride[0];
         for (int px = 0; px < 4; px++) {
            for (int ch = 0; ch < 4; ch++) {
               r->perm[4 * px + dpos[ch]] = (uint8_t)(4 * px + spos[ch]);
               r->orm[4 * px + dpos[ch]] = ch == 3 && (sx || dx || p->kind == SWEET_LPF_OPAQUE) ? 0xff : 0;
            }
         }
      }
      nr++;
   }
   if (!lp_setup_update_state(setup, true))
      return false;
   for (int k = 0; k < nr; k++) {
      struct sweet_fast_rect *made = NULL;
      if (res && !lp_scene_add_resource_reference(setup->scene, res, false, false)) {
         if (!lp_setup_flush_and_restart(setup))
            return true;      /* nothing binned for this or the later rectangles */
      }
      if (!sweet_lpf_bin(setup, &proto[k], &made)) {
         /* out of scene memory half way: drop what was binned, new scene, again */
         if (made)
            made->disable = 1;
         sweet_lpf_stat[SLPF_OOM]++;
         if (!lp_setup_flush_and_restart(setup) ||
             (res && !lp_scene_add_resource_reference(setup->scene, res, false, false)) ||
             !sweet_lpf_bin(setup, &proto[k], &made)) {
            if (made)
               made->disable = 1;
            continue;
         }
      }
      sweet_lpf_stat[SLPF_RECTS]++;
      sweet_lpf_stat[SLPF_PIXELS] += (uint64_t)(proto[k].x1 - proto[k].x0) * (uint64_t)(proto[k].y1 - proto[k].y0);
   }
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
      static uint64_t pl[12];
      uint64_t d[12];
      for (int i = 0; i < 12; i++) {
         d[i] = sweet_lpf_stat[i] - pl[i];
         pl[i] = sweet_lpf_stat[i];
      }
      lg("sweetgl: lp fast: %llu draws, %llu rects, %llu kpx; not taken: kind %llu fb %llu src %llu scale %llu "
         "offset %llu bounds %llu turned %llu clamp %llu; out of scene memory %llu",
         (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)(d[2] / 1000),
         (unsigned long long)d[11], (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5],
         (unsigned long long)d[6], (unsigned long long)d[7], (unsigned long long)d[8], (unsigned long long)d[9],
         (unsigned long long)d[10]);
   }'''),
    ])
