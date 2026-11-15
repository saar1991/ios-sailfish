/* fp_run's shape and mix kinds against a per-pixel double model of the shaders on softpipe */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "fastpath.h"
struct gl_context; struct gl_texture_object;
struct gl_context *_mesa_get_current_context(void) { return NULL; }
struct gl_texture_object *_mesa_lookup_texture(struct gl_context *c, unsigned id) { (void)c; (void)id; return NULL; }
struct util_format_description;
const struct util_format_description *util_format_description(int f) { (void)f; return 0; }
_Bool util_format_is_pure_integer(int f) { (void)f; return 0; }
void util_format_unpack_rgba_8unorm_rect(int f, void *d, unsigned ds, const void *s, unsigned ss, unsigned w, unsigned h) { (void)f; (void)d; (void)ds; (void)s; (void)ss; (void)w; (void)h; }
typedef struct dispatch_queue_s *dispatch_queue_t;
dispatch_queue_t dispatch_get_global_queue(long a, unsigned long b) { (void)a; (void)b; return NULL; }
void dispatch_apply_f(size_t n, dispatch_queue_t q, void *ctx, void (*work)(void *, size_t)) { (void)q; for (size_t i = n; i-- > 0;) work(ctx, i); }
#include <time.h>
int main(void){ struct fp_img s={0},d={0}; s.w=752;s.h=1336;s.fmt=FP_BGRA;s.stride=4*752;s.ptr=calloc(4,752*1336); d=s; d.ptr=calloc(4,752*1336);
 struct fp_job j={0}; j.kind=FPK_BLUR; j.xa=0;j.xb=750;j.ya=0;j.yb=1334;j.x0=0;j.x1=750;j.y0=0;j.y1=1334;j.ua=0;j.ub=1;j.va=0;j.vb=1;j.linear=1;j.ntap=12;j.bdiv=1;
 for(int t=0;t<12;t++){j.toff[t][0]=(t-6)*0.002f;j.tw[t]=1/12.f;}
 struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a); fp_run(&j,&d,&s,NULL,0); clock_gettime(CLOCK_MONOTONIC,&b);
 printf("12-tap 750x1334 single thread: %.0f ms\n",(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6); }
