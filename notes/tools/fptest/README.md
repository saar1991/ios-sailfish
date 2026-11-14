# fastpath.c host test

Checks `fp_run` (tools/sweetgl/fastpath.c) against the loop of commit
14f179d (verified pixel-exact against softpipe on the phone) and against a
float model of softpipe's blending. `bench.c` times full-screen draws.

```
S=~/ios-sailfish/tools/sweetgl            # the shim sources
M=~/mesa                                  # Mesa e24dc5bd sources
G=~/mesa-gen                              # holds util/format/u_format_gen.h:
#   python3 $M/src/util/format/u_format_table.py $M/src/util/format/u_format.yaml --enums > $G/util/format/u_format_gen.h
git -C ~/ios-sailfish show 14f179d:tools/sweetgl/fastpath.c | sed -n '/^static inline void$/,$p' \
  | sed 's/\bfp_run\b/old_fp_run/; s/\bfp_read_rgba\b/old_fp_read_rgba/' > old_loop.inc
# stubs/ holds dispatch/dispatch.h (dispatch_apply_f, dispatch_get_global_queue)
clang -O2 -std=gnu11 -DHAVE_PTHREAD -DHAVE_STRUCT_TIMESPEC -DHAVE_TIMESPEC_GET -DUTIL_ARCH_LITTLE_ENDIAN=1 \
  -DUTIL_ARCH_BIG_ENDIAN=0 -Istubs -I$S/include -I$M/include -I$M/src -I$M/src/mesa -I$M/src/mapi \
  -I$M/src/gallium/include -I$M/src/gallium/auxiliary -I$G -c $S/fastpath.c -o fastpath.o
gcc -O2 -std=gnu11 -I$S harness.c fastpath.o -o harness -lm && ./harness   # cases 6000 fails 0
gcc -O2 -std=gnu11 -I$S bench.c fastpath.o -o bench -lm && ./bench
```
