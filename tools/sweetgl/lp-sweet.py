#!/usr/bin/env python3
# sweetgl changes to Mesa's llvmpipe (build-llvmpipe.sh runs this on the Mesa tree).
#
# 1. No CPU wait when a fragment shader samples a texture that an earlier
#    scene renders to. llvmpipe's rasterizer threads take scenes one at a time
#    from one queue (lp_scene_dequeue, two barriers per scene), so a later
#    scene always sees an earlier scene's pixels. Stock llvmpipe still calls
#    llvmpipe_finish there (lp_state_sampler.c, "sampler_view"): the GL thread
#    stops until the queue is empty, and the rasterizer threads then sit idle
#    while the next scene is binned. Core Animation does this on every
#    offscreen pass (blurs, masks, group opacity). Now: flush the scene being
#    built if it writes the texture, never wait. Another context's unflushed
#    scene, or a vertex-shader sampler (run on the CPU by draw), keeps the
#    stock finish. Environment SGLLPWAIT=1 restores the stock behaviour.
#    The same for glTextureBarrierNV (llvmpipe_texture_barrier): eagl.c calls
#    it before every draw that reads the framebuffer it renders to (CA's
#    framebuffer fetch, emulated by sampling): flush, no wait. 8d9994d's
#    profile showed ~1100 of these per 10 s during animations, ~3 s of
#    GL-thread waiting.
#    Framebuffer-fetch draws (eagl.c sets sweet_lp_fbfetch_draw around them)
#    stay in the scene being built: each fragment reads only its own pixel,
#    llvmpipe renders straight into the texture and runs a bin's commands in
#    order on one thread, so earlier draws are already in memory. The opaque
#    whole-tile bin reset (which drops a bin's earlier commands) is skipped
#    while a bound fragment sampler reads a framebuffer texture.
# 2. A profile of the rasterizer threads, printed by eagl.c every 10 s
#    (sweet_lp_report): scenes and their sizes, busy and barrier time, why
#    scenes were flushed, where the GL thread waited, time by command and by
#    fragment-shader variant.
import sys, os

M = sys.argv[1]
LP = os.path.join(M, 'src/gallium/drivers/llvmpipe')


def patch(name, edits, tag='lp-sweet'):
    p = os.path.join(LP, name)
    s = open(p).read()
    mark = f'/* sweetgl: {tag} */'
    if mark in s:
        print('already patched', p, tag)
        return
    for a, b in edits:
        assert s.count(a) == 1, f'{name}: anchor not found once: {a[:60]!r}'
        s = s.replace(a, b)
    s = mark + '\n' + s
    open(p, 'w').write(s)
    print('patched', p, tag)


# ---------------------------------------------------------------- lp_rast.c
PROF = r'''
/* sweetgl: rasterizer profile (lp-sweet.py). Per-thread counters, no locks;
 * sweet_lp_report() prints the change since its last call. */
#include <mach/mach_time.h>
#define SWP_T 16
#define SWP_V 256
struct swp_var {
   const void *key;
   uint64_t ticks, cmds;
   unsigned sh, no, instrs, samplers, blend, src, dst, fmt, flags, skind;
   uint64_t sdraws, squal;   /* copied from the shader while the variant is in use */
};
struct swp_thr {
   uint64_t busy, wait, scenes, bins, tile, blit, vdrop, scene_busy;
   uint64_t op_ticks[LP_RAST_OP_MAX], op_n[LP_RAST_OP_MAX];
   struct swp_var var[SWP_V];
};
static struct swp_thr swp_thr[SWP_T];
#define SWP_S 24
static struct { unsigned w, h; uint64_t n, wall, busy; } swp_size[SWP_S];
static uint64_t swp_scenes, swp_wall, swp_sdrop, swp_scene_t0;
static unsigned swp_nthreads;

static inline uint64_t swp_now(void) { return mach_absolute_time(); }

static void
swp_var_add(struct swp_thr *t, const struct lp_rast_state *st, uint64_t d)
{
   const struct lp_fragment_shader_variant *v = st ? st->variant : NULL;
   uintptr_t k = (uintptr_t)v;
   unsigned h = (unsigned)((k >> 6) ^ (k >> 14)) & (SWP_V - 1);
   for (unsigned i = 0; i < 16; i++) {
      struct swp_var *e = &t->var[(h + i) & (SWP_V - 1)];
      if (e->key == v && (v != NULL || e->cmds != 0 || e->ticks != 0)) {
         e->ticks += d;
         e->cmds++;
         if (v && v->shader && (e->cmds & 63) == 1) {
            e->skind = v->shader->sweet_kind;
            e->sdraws = v->shader->sweet_draws;
            e->squal = v->shader->sweet_qual;
         }
         return;
      }
      if (e->key == NULL && e->cmds == 0) {
         if (v) {
            e->sh = v->shader ? v->shader->no : 0;
            e->no = v->no;
            e->instrs = v->nr_instrs;
            e->samplers = v->key.nr_samplers;
            e->blend = v->key.blend.rt[0].blend_enable;
            e->src = v->key.blend.rt[0].rgb_src_factor;
            e->dst = v->key.blend.rt[0].rgb_dst_factor;
            e->fmt = v->key.cbuf_format[0];
            e->flags = v->opaque | v->potentially_opaque << 1 | v->blit << 2 |
                       (v->shader ? (unsigned)v->shader->kind << 4 : 0);
            e->skind = v->shader ? v->shader->sweet_kind : 0;
         }
         e->key = v;
         e->ticks += d;
         e->cmds++;
         return;
      }
   }
   t->vdrop++;
}
'''

RAST_EDITS = [
    ('''#ifdef _WIN32
#include <windows.h>
#endif
''', '''#ifdef _WIN32
#include <windows.h>
#endif
''' + PROF),
    # time every command of a triangle bin, by op and by shader variant
    ('''   STATIC_ASSERT(ARRAY_SIZE(dispatch_tri) == LP_RAST_OP_MAX);

   for (const struct cmd_block *block = bin->head; block; block = block->next) {
      for (unsigned k = 0; k < block->count; k++) {
         dispatch_tri[block->cmd[k]](task, block->arg[k]);
      }
   }
''', '''   STATIC_ASSERT(ARRAY_SIZE(dispatch_tri) == LP_RAST_OP_MAX);

   struct swp_thr *swt = task->thread_index < SWP_T ? &swp_thr[task->thread_index] : NULL;
   /* task->state is only valid after this bin's own SET_STATE: before it, it
    * still points into an earlier (possibly freed) scene */
   const struct lp_rast_state *swst = NULL;
   uint64_t swt0 = swp_now();
   for (const struct cmd_block *block = bin->head; block; block = block->next) {
      for (unsigned k = 0; k < block->count; k++) {
         unsigned op = block->cmd[k];
         dispatch_tri[op](task, block->arg[k]);
         if (swt) {
            uint64_t swt1 = swp_now(), d = swt1 - swt0;
            swt0 = swt1;
            swt->op_ticks[op] += d;
            swt->op_n[op]++;
            if (op == LP_RAST_OP_SET_STATE)
               swst = task->state;
            else
               swp_var_add(swt, swst, d);
         }
      }
   }
'''),
    # bin-level: tile begin/end and blit bins
    ('''   struct lp_bin_info info = lp_characterize_bin(bin);

   lp_rast_tile_begin(task, bin, x, y);

   if (LP_DEBUG & DEBUG_NO_FASTPATH) {
      debug_rasterize_bin(task, bin);
   } else if (info.type & LP_RAST_FLAGS_BLIT) {
      blit_rasterize_bin(task, bin);
   }''', '''   struct lp_bin_info info = lp_characterize_bin(bin);
   struct swp_thr *swt = task->thread_index < SWP_T ? &swp_thr[task->thread_index] : NULL;
   uint64_t swb0 = swp_now();

   lp_rast_tile_begin(task, bin, x, y);
   uint64_t swb1 = swp_now();
   if (swt) {
      swt->tile += swb1 - swb0;
      swt->bins++;
   }

   if (LP_DEBUG & DEBUG_NO_FASTPATH) {
      debug_rasterize_bin(task, bin);
   } else if (info.type & LP_RAST_FLAGS_BLIT) {
      blit_rasterize_bin(task, bin);
      if (swt)
         swt->blit += swp_now() - swb1;
   }'''),
    ('''   lp_rast_tile_end(task);

#if MESA_DEBUG
   /* Debug/Perf flags:''', '''   uint64_t swb2 = swp_now();
   lp_rast_tile_end(task);
   if (swt)
      swt->tile += swp_now() - swb2;

#if MESA_DEBUG
   /* Debug/Perf flags:'''),
    # scene-level: busy, barrier wait; thread 0 records the scene's size and wall time
    ('''      if (task->thread_index == 0) {
         /* thread[0]:
          *  - get next scene to rasterize
          *  - map the framebuffer surfaces
          */
         lp_rast_begin(rast, lp_scene_dequeue(rast->full_scenes, true));
      }
''', '''      if (task->thread_index == 0) {
         /* thread[0]:
          *  - get next scene to rasterize
          *  - map the framebuffer surfaces
          */
         lp_rast_begin(rast, lp_scene_dequeue(rast->full_scenes, true));
         swp_scene_t0 = swp_now();
         swp_nthreads = rast->num_threads;
      }
'''),
    ('''      rasterize_scene(task, rast->curr_scene);

      /* wait for all threads to finish with this scene */
      util_barrier_wait(&rast->barrier);
''', '''      uint64_t sws0 = swp_now();
      struct lp_scene *sws_scene = rast->curr_scene;
      rasterize_scene(task, sws_scene);
      uint64_t sws1 = swp_now();
      struct swp_thr *swt = task->thread_index < SWP_T ? &swp_thr[task->thread_index] : NULL;
      if (swt) {
         swt->busy += sws1 - sws0;
         swt->scene_busy = sws1 - sws0;
         swt->scenes++;
      }

      /* wait for all threads to finish with this scene */
      util_barrier_wait(&rast->barrier);
      uint64_t sws2 = swp_now();
      if (swt)
         swt->wait += sws2 - sws1;
      if (task->thread_index == 0 && sws_scene) {
         /* every thread's scene_busy is in: they all passed the barrier */
         uint64_t b = 0;
         for (unsigned i = 0; i < rast->num_threads && i < SWP_T; i++)
            b += swp_thr[i].scene_busy;
         unsigned w = sws_scene->fb.width, h = sws_scene->fb.height, i;
         for (i = 0; i < SWP_S; i++) {
            if ((swp_size[i].w == w && swp_size[i].h == h) || swp_size[i].n == 0)
               break;
         }
         if (i < SWP_S) {
            swp_size[i].w = w;
            swp_size[i].h = h;
            swp_size[i].n++;
            swp_size[i].wall += sws2 - swp_scene_t0;
            swp_size[i].busy += b;
         } else {
            swp_sdrop++;
         }
         swp_scenes++;
         swp_wall += sws2 - swp_scene_t0;
      }
'''),
]

REPORT = r'''

/* sweetgl: lp-sweet.py; printed every 10 s from eagl.c */
#include <stdarg.h>
extern void sweet_lp_waits_report(void (*lg)(const char *, ...), double ms_per_tick);

static int
swp_cmp_var(const void *a, const void *b)
{
   const struct swp_var *x = a, *y = b;
   return x->ticks < y->ticks ? 1 : x->ticks > y->ticks ? -1 : 0;
}

void sweet_lp_report(void (*lg)(const char *, ...));
void
sweet_lp_report(void (*lg)(const char *, ...))
{
   static struct swp_thr prev[SWP_T];
   static struct swp_var all[SWP_T * SWP_V];
   static uint64_t pscenes, pwall, psize_n[SWP_S], psize_wall[SWP_S], psize_busy[SWP_S];
   static mach_timebase_info_data_t tb;
   if (tb.denom == 0)
      mach_timebase_info(&tb);
   double ms = (double)tb.numer / tb.denom / 1e6;
   uint64_t busy = 0, wait = 0, bins = 0, tile = 0, blit = 0, vdrop = 0;
   uint64_t op_t[LP_RAST_OP_MAX] = {0}, op_n[LP_RAST_OP_MAX] = {0};
   unsigned nall = 0;
   char busyl[160];
   int bo = snprintf(busyl, sizeof(busyl), "by thread");
   for (unsigned t = 0; t < SWP_T; t++) {
      struct swp_thr *c = &swp_thr[t], *p = &prev[t];
      uint64_t db = c->busy - p->busy;
      busy += db;
      wait += c->wait - p->wait;
      bins += c->bins - p->bins;
      tile += c->tile - p->tile;
      blit += c->blit - p->blit;
      vdrop += c->vdrop - p->vdrop;
      if (c->scenes && bo < (int)sizeof(busyl) - 12)
         bo += snprintf(busyl + bo, sizeof(busyl) - bo, " %.0f", db * ms);
      for (unsigned o = 0; o < LP_RAST_OP_MAX; o++) {
         op_t[o] += c->op_ticks[o] - p->op_ticks[o];
         op_n[o] += c->op_n[o] - p->op_n[o];
      }
      for (unsigned v = 0; v < SWP_V; v++) {
         struct swp_var e = c->var[v];
         uint64_t dt = e.ticks - p->var[v].ticks, dn = e.cmds - p->var[v].cmds;
         if (dn == 0)
            continue;
         unsigned j;
         for (j = 0; j < nall; j++) {
            if (all[j].key == e.key)
               break;
         }
         if (j == nall) {
            all[nall] = e;
            all[nall].ticks = 0;
            all[nall].cmds = 0;
            nall++;
         }
         all[j].ticks += dt;
         all[j].cmds += dn;
      }
      memcpy(p, c, sizeof(*p));
   }
   uint64_t sc = swp_scenes - pscenes, wall = swp_wall - pwall;
   pscenes = swp_scenes;
   pwall = swp_wall;
   lg("sweetgl: lp %llu scenes, wall %.0f ms (%.2f ms each), busy %.0f ms (%.0f%% of %u threads), barrier wait %.0f ms; "
       "%llu bins, tile load/store %.0f ms, blit bins %.0f ms; %s",
       (unsigned long long)sc, wall * ms, sc ? wall * ms / sc : 0.0, busy * ms,
       wall && swp_nthreads ? 100.0 * busy / ((double)wall * swp_nthreads) : 0.0, swp_nthreads, wait * ms,
       (unsigned long long)bins, tile * ms, blit * ms, busyl);
   /* scene sizes, busiest first */
   {
      char l[400];
      int o = snprintf(l, sizeof(l), "sweetgl: lp sizes (WxH n wall/busy ms):");
      bool used[SWP_S] = {0};
      for (int r = 0; r < 10; r++) {
         int best = -1;
         uint64_t bb = 0;
         for (int i = 0; i < SWP_S; i++) {
            uint64_t d = swp_size[i].busy - psize_busy[i];
            if (!used[i] && swp_size[i].n - psize_n[i] && (best < 0 || d > bb)) {
               best = i;
               bb = d;
            }
         }
         if (best < 0 || o > (int)sizeof(l) - 40)
            break;
         used[best] = true;
         o += snprintf(l + o, sizeof(l) - o, " %ux%u %llu %.0f/%.0f", swp_size[best].w, swp_size[best].h,
                       (unsigned long long)(swp_size[best].n - psize_n[best]),
                       (swp_size[best].wall - psize_wall[best]) * ms, bb * ms);
      }
      for (int i = 0; i < SWP_S; i++) {
         psize_n[i] = swp_size[i].n;
         psize_wall[i] = swp_size[i].wall;
         psize_busy[i] = swp_size[i].busy;
      }
      if (swp_sdrop)
         o += snprintf(l + o, sizeof(l) - o, " (+%llu other)", (unsigned long long)swp_sdrop);
      lg("%s", l);
   }
   /* commands */
   {
      static const char *const name[LP_RAST_OP_MAX] = {
         [LP_RAST_OP_CLEAR_COLOR] = "clear", [LP_RAST_OP_CLEAR_ZSTENCIL] = "clearz",
         [LP_RAST_OP_SHADE_TILE] = "tile", [LP_RAST_OP_SHADE_TILE_OPAQUE] = "tile-opaque",
         [LP_RAST_OP_SET_STATE] = "state", [LP_RAST_OP_SWEET_FAST] = "fast",
      };
      char l[400];
      int o = snprintf(l, sizeof(l), "sweetgl: lp commands (ms/thousands):");
      uint64_t tri_t = 0, tri_n = 0;
      for (unsigned i = 0; i < LP_RAST_OP_MAX; i++) {
         if (name[i] == NULL) {
            tri_t += op_t[i];
            tri_n += op_n[i];
         } else if (op_n[i] && o < (int)sizeof(l) - 40) {
            o += snprintf(l + o, sizeof(l) - o, " %s %.0f/%.1f", name[i], op_t[i] * ms, op_n[i] / 1000.0);
         }
      }
      o += snprintf(l + o, sizeof(l) - o, " triangles %.0f/%.1f", tri_t * ms, tri_n / 1000.0);
      for (unsigned i = 0; i < LP_RAST_OP_MAX && o < (int)sizeof(l) - 24; i++) {
         if (name[i] == NULL && op_n[i] * 20 > tri_n)
            o += snprintf(l + o, sizeof(l) - o, " [%#x %.0f/%.1f]", i, op_t[i] * ms, op_n[i] / 1000.0);
      }
      lg("%s", l);
   }
   /* shader variants */
   qsort(all, nall, sizeof(all[0]), swp_cmp_var);
   static const char *const fpk[] = {
      "?", "fill", "copy", "tint", "atint", "opaque", "alpha", "copyn", "tintn", "atintdk", "copydkn",
      "mask", "lumc", "cm4", "cm3", "super", "circle", "masksup", "mixbd", "blur",
   };
   for (unsigned j = 0; j < nall && j < 14; j++) {
      struct swp_var *e = &all[j];
      if (e->key == NULL) {
         lg("sweetgl: lp var (no state): %.0f ms, %llu cmds", e->ticks * ms, (unsigned long long)e->cmds);
         continue;
      }
      unsigned sk = e->skind;
      unsigned long long sd = e->sdraws, sq = e->squal;
      lg("sweetgl: lp var fs %u.%u %s: %.0f ms, %llu cmds; %u samplers, blend %u (%u,%u), fmt %u, "
          "opaque %u/%u kind %u; draws %llu, fast-path %llu",
          e->sh, e->no, sk < sizeof(fpk) / sizeof(fpk[0]) ? fpk[sk] : "?", e->ticks * ms,
          (unsigned long long)e->cmds, e->samplers, e->blend, e->src,
          e->dst, e->fmt, e->flags & 1, (e->flags >> 1) & 1, e->flags >> 4, sd, sq);
   }
   if (vdrop)
      lg("sweetgl: lp var table full: %llu commands not counted", (unsigned long long)vdrop);
   sweet_lp_waits_report(lg, ms);
}
'''

RAST_EDITS.append(('''/**
 * Called by setup module when it has something for us to render.
 */''', REPORT + '''

/**
 * Called by setup module when it has something for us to render.
 */'''))

patch('lp_rast.c', RAST_EDITS)

# ---------------------------------------------------------------- lp_setup.c
SETUP_EDITS = [
    ('''static bool
set_scene_state(struct lp_setup_context *setup,
                enum setup_state new_state,
                const char *reason)
{
   const unsigned old_state = setup->state;

   if (old_state == new_state)
      return true;
''', '''/* sweetgl: why scenes were sent to the rasterizer (by reason string) */
#define SWP_R 24
struct swp_reason { const char *r; uint64_t n, ticks; };
extern struct swp_reason sweet_lp_flushes[SWP_R], sweet_lp_waits[SWP_R];
struct swp_reason sweet_lp_flushes[SWP_R], sweet_lp_waits[SWP_R];
void sweet_lp_reason_add(struct swp_reason *t, const char *r, uint64_t ticks);
void
sweet_lp_reason_add(struct swp_reason *t, const char *r, uint64_t ticks)
{
   for (unsigned i = 0; i < SWP_R; i++) {
      if (t[i].r == r || t[i].r == NULL) {
         t[i].r = r;
         t[i].n++;
         t[i].ticks += ticks;
         return;
      }
   }
   t[SWP_R - 1].n++;
   t[SWP_R - 1].ticks += ticks;
}

static bool
set_scene_state(struct lp_setup_context *setup,
                enum setup_state new_state,
                const char *reason)
{
   const unsigned old_state = setup->state;

   if (old_state == new_state)
      return true;

   if (new_state == SETUP_FLUSHED && old_state != SETUP_FLUSHED)
      sweet_lp_reason_add(sweet_lp_flushes, reason ? reason : "?", 0);
'''),
    ('''boolean_is_placeholder_never_matches''', ''),
]
SETUP_EDITS.pop()
SETUP_EDITS.append(('''/**
 * Called by vbuf code when we're about to draw something.''', '''/* sweetgl: is texture a colour buffer of the framebuffer being drawn to? */
bool sweet_lp_setup_fb_has(const struct lp_setup_context *setup, const struct pipe_resource *texture);
bool
sweet_lp_setup_fb_has(const struct lp_setup_context *setup, const struct pipe_resource *texture)
{
   for (unsigned i = 0; i < setup->fb.nr_cbufs; i++) {
      if (setup->fb.cbufs[i].texture == texture)
         return true;
   }
   return false;
}

/* sweetgl: does a bound fragment sampler read a framebuffer texture? Then an
 * opaque tile must not drop the bin's earlier commands (it may read them). */
uint64_t sweet_lp_resets_kept;
bool sweet_lp_fb_sampled(const struct lp_setup_context *setup);
bool
sweet_lp_fb_sampled(const struct lp_setup_context *setup)
{
   for (unsigned i = 0; i < setup->fs.current_tex_num && i < PIPE_MAX_SHADER_SAMPLER_VIEWS; i++) {
      if (setup->fs.current_tex[i] && sweet_lp_setup_fb_has(setup, setup->fs.current_tex[i]))
         return true;
   }
   return false;
}

/* sweetgl: does the scene being built, or the framebuffer it draws to, write texture? */
bool sweet_lp_setup_writes(struct lp_setup_context *setup, const struct pipe_resource *texture);
bool
sweet_lp_setup_writes(struct lp_setup_context *setup, const struct pipe_resource *texture)
{
   for (unsigned i = 0; i < setup->fb.nr_cbufs; i++) {
      if (setup->fb.cbufs[i].texture == texture)
         return true;
   }
   if (setup->fb.zsbuf.texture == texture)
      return true;
   if (setup->scene) {
      mtx_lock(&setup->scene->mutex);
      unsigned ref = lp_scene_is_resource_referenced(setup->scene, texture);
      mtx_unlock(&setup->scene->mutex);
      if (ref & LP_REFERENCED_FOR_WRITE)
         return true;
   }
   return false;
}


/**
 * Called by vbuf code when we're about to draw something.'''))
patch('lp_setup.c', SETUP_EDITS)

# ---------------------------------------------------------------- lp_flush.c
FLUSH_EDITS = [
    ('''void
llvmpipe_finish(struct pipe_context *pipe,
                const char *reason)
{
   struct pipe_fence_handle *fence = NULL;
   llvmpipe_flush(pipe, &fence, reason);
   if (fence) {
      pipe->screen->fence_finish(pipe->screen, NULL, fence,
                                 OS_TIMEOUT_INFINITE);
      pipe->screen->fence_reference(pipe->screen, &fence, NULL);
   }
}
''', '''#include <mach/mach_time.h>
#include <stdlib.h>
#include <stdio.h>
struct swp_reason { const char *r; uint64_t n, ticks; };
extern struct swp_reason sweet_lp_flushes[], sweet_lp_waits[];
bool sweet_lp_sampler_flush(struct pipe_context *pipe, struct pipe_resource *resource);
void sweet_lp_waits_report(void (*lg)(const char *, ...), double ms);
extern void sweet_lp_reason_add(struct swp_reason *t, const char *r, uint64_t ticks);
extern bool sweet_lp_setup_writes(struct lp_setup_context *setup, const struct pipe_resource *texture);

void
llvmpipe_finish(struct pipe_context *pipe,
                const char *reason)
{
   struct pipe_fence_handle *fence = NULL;
   uint64_t t0 = mach_absolute_time();
   llvmpipe_flush(pipe, &fence, reason);
   if (fence) {
      pipe->screen->fence_finish(pipe->screen, NULL, fence,
                                 OS_TIMEOUT_INFINITE);
      pipe->screen->fence_reference(pipe->screen, &fence, NULL);
   }
   sweet_lp_reason_add(sweet_lp_waits, reason ? reason : "?", mach_absolute_time() - t0);
}

/* sweetgl: lp-sweet.py part 1 (see there) */
static uint64_t swp_nw_skip, swp_nw_flush, swp_nw_other, swp_nw_stock;

bool sweet_lp_nowait(void);
uint64_t sweet_lp_barriers_nowait;
bool
sweet_lp_nowait(void)
{
   static int on = -1;
   if (on < 0) {
      const char *e = getenv("SGLLPWAIT");
      on = !(e && e[0] == '1');
   }
   return on;
}

/* set by eagl.c around a framebuffer-fetch draw */
int sweet_lp_fbfetch_draw;
static uint64_t swp_nw_same;
extern bool sweet_lp_setup_fb_has(const struct lp_setup_context *setup, const struct pipe_resource *texture);
extern uint64_t sweet_lp_resets_kept;

bool
sweet_lp_sampler_flush(struct pipe_context *pipe, struct pipe_resource *resource)
{
   if (!sweet_lp_nowait()) {
      swp_nw_stock++;
      return false;
   }
   struct llvmpipe_screen *lp_screen = llvmpipe_screen(pipe->screen);
   unsigned other = 0;
   mtx_lock(&lp_screen->ctx_mutex);
   list_for_each_entry(struct llvmpipe_context, ctx, &lp_screen->ctx_list, list) {
      if ((struct pipe_context *)ctx != pipe)
         other |= llvmpipe_is_resource_referenced((struct pipe_context *)ctx, resource, 0);
   }
   mtx_unlock(&lp_screen->ctx_mutex);
   if (other & LP_REFERENCED_FOR_WRITE) {
      swp_nw_other++;
      return false;     /* the caller does the stock finish */
   }
   if (!(llvmpipe_is_resource_referenced(pipe, resource, 0) & LP_REFERENCED_FOR_WRITE))
      return true;
   if (sweet_lp_fbfetch_draw && sweet_lp_setup_fb_has(llvmpipe_context(pipe)->setup, resource)) {
      swp_nw_same++;    /* framebuffer fetch: same scene (see lp-sweet.py) */
      return true;
   }
   if (sweet_lp_setup_writes(llvmpipe_context(pipe)->setup, resource)) {
      llvmpipe_flush(pipe, NULL, "sampler_view (no wait)");
      swp_nw_flush++;
   } else {
      swp_nw_skip++;    /* only queued scenes write it: they rasterize first */
   }
   return true;
}

void
sweet_lp_waits_report(void (*lg)(const char *, ...), double ms)
{
   static uint64_t pf[24], pw[24], pwt[24], ps[4];
   char l[480];
   int o = snprintf(l, sizeof(l), "sweetgl: lp flushes by reason:");
   for (unsigned i = 0; i < 24 && sweet_lp_flushes[i].r; i++) {
      uint64_t d = sweet_lp_flushes[i].n - pf[i];
      pf[i] = sweet_lp_flushes[i].n;
      if (d && o < (int)sizeof(l) - 48)
         o += snprintf(l + o, sizeof(l) - o, " %s %llu", sweet_lp_flushes[i].r, (unsigned long long)d);
   }
   lg("%s", l);
   o = snprintf(l, sizeof(l), "sweetgl: lp GL-thread waits (n/ms):");
   for (unsigned i = 0; i < 24 && sweet_lp_waits[i].r; i++) {
      uint64_t d = sweet_lp_waits[i].n - pw[i], dt = sweet_lp_waits[i].ticks - pwt[i];
      pw[i] = sweet_lp_waits[i].n;
      pwt[i] = sweet_lp_waits[i].ticks;
      if (d && o < (int)sizeof(l) - 48)
         o += snprintf(l + o, sizeof(l) - o, " %s %llu/%.0f", sweet_lp_waits[i].r, (unsigned long long)d, dt * ms);
   }
   static uint64_t pb, pss, prk;
   o += snprintf(l + o, sizeof(l) - o, "; texture barriers without wait %llu; fbfetch binds in the same scene %llu, opaque-tile resets kept %llu",
                 (unsigned long long)(sweet_lp_barriers_nowait - pb), (unsigned long long)(swp_nw_same - pss),
                 (unsigned long long)(sweet_lp_resets_kept - prk));
   pb = sweet_lp_barriers_nowait;
   pss = swp_nw_same;
   prk = sweet_lp_resets_kept;
   o += snprintf(l + o, sizeof(l) - o, "; sampler binds without wait: skipped %llu, flushed %llu, other ctx %llu, stock %llu",
                 (unsigned long long)(swp_nw_skip - ps[0]), (unsigned long long)(swp_nw_flush - ps[1]),
                 (unsigned long long)(swp_nw_other - ps[2]), (unsigned long long)(swp_nw_stock - ps[3]));
   ps[0] = swp_nw_skip;
   ps[1] = swp_nw_flush;
   ps[2] = swp_nw_other;
   ps[3] = swp_nw_stock;
   lg("%s", l);
}
'''),
]
patch('lp_flush.c', FLUSH_EDITS)

# ---------------------------------------------------------------- lp_context.c
patch('lp_context.c', [
    ('''static void
llvmpipe_texture_barrier(struct pipe_context *pipe, unsigned flags)
{
   llvmpipe_finish(pipe, "barrier");
}
''', '''bool sweet_lp_nowait(void);   /* sweetgl: lp_flush.c */
extern uint64_t sweet_lp_barriers_nowait;

static void
llvmpipe_texture_barrier(struct pipe_context *pipe, unsigned flags)
{
   /* sweetgl: scenes rasterize in order, so a later draw reading this
    * context's earlier output only needs the current scene sent (lp-sweet.py) */
   if (sweet_lp_nowait()) {
      llvmpipe_flush(pipe, NULL, "barrier (no wait)");
      sweet_lp_barriers_nowait++;
      return;
   }
   llvmpipe_finish(pipe, "barrier");
}
'''),
])

# ---------------------------------------------------------------- lp_state_fs.h / lp_draw_arrays.c
# Which of eagl.c's fast-path kinds (fastpath.h FPK_*) a fragment shader
# draws, and how many of its draws the fast path would take: set from
# sweet_lp_draw_kind at each draw, printed with the profile's variants.
patch('lp_state_fs.h', [
    ('''   /* For debugging/profiling purposes */
   unsigned variant_key_size;
   unsigned no;
''', '''   /* For debugging/profiling purposes */
   unsigned variant_key_size;
   unsigned no;
   unsigned sweet_kind;              /* sweetgl: eagl.c FPK_* of the last draw */
   uint64_t sweet_draws, sweet_qual; /* sweetgl: draws, and those the fast path takes */
'''),
])
patch('lp_draw_arrays.c', [
    ('''#include "lp_query.h"''', '''#include "lp_query.h"
#include "lp_state_fs.h"
/* sweetgl: set by eagl.c before each draw: FPK_* kind | 0x100 if the fast path qualifies */
extern unsigned sweet_lp_draw_kind;
unsigned sweet_lp_draw_kind;'''),
    ('''   if (lp->dirty)
      llvmpipe_update_derived(lp);
''', '''   if (lp->dirty)
      llvmpipe_update_derived(lp);

   if (lp->fs) {
      lp->fs->sweet_kind = sweet_lp_draw_kind & 0xff;
      lp->fs->sweet_draws++;
      lp->fs->sweet_qual += (sweet_lp_draw_kind >> 8) & 1;
   }
'''),
])

# ---------------------------------------------------------------- lp_setup_rect.c
patch('lp_setup_rect.c', [
    ('''#include "lp_state_setup.h"''', '''#include "lp_state_setup.h"
bool sweet_lp_fb_sampled(const struct lp_setup_context *setup);   /* sweetgl: lp_setup.c */
extern uint64_t sweet_lp_resets_kept;'''),
    ('''      if (!scene->fb.zsbuf.texture && scene->fb_max_layer == 0 &&
          !scene->had_queries) {''', '''      bool sweet_keep = sweet_lp_fb_sampled(setup);
      sweet_lp_resets_kept += sweet_keep;
      if (!scene->fb.zsbuf.texture && scene->fb_max_layer == 0 &&
          !scene->had_queries && !sweet_keep) {'''),
])

# ---------------------------------------------------------------- lp_state_sampler.c
patch('lp_state_sampler.c', [
    ('''      if (view)
         llvmpipe_flush_resource(pipe, view->texture, 0, true, false, false, "sampler_view");
''', '''      if (view && !(shader == MESA_SHADER_FRAGMENT && sweet_lp_sampler_flush(pipe, view->texture)))
         llvmpipe_flush_resource(pipe, view->texture, 0, true, false, false, "sampler_view");
'''),
    ('''#include "lp_context.h"''', '''#include "lp_context.h"
bool sweet_lp_sampler_flush(struct pipe_context *pipe, struct pipe_resource *resource);   /* sweetgl: lp_flush.c */'''),
])

# ---------------------------------------------------------------- part 3: lp-fast.py
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib
lpfast = importlib.import_module('lp-fast')
lpfast.run(M, LP, lambda name, edits: patch(name, edits, 'lp-fast'), os.path.dirname(os.path.abspath(__file__)))
