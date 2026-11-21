/*
 * EAGL on sweetgl: QuartzCore's GLES renderer (CA::OGL::GLESContext) on
 * Mesa instead of Apple's Metal-backed GLEngine.
 *
 * CA uses EAGLContext only through initWithAPI:properties:, API,
 * setParameter:to:, getMacroContextPrivate, texImageIOSurface:...,
 * attachImage:toCoreSurface:invertedRender:, +setCurrentContext: and
 * +currentContext; every GL call goes through the GLI dispatch table it gets
 * from getMacroContextPrivate ({rctx, disp[1036]}, layout from GLEngine,
 * table generated into out/dispatch.c). Those methods are replaced here for
 * contexts we create; the original EAGL code still runs for everything else.
 *
 * IOSurfaces become texture storage the EGL way: imported as a GEM bo
 * (SweetGPU selector 12), wrapped in a pipe_resource, handed to
 * glEGLImageTargetTexture2DOES through the frontend's get_egl_image.
 */
#include <objc/NSObjCRuntime.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/vm_page_size.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>

#include "eagl.h"

#include "pipe/p_screen.h"
#include "pipe/p_state.h"
#include "frontend/api.h"
#include "frontend/winsys_handle.h"
#include "util/u_inlines.h"
#include "drm-uapi/drm_fourcc.h"

#include "fastpath.h"
#include "sweet_drm.h"
#include "sweetgl.h"

/* not in the GL bridge (ES-only): Mesa's implementation */
void _mesa_EGLImageTargetTexture2DOES(GLenum target, GLeglImageOES image);

struct sgl_context {
	void *rctx;                     /* = this; CA reads {rctx, disp[]} from macro */
	void *disp[SGL_SLOTS];
	id eagl;
	unsigned api;
	struct sweetgl_ctx *ctx;
};

struct sgl_image {
	struct pipe_resource *res;
	enum pipe_format format;
	unsigned internalformat;
};

__thread void *sgl_current;
int sgl_check_errors;
static int trace_left;          /* SWEETGL_TRACE=1: this many trace lines */
static bool dump_shaders;       /* SWEETGL_TRACE=1: every shader source, no limit */

#define TRACE(...) do { if (trace_left > 0) { trace_left--; sweet_drm_log("sweetgl: trace " __VA_ARGS__); } } while (0)

/* fast paths (defined further down): the shader classifier runs in ShaderSource */
static void fast_shader(GLuint shader, const char *src);


/* ponytail: linear table of contexts; backboardd makes one or two */
#define MAX_CTX 32
static struct sgl_context *ctxs[MAX_CTX];
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;

static IMP orig_init, orig_dealloc, orig_api, orig_set_param, orig_macro, orig_tex_iosurface, orig_attach,
    orig_set_current, orig_current;

static struct sgl_context *
lookup(id eagl)
{
	struct sgl_context *c = NULL;
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX && c == NULL && eagl != NULL; i++) {
		if (ctxs[i] && ctxs[i]->eagl == eagl) {
			c = ctxs[i];
		}
	}
	pthread_mutex_unlock(&ctx_lock);
	return c;
}

void
sgl_bind(void *rctx)
{
	struct sgl_context *c = rctx;
	sweetgl_make_current(c ? c->ctx : NULL);
	sgl_current = c;
}

void
sgl_unsupported(int slot, const char *name)
{
	static unsigned char seen[SGL_SLOTS];
	if (slot >= 0 && slot < SGL_SLOTS && !seen[slot]) {
		seen[slot] = 1;
		sweet_drm_log("sweetgl: unsupported GLI slot %d (%s)", slot, name);
	}
}

void
sgl_post(int slot)
{
	static unsigned char logged[SGL_SLOTS][8];     /* per slot and error, the first 3 */
	static bool used[SGL_SLOTS];
	if (!used[slot]) {
		used[slot] = true;
		sweet_drm_log("sweetgl: first call of slot %d %s", slot, sgl_slot_names[slot]);
	}
	GLenum e = glGetError();
	unsigned k = e - GL_INVALID_ENUM;
	if (e != GL_NO_ERROR && (k >= 8 || logged[slot][k] < 200)) {
		if (k < 8) {
			logged[slot][k]++;
		}
		sweet_drm_log("sweetgl: GL error %#x after slot %d %s", e, slot, sgl_slot_names[slot]);
	}
}

/* ---- hand-written slots ---- */

#define H(o) ((GLuint)(uintptr_t)(o))

void
sgl_DeleteObjectARB(void *rctx, GLhandleARB obj)
{
	SGL_CUR(rctx);
	if (glIsProgram(H(obj))) {
		glDeleteProgram(H(obj));
	} else {
		glDeleteShader(H(obj));
	}
}

void
sgl_GetObjectParameterivARB(void *rctx, GLhandleARB obj, GLenum pname, GLint *params)
{
	SGL_CUR(rctx);
	if (pname == GL_OBJECT_TYPE_ARB) {
		*params = glIsProgram(H(obj)) ? GL_PROGRAM_OBJECT_ARB : GL_SHADER_OBJECT_ARB;
	} else if (glIsProgram(H(obj))) {
		glGetProgramiv(H(obj), pname, params);
	} else {
		glGetShaderiv(H(obj), pname == GL_OBJECT_SUBTYPE_ARB ? GL_SHADER_TYPE : pname, params);
	}
}

void
sgl_GetInfoLogARB(void *rctx, GLhandleARB obj, GLsizei max, GLsizei *length, GLcharARB *log)
{
	SGL_CUR(rctx);
	if (glIsProgram(H(obj))) {
		glGetProgramInfoLog(H(obj), max, length, log);
	} else {
		glGetShaderInfoLog(H(obj), max, length, log);
	}
	if (log && max > 0 && log[0]) {
		sweet_drm_log("sweetgl: info log %u: %.300s", H(obj), log);
	}
}

void
sgl_GetAttachedObjectsARB(void *rctx, GLhandleARB prog, GLsizei max, GLsizei *count, GLhandleARB *obj)
{
	SGL_CUR(rctx);
	GLuint s[16];
	GLsizei n = 0;
	glGetAttachedShaders(H(prog), max < 16 ? max : 16, &n, s);
	for (GLsizei i = 0; i < n; i++) {
		obj[i] = (GLhandleARB)(uintptr_t)s[i];
	}
	if (count) {
		*count = n;
	}
}

GLhandleARB
sgl_GetHandleARB(void *rctx, GLenum pname)
{
	(void)rctx;
	(void)pname;
	return 0;
}

void
sgl_DiscardFramebufferEXT(void *rctx, GLenum target, GLsizei n, const GLenum *attachments)
{
	SGL_CUR(rctx);
	glInvalidateFramebuffer(target, n, attachments);
}

void
sgl_GetShaderPrecisionFormatOES(void *rctx, GLenum shader, GLenum precision, GLint *range, GLint *prec)
{
	SGL_CUR(rctx);
	glGetShaderPrecisionFormat(shader, precision, range, prec);
}

void
sgl_ReleaseShaderCompilerOES(void *rctx)
{
	(void)rctx;
}

void
sgl_ShaderBinaryOES(void *rctx, GLsizei n, const GLuint *shaders, GLenum format, const void *binary,
    GLsizei length)
{
	SGL_CUR(rctx);
	glShaderBinary(n, shaders, format, binary, length);
}

void
sgl_GetIntegerIndexedv(void *rctx, GLenum pname, GLuint index, GLint *data)
{
	SGL_CUR(rctx);
	glGetIntegeri_v(pname, index, data);
}

const GLubyte *
sgl_GetString(void *rctx, GLenum name)
{
	SGL_CUR(rctx);
	const GLubyte *s = glGetString(name);
	/*
	 * Without framebuffer fetch CA's GL renderer drops every layer with a
	 * blend-mode compositing filter (plusD/plusL, colorDodge, multiply,
	 * overlay...: CC toggle circles, App Library plates, Reduce
	 * Transparency backgrounds). Emulated below (fbfetch_*).
	 */
	if (name == GL_EXTENSIONS && s != NULL) {
		static char *ext;
		if (ext == NULL && asprintf(&ext, "%s GL_EXT_shader_framebuffer_fetch", s) < 0) {
			ext = NULL;
		}
		return ext != NULL ? (const GLubyte *)ext : s;
	}
	if (s == NULL) {
		sweet_drm_log("sweetgl: glGetString(%#x) = NULL (error %#x)", name, glGetError());
	}
	return s;
}

/*
 * GL_TEXTURE_RECTANGLE (Apple's ES has it, Mesa's ES does not): such
 * textures are 2D ones (SGL_TGT), and shaders sampling them as
 * sampler2DRect get texel coordinates scaled by sgl_rect_scale, set before
 * each draw to 1/size of the texture on that sampler's unit.
 * ponytail: one rect sampler per shader (CA's), small fixed tables.
 */
#define RECT_MAX 64
static struct { GLuint shader; char sampler[32]; } rect_shaders[RECT_MAX];
static struct { GLuint program; GLint scale, sampler; } rect_programs[RECT_MAX];
static int n_rect_shaders, n_rect_programs;

/* level-0 sizes of 2D textures as created through us (rect scales without GL queries) */
#define TEXSZ_MAX 8192
static uint16_t tex_w[TEXSZ_MAX], tex_h[TEXSZ_MAX];
/* the IOSurface last bound to each texture (frame listings, SGLFRAMES) */
static uint32_t tex_sid[TEXSZ_MAX];

static void
tex_size_bound(GLenum target, GLint level, GLsizei w, GLsizei h)
{
	GLint tex = 0;
	if (level != 0 || (target != GL_TEXTURE_2D && target != 0x84F5)) {
		return;
	}
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	if (tex > 0 && tex < TEXSZ_MAX) {
		tex_w[tex] = (uint16_t)w;
		tex_h[tex] = (uint16_t)h;
	}
}

/*
 * GL_EXT_shader_framebuffer_fetch on a GPU without it: gl_LastFragData[0]
 * becomes a read of the framebuffer's own texture at the fragment, bound
 * to FBFETCH_UNIT before each draw after a flush (texture barrier). Reads
 * see the framebuffer as it was before the draw, not earlier primitives of
 * the same draw. ponytail: CA's draws are quads that do not overlap themselves.
 */
#define FBFETCH_UNIT 15
#define FBFETCH_MAX 512
static GLuint fbfetch_shaders[FBFETCH_MAX];
static int n_fbfetch_shaders;
static const char fbfetch_decl[] =
    "uniform lowp sampler2D sgl_fb;\n"
    "uniform highp vec2 sgl_fb_scale;\n";
static const char fbfetch_read[] = "texture2D(sgl_fb, gl_FragCoord.xy * sgl_fb_scale)";

/* src without the extension, sgl_fb declared after the directives, reads replaced (src is freed) */
static char *
fbfetch_rewrite(char *src)
{
	char *ext = strstr(src, "#extension GL_EXT_shader_framebuffer_fetch");
	for (char *e = ext; e != NULL && *e != '\n' && *e != 0; e++) {
		*e = ' ';
	}
	/* after the leading preprocessor lines */
	size_t at = 0;
	while (src[at] == '#' || src[at] == ' ' || src[at] == '\n') {
		char *nl = strchr(src + at, '\n');
		if (nl == NULL) {
			break;
		}
		at = (size_t)(nl - src + 1);
	}
	size_t reads = 0;
	for (char *r = strstr(src, "gl_LastFragData[0]"); r != NULL; r = strstr(r + 1, "gl_LastFragData[0]")) {
		reads++;
	}
	size_t n = strlen(src);
	char *out = malloc(n + sizeof(fbfetch_decl) + reads * sizeof(fbfetch_read) + 1), *o = out;
	memcpy(o, src, at);
	o += at;
	memcpy(o, fbfetch_decl, sizeof(fbfetch_decl) - 1);
	o += sizeof(fbfetch_decl) - 1;
	for (const char *p = src + at; *p != 0;) {
		if (strncmp(p, "gl_LastFragData[0]", 18) == 0) {
			memcpy(o, fbfetch_read, sizeof(fbfetch_read) - 1);
			o += sizeof(fbfetch_read) - 1;
			p += 18;
		} else {
			*o++ = *p++;
		}
	}
	*o = 0;
	free(src);
	return out;
}

/*
 * Texture functions CA's GL renderer has no tex_fragment_shaders entry for
 * (create_shader fails, the draw is dropped); sgl_install_texfns() points
 * the entries at these calls and the functions are added here. Both use
 * color_matrix[0..3] (input r, g, b, a columns) + [4] (bias).
 * 0x2d (render_vibrant_color_matrix: Control Center off circles, App
 * Library plates): the SW renderer's tex_vibrant_color_matrix, i.e. the
 * destination through the matrix (unpremultiplied) mixed in by the
 * source's alpha.
 * 0x14 (color_matrix_function for a matrix filter without its premultiply
 * flag, next to 0x13 colorMatrix4x4Bias): the matrix on the premultiplied
 * colour as is (CA asks for the full rect then: the bias lights up
 * transparent pixels). SW has neither (no-op there).
 */
static const char vcm_func[] =
    "lowp vec4 sgl_vcm(lowp vec4 s) {\n"
    "  lowp vec4 d = gl_LastFragData[0];\n"
    "  lowp vec4 u = vec4(d.rgb / max(d.a, .005), d.a);\n"
    "  u = clamp(u.r * color_matrix[0] + u.g * color_matrix[1] + u.b * color_matrix[2]\n"
    "    + u.a * color_matrix[3] + color_matrix[4], 0., 1.);\n"
    "  u.rgb *= u.a;\n"
    "  return mix(d, d.a > 0. ? u : vec4(0.), s.a);\n"
    "}\n";
static const char cm4_func[] =
    "lowp vec4 sgl_cm4(lowp vec4 s) {\n"
    "  lowp vec4 u = clamp(s.r * color_matrix[0] + s.g * color_matrix[1] + s.b * color_matrix[2]\n"
    "    + s.a * color_matrix[3] + color_matrix[4], 0., 1.);\n"
    "  return vec4(min(u.rgb, u.a), u.a);\n"
    "}\n";

/*
 * 0x26/0x27 (Context::variable_masked_blur_surface, CAFilter variableBlur):
 * CA copies the source into a surface S, renders a mip chain of S with
 * 0x26 passes and blurs by sampling S's mips per the mask in 0x27. GLES
 * surfaces have one level (set_surface attaches level 0), so each 0x26
 * pass would draw a half-size copy into S itself: they draw nothing here
 * (sgl_nop). 0x27 blurs S in one pass instead: a 24-tap golden-angle disc,
 * Gaussian weighted, radius = mask alpha (1 - alpha if minmax_dir.y, the
 * BlurParams 0x10 flag) times minmax_dir.x (BlurParams radius * 2.6, in
 * pixels) / 2, tap spacing from the texcoord derivatives (S is about
 * destination sized). ponytail: one pass, cost grows with radius only via
 * texture cache misses; a downsampled pre-pass if large blurs get slow.
 */
static const char nop_func[] =
    "lowp vec4 sgl_nop() {\n"
    "  discard;\n"
    "  return vec4(0.);\n"
    "}\n";
static const char vblur_func[] =
    "uniform highp vec4 minmax_dir;\n"
    "lowp vec4 sgl_vblur(lowp vec4 m) {\n"
    "  highp vec2 tc = texcoord0_0;\n"
    "  highp vec2 px = vec2(length(vec2(dFdx(tc.x), dFdy(tc.x))), length(vec2(dFdx(tc.y), dFdy(tc.y))));\n"
    "  mediump float r = (minmax_dir.y > .5 ? 1. - m.a : m.a) * minmax_dir.x * .5;\n"
    "  mediump vec4 acc = texture2D(texture0, tc);\n"
    "  if (r < .5) {\n"
    "    return acc;\n"
    "  }\n"
    "  mediump float wsum = 1.;\n"
    "  for (int i = 1; i <= 24; i++) {\n"
    "    mediump float f = float(i) / 24.;\n"
    "    mediump float ang = float(i) * 2.3999632;\n"
    "    mediump float w = exp(-2. * f);\n"
    "    acc += w * texture2D(texture0, tc + vec2(cos(ang), sin(ang)) * (sqrt(f) * r) * px);\n"
    "    wsum += w;\n"
    "  }\n"
    "  return acc / wsum;\n"
    "}\n";

/*
 * The sgl_ texture functions src calls (and color_matrix if undeclared)
 * before main, extension directives after the leading ones (src is freed).
 */
static char *
texfn_rewrite(char *src)
{
	char *at = strstr(src, "void main");
	if (at == NULL) {
		return src;
	}
	bool cm = strstr(src, "sgl_vcm(") || strstr(src, "sgl_cm4(");
	const char *ext = strstr(src, "sgl_vblur(") ? "#extension GL_OES_standard_derivatives : enable\n" : "";
	const char *parts[5] = {
	    cm && !strstr(src, " color_matrix[5];") ? "uniform lowp vec4 color_matrix[5];\n" : "",
	    strstr(src, "sgl_vcm(") ? vcm_func : "", strstr(src, "sgl_cm4(") ? cm4_func : "",
	    strstr(src, "sgl_nop(") ? nop_func : "", strstr(src, "sgl_vblur(") ? vblur_func : ""};
	/* after the leading preprocessor lines */
	size_t top = 0;
	while (src[top] == '#' || src[top] == ' ' || src[top] == '\n') {
		char *nl = strchr(src + top, '\n');
		if (nl == NULL) {
			break;
		}
		top = (size_t)(nl - src + 1);
	}
	size_t n = strlen(src), pre = (size_t)(at - src), add = strlen(ext);
	for (int i = 0; i < 5; i++) {
		add += strlen(parts[i]);
	}
	char *out = malloc(n + add + 1), *o = out;
	memcpy(o, src, top);
	o += top;
	memcpy(o, ext, strlen(ext));
	o += strlen(ext);
	memcpy(o, src + top, pre - top);
	o += pre - top;
	for (int i = 0; i < 5; i++) {
		memcpy(o, parts[i], strlen(parts[i]));
		o += strlen(parts[i]);
	}
	memcpy(o, at, n - pre + 1);
	free(src);
	return out;
}

static const char rect_prelude[] =
    "#define sampler2DRect sampler2D\n"
    "#define texture2DRect(s, c) texture2D(s, (c) * sgl_rect_scale)\n"
    "uniform highp vec2 sgl_rect_scale;\n";

void
sgl_ShaderSourceARB(void *rctx, GLhandleARB shader, GLsizei count, const GLcharARB **string, const GLint *length)
{
	SGL_CUR(rctx);
	size_t n = 0;
	for (GLsizei i = 0; i < count; i++) {
		n += length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
	}
	char *src = malloc(n + sizeof(rect_prelude) + 1), *p = src;
	for (GLsizei i = 0; i < count; i++) {
		size_t l = length && length[i] >= 0 ? (size_t)length[i] : strlen(string[i]);
		memcpy(p, string[i], l);
		p += l;
	}
	*p = 0;
	/*
	 * CA's inverted-mask shader uses coord_arg (clamp bounds) without
	 * declaring it; Apple's compiler takes that, Mesa's does not.
	 */
	const char *extra = strstr(src, "coord_arg") && !strstr(src, "vec4 coord_arg") ?
	    "uniform highp vec4 coord_arg;\n" : "";
	size_t extra_len = strlen(extra);
	if (extra_len) {
		src = realloc(src, n + sizeof(rect_prelude) + extra_len + 1);
		size_t at = strncmp(src, "#version", 8) == 0 && strchr(src, '\n') ? (size_t)(strchr(src, '\n') - src + 1) : 0;
		memmove(src + at + extra_len, src + at, n - at + 1);
		memcpy(src + at, extra, extra_len);
		n += extra_len;
	}
	const char *rect = strstr(src, "sampler2DRect");
	if (rect != NULL && n_rect_shaders < RECT_MAX) {
		const char *name = rect + strlen("sampler2DRect");
		while (*name == ' ' || *name == '\t') {
			name++;
		}
		size_t l = strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
		rect_shaders[n_rect_shaders].shader = H(shader);
		snprintf(rect_shaders[n_rect_shaders].sampler, sizeof(rect_shaders[0].sampler), "%.*s", (int)l, name);
		n_rect_shaders++;
		/* after a leading #version line, if any */
		size_t at = strncmp(src, "#version", 8) == 0 && strchr(src, '\n') ? (size_t)(strchr(src, '\n') - src + 1) : 0;
		memmove(src + at + sizeof(rect_prelude) - 1, src + at, n - at + 1);
		memcpy(src + at, rect_prelude, sizeof(rect_prelude) - 1);
	}
	if (strstr(src, "sgl_vcm(") != NULL || strstr(src, "sgl_cm4(") != NULL || strstr(src, "sgl_nop(") != NULL ||
	    strstr(src, "sgl_vblur(") != NULL) {
		src = texfn_rewrite(src);
	}
	if (strstr(src, "GL_EXT_shader_framebuffer_fetch") != NULL || strstr(src, "gl_LastFragData") != NULL) {
		src = fbfetch_rewrite(src);
		if (n_fbfetch_shaders < FBFETCH_MAX) {
			fbfetch_shaders[n_fbfetch_shaders++] = H(shader);
		}
	}
	if (dump_shaders) {
		/* whole source, in 400-byte pieces (log lines are 512 bytes) */
		sweet_drm_log("sweetgl: shader %u source (%zu bytes):", H(shader), strlen(src));
		for (size_t i = 0, l = strlen(src); i < l; i += 400) {
			sweet_drm_log("sweetgl:   | %.400s", src + i);
		}
	}
	fast_shader(H(shader), src);
	const GLchar *one = src;
	glShaderSource(H(shader), 1, &one, NULL);
	free(src);
}

static void
rect_link(GLuint program)
{
	GLuint attached[8];
	GLsizei na = 0;
	glGetAttachedShaders(program, 8, &na, attached);
	for (GLsizei a = 0; a < na; a++) {
		for (int i = 0; i < n_rect_shaders; i++) {
			if (rect_shaders[i].shader == attached[a] && n_rect_programs < RECT_MAX) {
				rect_programs[n_rect_programs].program = program;
				rect_programs[n_rect_programs].scale = glGetUniformLocation(program, "sgl_rect_scale");
				rect_programs[n_rect_programs].sampler = glGetUniformLocation(program, rect_shaders[i].sampler);
				n_rect_programs++;
				return;
			}
		}
	}
}

/* per unit: the 2D and rect bindings CA made, and the one GL holds */
#define UNITS 16
static GLuint unit_2d[UNITS], unit_rect[UNITS], unit_held[UNITS];

static int
active_unit(void)
{
	GLint active = GL_TEXTURE0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	return (active - GL_TEXTURE0) & (UNITS - 1);
}

GLenum
sgl_tgt(GLenum target)
{
	int u = active_unit();
	GLuint want = target == GL_TEXTURE_2D ? unit_2d[u] : unit_rect[u];
	if (unit_held[u] != want) {
		glBindTexture(GL_TEXTURE_2D, want);
		unit_held[u] = want;
	}
	return GL_TEXTURE_2D;
}

/*
 * Textures holding a copy of a surface (copy_surface): copied again before
 * a draw when the surface's seed moved (its writer unlocked it since).
 * ponytail: one copy per texture name below TEXSZ_MAX.
 */
struct copied {
	IOSurfaceRef surface;
	uint32_t seed;
	GLenum format, type;
};
static struct copied copies[TEXSZ_MAX];
/* texture swizzle of the bound texture: identity, or A,B,G,R bytes read as RGBA */
static uint8_t tex_argb[TEXSZ_MAX];
static bool copy_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, size_t w, size_t h,
    size_t pitch, const char *why);
static void set_swizzle(GLenum target, bool argb);

static void
copy_forget(GLuint tex)
{
	if (tex < TEXSZ_MAX && copies[tex].surface != NULL) {
		CFRelease(copies[tex].surface);
		copies[tex].surface = NULL;
	}
}

static void
copy_refresh(int u, int *active)
{
	GLuint tex = unit_held[u];
	struct copied *c = tex < TEXSZ_MAX ? &copies[tex] : NULL;
	if (c == NULL || c->surface == NULL || IOSurfaceGetSeed(c->surface) == c->seed) {
		return;
	}
	if (*active < 0) {
		*active = active_unit();
	}
	glActiveTexture(GL_TEXTURE0 + u);
	IOSurfaceRef s = c->surface;
	copy_surface(s, GL_TEXTURE_2D, c->format, c->type, IOSurfaceGetWidth(s), IOSurfaceGetHeight(s),
	    IOSurfaceGetBytesPerRow(s), "refresh");
}

extern void _mesa_TextureBarrierNV(void);

/*
 * CA reads the destination by sampling the texture it is rendering into
 * (Apple's GPUs return what memory held when the pass began). A flush
 * first makes the earlier draws visible to the sampler; without it the
 * read came back empty (App Library pods, CC tiles, vibrant glyphs).
 * ponytail: sampler units per program read once (CA sets them at link).
 */
#define PROG_MAX 4096
static uint16_t prog_units[PROG_MAX];   /* bit u: a sampler reads unit u; bit 15: known */

static void
feedback_barrier(GLint prog)
{
	GLint fb = 0, att = 0, type = 0;
	if (prog <= 0 || prog >= PROG_MAX) {
		return;
	}
	if (!(prog_units[prog] & 0x8000)) {
		uint16_t m = 0x8000;
		GLint n = 0;
		glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
		for (GLint i = 0; i < n; i++) {
			char name[64];
			GLint size = 0, unit = 0;
			GLenum t = 0;
			glGetActiveUniform(prog, i, sizeof(name), NULL, &size, &t, name);
			if (t == GL_SAMPLER_2D) {
				glGetUniformiv(prog, glGetUniformLocation(prog, name), &unit);
				m |= unit >= 0 && unit < 15 ? 1u << unit : 0;
			}
		}
		prog_units[prog] = m;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	if (fb == 0) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	if (type != GL_TEXTURE) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att);
	for (int u = 0; u < 15; u++) {
		if ((prog_units[prog] & 1u << u) && unit_held[u] == (GLuint)att) {
			_mesa_TextureBarrierNV();
			return;
		}
	}
}

/* sampler and scale locations + 1 of programs with an fbfetch shader (0: none) */
static GLint fbfetch_sampler[PROG_MAX], fbfetch_scale[PROG_MAX];

static void
fbfetch_link(GLuint program)
{
	GLuint attached[8];
	GLsizei na = 0;
	if (program >= PROG_MAX) {
		return;
	}
	fbfetch_sampler[program] = fbfetch_scale[program] = 0;
	glGetAttachedShaders(program, 8, &na, attached);
	for (GLsizei a = 0; a < na; a++) {
		for (int i = 0; i < n_fbfetch_shaders; i++) {
			if (fbfetch_shaders[i] == attached[a]) {
				fbfetch_sampler[program] = glGetUniformLocation(program, "sgl_fb") + 1;
				fbfetch_scale[program] = glGetUniformLocation(program, "sgl_fb_scale") + 1;
				return;
			}
		}
	}
}

/*
 * llvmpipe: a framebuffer-fetch draw can stay in the scene being built
 * (lp-sweet.py): each fragment reads only its own pixel, llvmpipe renders
 * straight into the texture and runs a tile's commands in order, so the
 * earlier draws are already in memory. No flush, no barrier; Mesa is told
 * through sweet_lp_fbfetch_draw while the draw runs. SGLFBSAME=0: flush
 * and barrier as before (needed for softpipe, which keeps a tile cache).
 */
#ifdef SWEETGL_LLVMPIPE
extern int sweet_lp_fbfetch_draw;
extern unsigned sweet_lp_draw_kind;   /* lp-sweet.py: FPK_* | 0x100 if the fast path qualifies */
#endif
static int fbfetch_now;   /* the next draw reads the framebuffer in the same scene */

static int
fbfetch_same_scene(void)
{
#ifdef SWEETGL_LLVMPIPE
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("SGLFBSAME"), *d = getenv("GALLIUM_DRIVER");
		on = !(e != NULL && e[0] == '0') && !(d != NULL && strcmp(d, "softpipe") == 0);
		sweet_drm_log("sweetgl: framebuffer fetch in the same scene: %s", on ? "on" : "off");
	}
	return on;
#else
	return 0;
#endif
}

/* before a draw of an fbfetch program: the framebuffer's texture on FBFETCH_UNIT, flushed */
static void
fbfetch_bind(GLint prog)
{
	GLint fb = 0, type = 0, att = 0, w = 0, h = 0, active = GL_TEXTURE0;
	if (prog <= 0 || prog >= PROG_MAX || fbfetch_sampler[prog] == 0) {
		return;
	}
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	if (fb == 0) {
		return;
	}
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att);
	if (type != GL_TEXTURE || att <= 0) {
		return;
	}
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE0 + FBFETCH_UNIT);
	glBindTexture(GL_TEXTURE_2D, (GLuint)att);
	unit_held[FBFETCH_UNIT] = (GLuint)att;
	if (att < TEXSZ_MAX && tex_w[att] > 0) {
		w = tex_w[att];
		h = tex_h[att];
	} else {
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
	}
	glActiveTexture((GLenum)active);
	glUniform1i(fbfetch_sampler[prog] - 1, FBFETCH_UNIT);
	if (fbfetch_scale[prog] > 0) {
		glUniform2f(fbfetch_scale[prog] - 1, w > 0 ? 1.0f / (GLfloat)w : 1.0f, h > 0 ? 1.0f / (GLfloat)h : 1.0f);
	}
	/*
	 * Rasterize the draws softpipe still has queued before the barrier, so
	 * the framebuffer fetch reads them (test: rounded corners came out right
	 * only while the trace's glReadPixels forced this flush).
	 */
	if (fbfetch_same_scene()) {
		fbfetch_now = 1;
	} else {
		glFlush();
		_mesa_TextureBarrierNV();
	}
	/* SWEETGL_GLERR: an error here comes from the setup above, not from the app's draw */
	if (sgl_check_errors) {
		static int setup_logged;
		GLenum se = glGetError();
		if (se != GL_NO_ERROR && setup_logged++ < 50) {
			sweet_drm_log("sweetgl: fbfetch setup err %#x prog %d", se, prog);
		}
	}
}

/* before a draw: each unit holds what the program samples there, rect scale set */
static void
draw_prep(GLenum mode, GLsizei count)
{
	GLint prog = 0, unit = -1, w = 0, h = 0, scale = -1;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	for (int i = 0; i < n_rect_programs; i++) {
		if (rect_programs[i].program == (GLuint)prog) {
			scale = rect_programs[i].scale;
			unit = 0;
			if (rect_programs[i].sampler >= 0) {
				glGetUniformiv(prog, rect_programs[i].sampler, &unit);
			}
			break;
		}
	}
	int active = -1;
	for (int u = 0; u < UNITS; u++) {
		GLuint want = u == unit ? unit_rect[u] : unit_2d[u];
		if (unit_held[u] != want) {
			if (active < 0) {
				active = active_unit();
			}
			glActiveTexture(GL_TEXTURE0 + u);
			glBindTexture(GL_TEXTURE_2D, want);
			unit_held[u] = want;
		}
		if (want != 0) {
			copy_refresh(u, &active);
		}
	}
	if (unit >= 0 && unit < UNITS && scale >= 0) {
		GLuint tex = unit_rect[unit];
		if (tex < TEXSZ_MAX) {
			w = tex_w[tex];
			h = tex_h[tex];
		}
		if (w <= 0 || h <= 0) {
			if (active < 0) {
				active = active_unit();
			}
			glActiveTexture(GL_TEXTURE0 + unit);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
		}
		glUniform2f(scale, w > 0 ? 1.0f / w : 1.0f, h > 0 ? 1.0f / h : 1.0f);
	}
	if (active >= 0) {
		glActiveTexture(GL_TEXTURE0 + active);
	}
	fbfetch_bind(prog);
	feedback_barrier(prog);
}

void
sgl_BindTexture(void *rctx, GLenum target, GLuint texture)
{
	SGL_CUR(rctx);
	if (target == GL_TEXTURE_2D || target == 0x84F5) {
		int u = active_unit();
		*(target == GL_TEXTURE_2D ? &unit_2d[u] : &unit_rect[u]) = texture;
		unit_held[u] = texture;
		glBindTexture(GL_TEXTURE_2D, texture);
	} else {
		glBindTexture(target, texture);
	}
	SGL_POST(5);
}

/* GL unbinds deleted textures everywhere: so do the shadows */
void
sgl_DeleteTextures(void *rctx, GLsizei n, const GLuint *textures)
{
	SGL_CUR(rctx);
	for (GLsizei i = 0; i < n; i++) {
		copy_forget(textures[i]);
		if (textures[i] < TEXSZ_MAX) {
			tex_argb[textures[i]] = 0;      /* a reused name starts unswizzled */
		}
		for (int u = 0; u < UNITS; u++) {
			if (unit_2d[u] == textures[i]) {
				unit_2d[u] = 0;
			}
			if (unit_rect[u] == textures[i]) {
				unit_rect[u] = 0;
			}
			if (unit_held[u] == textures[i]) {
				unit_held[u] = 0;
			}
		}
	}
	glDeleteTextures(n, textures);
	SGL_POST(59);
}

void
sgl_PixelStorei(void *rctx, GLenum pname, GLint param)
{
	SGL_CUR(rctx);
	(void)0;
	glPixelStorei(pname, param);
	SGL_POST(195);
}

/*
 * CPU fast paths (fastpath.c) for CA's simplest draws: one screen-aligned
 * rectangle (6 indices or a 4-vertex strip, client arrays) with CA's vertex
 * shader and a fill / copy / tinted-copy fragment shader, no blending or
 * premultiplied "over", clamp-to-edge nearest or linear sampling of level 0.
 * SGLFAST (environment): 0 or unset, only count the draws that qualify;
 * 1, draw them on the CPU instead of softpipe, after checking the first 10
 * of each shader kind and blend mode against softpipe (a kind and blend
 * that differs stays with softpipe); 2, verify only: compute them, let
 * softpipe draw, compare (with a one-pixel border), the first 10 draws of
 * each shader kind and blend mode. A trailing s (1s, 2s) keeps the
 * pixel loop on one thread.
 * Counts and times every 10 s with the flush statistics.
 */
#define FAST_VS 0x40
#define FAST_BVS 0x41   /* a blur pass's vertex shader (blur_sh: its texcoords) */
static int fast_mode;
/*
 * CA's blur passes. Vertex shader: texcoordN = t0 + sign * offset[k],
 * t0 = vertex_texcoord0 * texmat0.xy + texmat0.zw. Fragment shader: qI =
 * texture2D(texture0, texcoordN); acc = qI * weight[i].c, then acc = qI *
 * weight[i].c + acc, ...; optionally acc /= max(0.001, acc.a) when divide
 * is not 0; gl_FragData[0] = acc. Parsed from main's body.
 */
struct blur_sh {
	uint8_t n, div, warr;                   /* fs: taps; uses divide; weight is an array */
	uint8_t tc[FP_TAPS], wi[FP_TAPS];       /* fs: each tap's texcoord and weight component (vec4 * 4 + c) */
	uint16_t have;                          /* vs: the texcoords it sets */
	int8_t sign[FP_TAPS];                   /* vs, by texcoord: sign and offset index */
	uint8_t k[FP_TAPS];
};
static struct blur_sh *blur_sh[PROG_MAX];
static struct prog_blur {
	int n, div;
	int8_t sign[FP_TAPS];
	uint8_t k[FP_TAPS], wi[FP_TAPS];
	GLint off[FP_TAPS], w[4], divloc;       /* offset[k], weight (vec4 i), divide */
} *prog_blur[PROG_MAX];
static uint8_t shader_kind[PROG_MAX], prog_kind[PROG_MAX];
static char *shader_body[PROG_MAX];     /* fragment shader main(), whitespace removed */
static uint8_t shader_clamp[PROG_MAX], prog_clamp[PROG_MAX];   /* CA's clamped-texcoord variant */
static struct {
	GLint pos, color, tc0, matrix, texmat0, tex0, tex1, texmatw, nscale, carg, tc1, texmat1, farg, cm[4];
} prog_loc[PROG_MAX];
enum { NO_KIND, NO_MODE, NO_STATE, NO_BLEND, NO_ARRAY, NO_SHAPE, NO_TEX, NO_FB, NO_MAP, NO_COUNT };
static struct {
	uint32_t draws, kind[FPK_COUNT], no[NO_COUNT], ok, bad;
	uint64_t eligible_abs, fast_abs, no_abs[NO_COUNT], map_abs, run_abs;
	uint64_t kind_abs[FPK_COUNT], kind_px[FPK_COUNT], wait_abs;    /* drawn here: time and pixels by kind */
	uint32_t multi_checked, spot;
	uint64_t core_abs[8];   /* draw time by the CPU the drawing thread was on (0-3 A55, 4-7 A78) */
	uint32_t blend[FPB_COUNT];
} fst;
/* per program over the 10 s window: draws, qualifying draws, time, reasons seen (bit per NO_) */
static struct {
	uint32_t draws, fast;
	uint16_t no;
	uint64_t abs;
} fpr[PROG_MAX];
static int fast_why;            /* reason of the last fast_no */
/* the first rejects of these reasons are logged with their details */
static int fast_detail_left[NO_COUNT] = {[NO_ARRAY] = 40, [NO_BLEND] = 20, [NO_TEX] = 20, [NO_MODE] = 10,
    [NO_SHAPE] = 10, [NO_STATE] = 10, [NO_FB] = 10, [NO_MAP] = 10};
/* and 4 more of each reason for each kind (a new kind's reasons after the old ones used theirs) */
static uint8_t fast_detail_kind[NO_COUNT][FPK_COUNT];
#define FAST_DETAIL(why, ...) do { int dk_ = kind > 0 && kind < FPK_COUNT ? kind : 0; \
	if (fast_detail_left[why] > 0 || fast_detail_kind[why][dk_] < 4) { \
	if (fast_detail_left[why] > 0) { fast_detail_left[why]--; } else { fast_detail_kind[why][dk_]++; } \
	sweet_drm_log("sweetgl: fast no " __VA_ARGS__); } } while (0)
static struct {
	uint8_t *buf;
	int bx, by, bw, bh, x0, y0, x1, y1, kind, blend, prog, spot, nrect;
	GLuint tex;
} fast_pend;
static uint32_t fast_verified[FPK_COUNT][FPB_COUNT], fast_bad_logged;
static uint64_t fast_vpix[FPK_COUNT][FPB_COUNT];        /* pixels compared */
static uint32_t fast_spot[FPK_COUNT][FPB_COUNT];        /* draws since the last spot check */
/*
 * Checked enough: FAST_VERIFY_EACH draws, or 3 covering a million pixels
 * (a full-screen blur checked costs softpipe's 2 s each time).
 */
#define FAST_DONE(k, b) (fast_verified[k][b] >= FAST_VERIFY_EACH || \
	(fast_verified[k][b] >= 3 && fast_vpix[k][b] >= 1000000))
static uint8_t fast_off[FPK_COUNT][FPB_COUNT];   /* differed from softpipe: not drawn here any more */
#define FAST_VERIFY_EACH 10

/* CA's dither block after the colour (NOISE_SIZE 32 in its vertex shader) */
#define FAST_NOISE "{mediumpvec3n=texture2DProj(texture1,texcoordN).xyz;n=(n-.5)*noise_scale;" \
	"lowpvec3c=gl_FragData[0].rgb+n*gl_FragData[0].a;gl_FragData[0].rgb=clamp(c,0.,gl_FragData[0].a);}"
#define FAST_NOISE_SIZE 32.0f
#define FAST_COLORED(k) ((k) == FPK_FILL || (k) == FPK_TINT || (k) == FPK_ATINT || (k) == FPK_TINTN || (k) == FPK_ATINTDK || \
	(k) == FPK_MASK || (k) == FPK_LUMC || (k) == FPK_SUPER || (k) == FPK_CIRCLE || (k) == FPK_MASKSUP)
/* samples texture0 */
#define FAST_TEXTURED(k) ((k) != FPK_FILL && (k) != FPK_LUMC && (k) != FPK_CM4 && (k) != FPK_CM3 && (k) != FPK_SUPER && \
	(k) != FPK_CIRCLE)
/* uses texcoord0 (texture0, or a shape of it) */
#define FAST_COORDS(k) (FAST_TEXTURED(k) || (k) == FPK_SUPER || (k) == FPK_CIRCLE)
/* uses texcoord1, and samples texture1 with it */
#define FAST_COORDS1(k) ((k) == FPK_MASK || (k) == FPK_MASKSUP || (k) == FPK_MIXBD)
#define FAST_TEXTURED1(k) ((k) == FPK_MASK || (k) == FPK_MIXBD)
/* CA's shape helpers, whitespace removed (checked in the whole source like darkenSover) */
#define FAST_SUPER_FN "lowpvec4supercircleImage(highpvec2uv){highpfloatd,w;uv=max(abs(uv)*1.199-vec2(0.199),0.0);" \
	"d=pow(uv.x,2.68)+pow(uv.y,2.68);w=max(fwidth(d),1e-4);mediumpfloatl=clamp((w*.5+(1.-d))/w,0.,1.);returnvec4(l);}"
#define FAST_CIRCLE_FN "lowpvec4circleImage(mediumpvec2uv){highpfloatd,w,s;d=max(dot(uv,uv),1e-4);d=d*inversesqrt(d);" \
	"w=max(fwidth(d),1e-4);mediumpfloatl=clamp((w*.5+(1.-d))/w,0.,1.);returnvec4(l);}"
#define FAST_LUMC_FN "lowpvec4luminanceCurve(lowpvec4s0,lowpvec4p,lowpfloata){lowpfloatx=dot(s0.rgb,vec3(.2125,.7154,.0721));" \
	"x=clamp((((p.x*x)+p.y)*x+p.z)*x+p.w,0.0,1.0);returnvec4(mix(s0.rgb,vec3(x,x,x),a),s0.a);}"
#define FAST_NOISY(k) ((k) == FPK_COPYN || (k) == FPK_TINTN || (k) == FPK_COPYDKN)
/* the backdrop read (fbfetch rewrite) and CA's darken composite, as the darken kinds start and use them */
#define FAST_FB "lowpvec4d0=texture2D(sgl_fb,gl_FragCoord.xy*sgl_fb_scale);"
#define FAST_DARKEN "{lowpvec4S=gl_FragData[0];lowpvec4D=d0;lowpvec4R=darkenSover(S,D);gl_FragData[0]=R;}"
#define FAST_DARKEN_FN "lowpvec4darkenSover(lowpvec4S,lowpvec4D){lowpfloaty=dot(D.rgb,vec3(.2125,.7154,.0721));" \
	"S=S*(y*y)*(y*y);returnD*(1.-S.a)+S;}"
/* CA's clamped sampling: tc0_0 = clamp(texcoord0_0, coord_arg.xy, coord_arg.zw), then tc0_0 in place of texcoord0_0 */
#define FAST_CLAMP "mediumpvec2tc0_0=clamp(texcoord0_0,coord_arg.xy,coord_arg.zw);"

/* a blur vertex shader's main, whitespace removed; 0 when it is one */
static int
blur_parse_vs(const char *b, struct blur_sh *o)
{
	static const char head[] =
	    "{highpvec2t0=vertex_texcoord0*texmat0.xy+texmat0.zw;gl_Position=vertex_matrix*vertex_position;";
	memset(o, 0, sizeof(*o));
	if (strncmp(b, head, sizeof(head) - 1) != 0) {
		return -1;
	}
	const char *p = b + sizeof(head) - 1;
	while (*p != '}') {
		int tc = -1, k = -1, len = 0;
		char sg = 0;
		if (sscanf(p, "texcoord%d=t0%coffset[%d];%n", &tc, &sg, &k, &len) != 3 || len == 0 || tc < 0 ||
		    tc >= FP_TAPS || k < 0 || k >= FP_TAPS || (sg != '+' && sg != '-') || (o->have & (1u << tc))) {
			return -1;
		}
		o->have |= (uint16_t)(1u << tc);
		o->sign[tc] = sg == '+' ? 1 : -1;
		o->k[tc] = (uint8_t)k;
		p += len;
	}
	return p[1] == 0 && o->have != 0 ? 0 : -1;
}

/* a blur fragment shader's main, whitespace removed; 0 when it is one */
static int
blur_parse_fs(const char *b, struct blur_sh *o)
{
	int qtc[FP_TAPS], ended = 0;
	for (int i = 0; i < FP_TAPS; i++) {
		qtc[i] = -1;
	}
	memset(o, 0, sizeof(*o));
	if (*b++ != '{') {
		return -1;
	}
	while (*b != 0 && *b != '}') {
		const char *e = strchr(b, ';');
		char st[160];
		if (e == NULL || ended || (size_t)(e - b) >= sizeof(st)) {
			return -1;
		}
		memcpy(st, b, (size_t)(e - b));
		st[e - b] = 0;
		b = e + 1;
		int q = -1, tc = -1, vi = 0, len = 0;
		char c = 0;
		if (strncmp(st, "lowpvec4q", 9) == 0 || strcmp(st, "mediumpvec4acc") == 0 || strcmp(st, "highpvec4acc") == 0) {
			continue;
		}
		if (sscanf(st, "q%d=texture2D(texture0,texcoord%d)%n", &q, &tc, &len) == 2 && st[len] == 0) {
			if (q < 0 || q >= FP_TAPS || tc < 0 || tc >= FP_TAPS) {
				return -1;
			}
			qtc[q] = tc;
			continue;
		}
		if (strcmp(st, "if(divide!=0.0)acc/=max(0.001,acc.a)") == 0) {
			o->div = 1;
			continue;
		}
		if (strcmp(st, "gl_FragData[0]=acc") == 0) {
			ended = 1;
			continue;
		}
		int arr = 0;
		if (sscanf(st, "acc=q%d*weight[%d].%c%n", &q, &vi, &c, &len) == 3) {
			arr = 1;
		} else if (sscanf(st, "acc=q%d*weight.%c%n", &q, &c, &len) != 2) {
			return -1;
		}
		/* the first term sets acc, the others add to it */
		const char *rest = st + len;
		if (strcmp(rest, o->n == 0 ? "" : "+acc") != 0 || q < 0 || q >= FP_TAPS || qtc[q] < 0 || vi < 0 || vi > 3 ||
		    strchr("xyzw", c) == NULL || c == 0 || o->n >= FP_TAPS || (o->n > 0 && arr != o->warr) || o->div) {
			return -1;
		}
		o->warr = (uint8_t)arr;
		o->tc[o->n] = (uint8_t)qtc[q];
		o->wi[o->n] = (uint8_t)(vi * 4 + (int)(strchr("xyzw", c) - "xyzw"));
		o->n++;
	}
	return ended && *b == '}' && b[1] == 0 && o->n > 0 ? 0 : -1;
}

static void
fast_shader(GLuint shader, const char *src)
{
	static const char *const body[FPK_COUNT] = {
		[FPK_FILL] = "{gl_FragData[0]=color;}",
		[FPK_COPY] = "{lowpvec4s0=texture2D(texture0,texcoord0_0);gl_FragData[0]=s0;}",
		[FPK_TINT] = "{lowpvec4s0=texture2D(texture0,texcoord0_0);gl_FragData[0]=color*s0;}",
		[FPK_ATINT] = "{lowpvec4t0=texture2D(texture0,texcoord0_0);lowpvec4s0=vec4(t0.a);gl_FragData[0]=color*s0;}",
		[FPK_OPAQUE] = "{lowpvec4t0=texture2D(texture0,texcoord0_0);lowpvec4s0=vec4(t0.rgb,1.);gl_FragData[0]=s0;}",
		[FPK_ALPHA] = "{lowpvec4t0=texture2D(texture0,texcoord0_0);lowpvec4s0=vec4(t0.a);gl_FragData[0]=s0;}",
		[FPK_COPYN] = "{lowpvec4s0=texture2D(texture0,texcoord0_0);gl_FragData[0]=s0;" FAST_NOISE "}",
		[FPK_TINTN] = "{lowpvec4s0=texture2D(texture0,texcoord0_0);gl_FragData[0]=color*s0;" FAST_NOISE "}",
		[FPK_ATINTDK] = "{" FAST_FB "lowpvec4t0=texture2D(texture0,texcoord0_0);lowpvec4s0=vec4(t0.a);"
		    "gl_FragData[0]=color*s0;" FAST_DARKEN "}",
		[FPK_COPYDKN] = "{" FAST_FB "lowpvec4s0=texture2D(texture0,texcoord0_0);gl_FragData[0]=s0;" FAST_DARKEN
		    FAST_NOISE "}",
		[FPK_MASK] = "{lowpvec4s0=texture2D(texture0,texcoord0_0);lowpvec4s1=texture2D(texture1,texcoord1_0);"
		    "gl_FragData[0]=color*s0*s1.a;}",
		[FPK_LUMC] = "{" FAST_FB "lowpvec4s0=d0;gl_FragData[0]=luminanceCurve(s0,function_arg,color.a);}",
		[FPK_CM4] = "{" FAST_FB "lowpvec4s0=d0;gl_FragData[0]=vec4(s0.r*color_matrix[0]+s0.g*color_matrix[1]+"
		    "s0.b*color_matrix[2]+s0.a*color_matrix[3],s0.a);}",
		[FPK_CM3] = "{" FAST_FB "lowpvec4s0=d0;gl_FragData[0]=vec4(s0.r*color_matrix[0]+s0.g*color_matrix[1]+"
		    "s0.b*color_matrix[2],s0.a);}",
		[FPK_SUPER] = "{lowpvec4s0=supercircleImage(texcoord0_0);gl_FragData[0]=color*s0;}",
		[FPK_CIRCLE] = "{lowpvec4s0=circleImage(texcoord0_0);gl_FragData[0]=color*s0;}",
		[FPK_MASKSUP] = "{lowpvec4s0=texture2D(texture0,texcoord0_0);lowpvec4s1=supercircleImage(texcoord1_0);"
		    "gl_FragData[0]=color*s0*s1.a;}",
		[FPK_MIXBD] = "{" FAST_FB "lowpvec4s0=texture2D(texture0,texcoord0_0);lowpvec4s1=texture2D(texture1,texcoord1_0);"
		    "gl_FragData[0]=mix(s1,d0,s0.a);}",
	};
	if (shader >= PROG_MAX) {
		return;
	}
	shader_kind[shader] = 0;
	if (strstr(src, "gl_Position = vertex_matrix * vertex_position;") != NULL &&
	    strstr(src, "color = vertex_color;") != NULL &&
	    strstr(src, "texcoord0_0 = vertex_texcoord0 * texmat0.xy + texmat0.zw;") != NULL) {
		shader_kind[shader] = FAST_VS;
		return;
	}
	if (strstr(src, "gl_Position") != NULL && strstr(src, "uniform highp vec2 offset[") != NULL) {
		/* a blur pass's vertex shader? */
		const char *vm = strstr(src, "void main");
		vm = vm != NULL ? strchr(vm, ')') : NULL;
		char *vb = vm != NULL ? malloc(strlen(vm)) : NULL;
		struct blur_sh t;
		if (vb != NULL) {
			size_t vn = 0;
			for (const char *q = vm + 1; *q != 0; q++) {
				if (*q != ' ' && *q != '\n' && *q != '\t' && *q != '\r') {
					vb[vn++] = *q;
				}
			}
			vb[vn] = 0;
			if (blur_parse_vs(vb, &t) == 0 && (blur_sh[shader] != NULL || (blur_sh[shader] = malloc(sizeof(t))) != NULL)) {
				*blur_sh[shader] = t;
				shader_kind[shader] = FAST_BVS;
			}
			free(vb);
		}
		if (shader_kind[shader] == FAST_BVS) {
			return;
		}
	}
	/* vertex shaders of other kinds: whole */
	static int unknown_vs_left = 16;
	if (!dump_shaders && unknown_vs_left > 0 && strstr(src, "gl_Position") != NULL) {
		unknown_vs_left--;
		sweet_drm_log("sweetgl: unknown vs %u source (%zu bytes):", shader, strlen(src));
		for (size_t i = 0, l = strlen(src); i < l && i < 2400; i += 400) {
			sweet_drm_log("sweetgl:   | %.400s", src + i);
		}
	}
	/* main's body, whatever its parameter list ("(void)" or "()") */
	const char *m = strstr(src, "void main");
	m = m != NULL ? strchr(m, ')') : NULL;
	if (m == NULL) {
		return;
	}
	m++;
	size_t n = 0, cap = strlen(m) + 1;
	char *b = malloc(cap);
	if (b == NULL) {
		return;
	}
	for (const char *q = m; *q != 0; q++) {
		if (*q != ' ' && *q != '\n' && *q != '\t' && *q != '\r') {
			b[n++] = *q;
		}
	}
	b[n] = 0;
	/* the clamped variant: the same body once the clamp is taken out and tc0_0 named back */
	const char *cmp = b;
	char *unclamped = NULL;
	shader_clamp[shader] = 0;
	if (b[0] == '{' && strncmp(b + 1, FAST_CLAMP, strlen(FAST_CLAMP)) == 0 && (unclamped = malloc(n + 64)) != NULL) {
		char *o = unclamped;
		*o++ = '{';
		for (const char *q = b + 1 + strlen(FAST_CLAMP); *q != 0;) {
			if (strncmp(q, "tc0_0", 5) == 0 && (q == b || q[-1] != 'r')) {
				memcpy(o, "texcoord0_0", 11);
				o += 11, q += 5;
			} else {
				*o++ = *q++;
			}
		}
		*o = 0;
		cmp = unclamped;
		shader_clamp[shader] = 1;
	}
	for (int k = 1; k < FPK_COUNT; k++) {
		if (body[k] != NULL && strcmp(cmp, body[k]) == 0) {
			shader_kind[shader] = (uint8_t)k;
		}
	}
	free(unclamped);
	const int sk = shader_kind[shader];
	if (sk == FPK_ATINTDK || sk == FPK_COPYDKN || sk == FPK_LUMC || sk == FPK_SUPER || sk == FPK_CIRCLE ||
	    sk == FPK_MASKSUP) {
		/* and darkenSover / luminanceCurve must be CA's: the whole source, whitespace removed */
		size_t l = strlen(src);
		char *all = malloc(l + 1), *o = all;
		if (all != NULL) {
			for (const char *q = src; *q != 0; q++) {
				if (*q != ' ' && *q != '\n' && *q != '\t' && *q != '\r') {
					*o++ = *q;
				}
			}
			*o = 0;
		}
		if (all == NULL ||
		    strstr(all, sk == FPK_LUMC ? FAST_LUMC_FN : sk == FPK_SUPER || sk == FPK_MASKSUP ? FAST_SUPER_FN :
		    sk == FPK_CIRCLE ? FAST_CIRCLE_FN : FAST_DARKEN_FN) == NULL) {
			shader_kind[shader] = 0;
		}
		free(all);
	}
	struct blur_sh bt;
	if (shader_kind[shader] == 0 && strstr(src, "gl_FragData") != NULL && blur_parse_fs(b, &bt) == 0 &&
	    (blur_sh[shader] != NULL || (blur_sh[shader] = malloc(sizeof(bt))) != NULL)) {
		*blur_sh[shader] = bt;
		shader_kind[shader] = FPK_BLUR;
		shader_clamp[shader] = 0;
	}
	if (shader_kind[shader] == 0) {
		shader_clamp[shader] = 0;
		/* kinds not added yet: their whole source, helper functions included */
		static int unknown_left = 48, pieces_left = 400;
		if (!dump_shaders && unknown_left > 0 && pieces_left > 0 && strstr(src, "gl_FragData") != NULL) {
			unknown_left--;
			sweet_drm_log("sweetgl: unknown fs %u source (%zu bytes):", shader, strlen(src));
			for (size_t i = 0, l = strlen(src); i < l && pieces_left > 0; i += 400, pieces_left--) {
				sweet_drm_log("sweetgl:   | %.400s", src + i);
			}
		}
	}
	free(shader_body[shader]);
	shader_body[shader] = b;
}

static void
fast_link(GLuint program)
{
	GLuint sh[4] = {0};
	GLsizei n = 0;
	int vs = 0, fs = 0;
	if (program >= PROG_MAX) {
		return;
	}
	prog_kind[program] = 0;
	prog_clamp[program] = 0;
	glGetAttachedShaders(program, 4, &n, sh);
	GLuint fsh = 0, bvsh = 0;
	for (GLsizei i = 0; i < n; i++) {
		int k = sh[i] < PROG_MAX ? shader_kind[sh[i]] : 0;
		vs |= k == FAST_VS || k == FAST_BVS;
		bvsh = k == FAST_BVS ? sh[i] : bvsh;
		fs = k != 0 && k != FAST_VS && k != FAST_BVS ? k : fs;
		fsh = k != FAST_VS && k != FAST_BVS && sh[i] < PROG_MAX && shader_body[sh[i]] != NULL ? sh[i] : fsh;
	}
	/* a blur's two halves go together, and only with each other */
	if ((fs == FPK_BLUR) != (bvsh != 0)) {
		vs = 0;
	}
	/* which program is which: the program numbers change between boots */
	static int linked_logged;
	if (linked_logged++ < 200) {
		const char *b = fsh < PROG_MAX && shader_body[fsh] != NULL ? shader_body[fsh] : "?";
		size_t l = strlen(b);
		sweet_drm_log("sweetgl: fast link prog %u fs %u kind %d vs %d: %.360s", program, fsh, fs, vs, fs ? "" : b);
		if (!fs && l > 360) {
			sweet_drm_log("sweetgl: fast link prog %u ...: %.400s", program, b + 360);
		}
	}
	if (!vs || !fs) {
		return;
	}
	if (fs == FPK_BLUR) {
		/* the taps: the fragment shader's texcoords and weights, the vertex shader's offsets */
		const struct blur_sh *f = blur_sh[fsh], *v = blur_sh[bvsh];
		struct prog_blur pb;
		memset(&pb, 0, sizeof(pb));
		if (f == NULL || v == NULL) {
			return;
		}
		pb.n = f->n, pb.div = f->div;
		for (int t = 0; t < f->n; t++) {
			if (!(v->have & (1u << f->tc[t]))) {
				return;
			}
			pb.sign[t] = v->sign[f->tc[t]], pb.k[t] = v->k[f->tc[t]], pb.wi[t] = f->wi[t];
		}
		for (int k = 0; k < FP_TAPS; k++) {
			char nm[16];
			snprintf(nm, sizeof(nm), "offset[%d]", k);
			pb.off[k] = glGetUniformLocation(program, nm);
		}
		for (int i = 0; i < 4; i++) {
			char nm[16];
			snprintf(nm, sizeof(nm), "weight[%d]", i);
			pb.w[i] = f->warr ? glGetUniformLocation(program, nm) : i == 0 ? glGetUniformLocation(program, "weight") : -1;
		}
		pb.divloc = f->div ? glGetUniformLocation(program, "divide") : -1;
		for (int t = 0; t < pb.n; t++) {
			if (pb.off[pb.k[t]] < 0 || pb.w[pb.wi[t] / 4] < 0) {
				return;
			}
		}
		if ((pb.div && pb.divloc < 0) || (prog_blur[program] == NULL && (prog_blur[program] = malloc(sizeof(pb))) == NULL)) {
			return;
		}
		*prog_blur[program] = pb;
	}
	prog_loc[program].pos = glGetAttribLocation(program, "vertex_position");
	prog_loc[program].color = glGetAttribLocation(program, "vertex_color");
	prog_loc[program].tc0 = glGetAttribLocation(program, "vertex_texcoord0");
	prog_loc[program].matrix = glGetUniformLocation(program, "vertex_matrix");
	prog_loc[program].texmat0 = glGetUniformLocation(program, "texmat0");
	prog_loc[program].tex0 = glGetUniformLocation(program, "texture0");
	prog_loc[program].tex1 = glGetUniformLocation(program, "texture1");
	prog_loc[program].texmatw = glGetUniformLocation(program, "texmatW");
	prog_loc[program].nscale = glGetUniformLocation(program, "noise_scale");
	prog_loc[program].carg = glGetUniformLocation(program, "coord_arg");
	prog_loc[program].tc1 = glGetAttribLocation(program, "vertex_texcoord1");
	prog_loc[program].texmat1 = glGetUniformLocation(program, "texmat1");
	prog_loc[program].farg = glGetUniformLocation(program, "function_arg");
	if (fs == FPK_LUMC && prog_loc[program].farg < 0) {
		return;
	}
	if (fs == FPK_CM4 || fs == FPK_CM3) {
		/* color_matrix is vec3[4] (or [3]): one location per row */
		static const char *const row[4] = {"color_matrix[0]", "color_matrix[1]", "color_matrix[2]", "color_matrix[3]"};
		for (int r = 0; r < 4; r++) {
			prog_loc[program].cm[r] = r < (fs == FPK_CM4 ? 4 : 3) ? glGetUniformLocation(program, row[r]) : -1;
			if (r < (fs == FPK_CM4 ? 4 : 3) && prog_loc[program].cm[r] < 0) {
				return;
			}
		}
	}
	if (FAST_COORDS1(fs) && (prog_loc[program].tc1 < 0 || prog_loc[program].texmat1 < 0 ||
	    (FAST_TEXTURED1(fs) && prog_loc[program].tex1 < 0))) {
		return;
	}
	int clamped = fsh < PROG_MAX && shader_clamp[fsh];
	if (clamped && prog_loc[program].carg < 0) {
		return;
	}
	int colored = FAST_COLORED(fs), textured = FAST_TEXTURED(fs), coords = FAST_COORDS(fs);
	if (FAST_NOISY(fs) && (prog_loc[program].tex1 < 0 || prog_loc[program].texmatw < 0 || prog_loc[program].nscale < 0)) {
		return;
	}
	if (prog_loc[program].pos < 0 || prog_loc[program].matrix < 0 || (colored && prog_loc[program].color < 0) ||
	    (coords && (prog_loc[program].tc0 < 0 || prog_loc[program].texmat0 < 0)) || (textured && prog_loc[program].tex0 < 0)) {
		return;
	}
	prog_kind[program] = (uint8_t)fs;
	prog_clamp[program] = (uint8_t)clamped;
}

#ifndef GL_HALF_FLOAT_OES
#define GL_HALF_FLOAT_OES 0x8D61
#endif

/* IEEE half to float (CA passes vertex colours as GL_HALF_FLOAT) */
static float
half_float(uint16_t h)
{
	int e = (h >> 10) & 0x1f, m = h & 0x3ff;
	float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : ldexpf((float)(m | 0x400), e - 25);
	return (h & 0x8000) ? -v : v;
}

struct fast_attr {
	const uint8_t *p;
	GLint on, size, type, stride, norm, buf;
	GLfloat cur[4];
};

static int
fast_attr_get(GLint loc, struct fast_attr *a)
{
	void *ptr = NULL;
	memset(a, 0, sizeof(*a));
	glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->on);
	if (!a->on) {
		glGetVertexAttribfv((GLuint)loc, GL_CURRENT_VERTEX_ATTRIB, a->cur);
		return 0;
	}
	glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buf);
	glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
	glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type);
	glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride);
	glGetVertexAttribiv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->norm);
	glGetVertexAttribPointerv((GLuint)loc, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr);
	a->p = ptr;
	return a->buf != 0 || ptr == NULL || a->size < 1 || a->size > 4 ||
	       (a->type != GL_FLOAT && a->type != GL_UNSIGNED_BYTE && a->type != GL_HALF_FLOAT &&
	        a->type != GL_HALF_FLOAT_OES) ? -1 : 0;
}

static void
fast_attr_read(const struct fast_attr *a, int idx, GLfloat v[4])
{
	v[0] = v[1] = v[2] = 0.0f;
	v[3] = 1.0f;
	if (!a->on) {
		memcpy(v, a->cur, sizeof(a->cur));
		return;
	}
	int half = a->type == GL_HALF_FLOAT || a->type == GL_HALF_FLOAT_OES;
	int esz = a->type == GL_FLOAT ? 4 : half ? 2 : 1;
	const uint8_t *e = a->p + (size_t)idx * (size_t)(a->stride ? a->stride : a->size * esz);
	for (int c = 0; c < a->size; c++) {
		if (a->type == GL_FLOAT) {
			memcpy(&v[c], e + 4 * c, 4);
		} else if (half) {
			uint16_t h;
			memcpy(&h, e + 2 * c, 2);
			v[c] = half_float(h);
		} else {
			v[c] = a->norm ? (GLfloat)e[c] / 255.0f : (GLfloat)e[c];
		}
	}
}

static int
fast_no(int why)
{
	fst.no[why]++;
	fast_why = why;
	return 0;
}

static inline int
fast_near(float a, float b, float eps)
{
	return fabsf(a - b) <= eps;
}

/*
 * Texture coordinates of a rectangle's corners (cx, cy: at the high x / y
 * edge): u must depend on one axis only and v on the other. uv[0], uv[1]: u
 * at that axis' low and high edge; uv[2], uv[3]: v. swap: u follows y.
 */
static int
fast_uv(const int cx[6], const int cy[6], const float tu[6], const float tv[6], int swap, float uv[4])
{
	int set[4] = {0};
	for (int i = 0; i < 6; i++) {
		int ui = swap ? cy[i] : cx[i], vi = 2 + (swap ? cx[i] : cy[i]);
		if (!set[ui]) {
			uv[ui] = tu[i], set[ui] = 1;
		} else if (!fast_near(uv[ui], tu[i], 1e-5f)) {
			return -1;
		}
		if (!set[vi]) {
			uv[vi] = tv[i], set[vi] = 1;
		} else if (!fast_near(uv[vi], tv[i], 1e-5f)) {
			return -1;
		}
	}
	return set[0] && set[1] && set[2] && set[3] ? 0 : -1;
}

/*
 * Returns 0: softpipe draws; 1: drawn here; 2: qualifies (counting or
 * verifying), softpipe draws.
 */
/* a texture a fast-path shader samples: bound as CA bound it, one level, identity swizzle, these wraps */
static int
fast_tex(GLint prog, GLint loc, GLint att, int kind, GLenum wrap, int nearest_only, const char *what, GLuint *tex,
    GLint *minf, GLint *magf)
{
	GLint unit = -1, active = GL_TEXTURE0, bound = 0, sampler = 0, ws = 0, wt = 0, base = 0, sw[4] = {0};
	glGetUniformiv((GLuint)prog, loc, &unit);
	if (unit < 0 || unit >= UNITS || (*tex = unit_held[unit]) == 0 || *tex == (GLuint)att) {
		FAST_DETAIL(NO_TEX, "%s prog %d kind %d: unit %d texture %u target %d", what, prog, kind, unit,
		    unit >= 0 && unit < UNITS ? unit_held[unit] : 0, att);
		return -1;
	}
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE0 + (GLenum)unit);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	glGetIntegerv(GL_SAMPLER_BINDING, &sampler);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minf);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magf);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wt);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, &base);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &sw[0]);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, &sw[1]);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &sw[2]);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, &sw[3]);
	glActiveTexture((GLenum)active);
	if ((GLuint)bound != *tex || sampler != 0 || ws != (GLint)wrap || wt != (GLint)wrap || base != 0 ||
	    sw[0] != GL_RED || sw[1] != GL_GREEN || sw[2] != GL_BLUE || sw[3] != GL_ALPHA ||
	    (*minf != GL_NEAREST && (nearest_only || (*minf != GL_LINEAR && *minf != GL_NEAREST_MIPMAP_NEAREST &&
	    *minf != GL_LINEAR_MIPMAP_NEAREST && *minf != GL_NEAREST_MIPMAP_LINEAR && *minf != GL_LINEAR_MIPMAP_LINEAR))) ||
	    (*magf != GL_NEAREST && (nearest_only || *magf != GL_LINEAR))) {
		FAST_DETAIL(NO_TEX, "%s prog %d kind %d: texture %u bound %d sampler %d wrap %#x %#x base %d swizzle %#x %#x "
		    "%#x %#x filter %#x %#x", what, prog, kind, *tex, bound, sampler, ws, wt, base, sw[0], sw[1], sw[2], sw[3],
		    *minf, *magf);
		return -1;
	}
	return 0;
}

/*
 * llvmpipe: hand the rectangles of a fill, copy or opaque-copy draw to the
 * rasterizer threads (lp-fast.py via sweet_lpfast.h); llvmpipe takes them
 * when it can draw every rectangle exactly, else the draw runs as usual.
 * SGLLPFAST=0: never.
 */
#ifdef SWEETGL_LLVMPIPE
#include "sweet_lpfast.h"
#endif

static void
lpf_offer(int kind, int blend, const struct fp_job *jobs, int n, GLuint tex)
{
#ifdef SWEETGL_LLVMPIPE
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("SGLLPFAST"), *d = getenv("GALLIUM_DRIVER");
		on = !(e != NULL && e[0] == '0') && !(d != NULL && strcmp(d, "softpipe") == 0);
		sweet_drm_log("sweetgl: rasterizer fast path for fills and 1:1 copies: %s", on ? "on" : "off");
	}
	struct sweet_lpf *p = &sweet_lpf_pending;
	p->n = 0;
	if (!on || n <= 0 || n > SWEET_LPF_MAX || (kind != FPK_FILL && kind != FPK_COPY && kind != FPK_OPAQUE) ||
	    (blend != FPB_NONE && blend != FPB_PREMUL)) {
		return;
	}
	p->src = NULL;
	if (kind != FPK_FILL && (p->src = fp_tex_resource(tex)) == NULL) {
		return;
	}
	p->kind = kind;
	p->blend = blend;
	for (int r = 0; r < n; r++) {
		const struct fp_job *j = &jobs[r];
		struct sweet_lpf_job *o = &p->job[r];
		o->x0 = j->x0, o->y0 = j->y0, o->x1 = j->x1, o->y1 = j->y1;
		o->xa = j->xa, o->xb = j->xb, o->ya = j->ya, o->yb = j->yb;
		o->ua = j->ua, o->ub = j->ub, o->va = j->va, o->vb = j->vb;
		o->swap = j->swap, o->tclamp = j->tclamp;
		o->cu0 = j->cu0, o->cu1 = j->cu1, o->cv0 = j->cv0, o->cv1 = j->cv1;
		memcpy(o->color, j->color, 4);
	}
	p->n = n;
#else
	(void)kind, (void)blend, (void)jobs, (void)n, (void)tex;
#endif
}

/* what one rectangle of a draw needs from its 6 vertices, before the textures are mapped */
struct fast_in {
	GLint prog;
	int kind, colored, textured, coords, masked, mtextured;     /* masked: texcoord1 used */
	const struct fast_attr *ap, *ac, *at, *at1;
	const GLfloat *M, *tm, *tm1;
	const GLint *vp;
	int scissor;
	GLint sc[4];
};

/*
 * One rectangle (two triangles, 6 vertex indices) into a job: window rect,
 * texture coordinates per axis (as drawn or turned), colour, the pixels
 * softpipe would cover. Returns 0, or the NO_ reason (counted by the caller).
 */
static int
fast_rect(const struct fast_in *in, const int idx[6], struct fp_job *j)
{
	const GLfloat *M = in->M;
	GLint prog = in->prog;
	int kind = in->kind;
	float wx[6], wy[6], tu[6], tv[6], tu1[6], tv1[6], w0 = 0.0f;
	GLfloat col0[4] = {0};
	for (int i = 0; i < 6; i++) {
		GLfloat p[4], c[4], t[4];
		fast_attr_read(in->ap, idx[i], p);
		float cx = M[0] * p[0] + M[4] * p[1] + M[8] * p[2] + M[12] * p[3];
		float cy = M[1] * p[0] + M[5] * p[1] + M[9] * p[2] + M[13] * p[3];
		float cz = M[2] * p[0] + M[6] * p[1] + M[10] * p[2] + M[14] * p[3];
		float cw = M[3] * p[0] + M[7] * p[1] + M[11] * p[2] + M[15] * p[3];
		if (!(cw > 0.0f) || !isfinite(cx) || !isfinite(cy) || !isfinite(cz) || fabsf(cz) > cw ||
		    (i > 0 && !fast_near(cw, w0, 1e-6f * w0))) {
			FAST_DETAIL(NO_SHAPE, "shape prog %d kind %d: vertex %d clip %g,%g,%g,%g", prog, kind, i, cx, cy, cz, cw);
			return NO_SHAPE;
		}
		w0 = cw;
		wx[i] = (float)in->vp[0] + (cx / cw + 1.0f) * (float)in->vp[2] * 0.5f;
		wy[i] = (float)in->vp[1] + (cy / cw + 1.0f) * (float)in->vp[3] * 0.5f;
		if (in->colored) {
			fast_attr_read(in->ac, idx[i], c);
			if (i == 0) {
				memcpy(col0, c, sizeof(col0));
			} else if (memcmp(c, col0, sizeof(col0)) != 0) {
				FAST_DETAIL(NO_SHAPE, "shape prog %d kind %d: colour differs between vertices", prog, kind);
				return NO_SHAPE;
			}
		}
		if (in->coords) {
			fast_attr_read(in->at, idx[i], t);
			tu[i] = t[0] * in->tm[0] + in->tm[2];
			tv[i] = t[1] * in->tm[1] + in->tm[3];
			if (!isfinite(tu[i]) || !isfinite(tv[i])) {
				return NO_SHAPE;
			}
		}
		if (in->masked) {
			fast_attr_read(in->at1, idx[i], t);
			tu1[i] = t[0] * in->tm1[0] + in->tm1[2];
			tv1[i] = t[1] * in->tm1[1] + in->tm1[3];
			if (!isfinite(tu1[i]) || !isfinite(tv1[i])) {
				return NO_SHAPE;
			}
		}
	}
	/* an axis-aligned rectangle: two triangles that meet on a diagonal */
	float xa = wx[0], xb = wx[0], ya = wy[0], yb = wy[0];
	for (int i = 1; i < 6; i++) {
		xa = fminf(xa, wx[i]), xb = fmaxf(xb, wx[i]), ya = fminf(ya, wy[i]), yb = fmaxf(yb, wy[i]);
	}
	const float e = 1e-3f;
	if (!(xb - xa > e) || !(yb - ya > e)) {
		return NO_SHAPE;
	}
	int corner[6], cx[6], cy[6];
	for (int i = 0; i < 6; i++) {
		cx[i] = fast_near(wx[i], xb, e), cy[i] = fast_near(wy[i], yb, e);
		if ((!cx[i] && !fast_near(wx[i], xa, e)) || (!cy[i] && !fast_near(wy[i], ya, e))) {
			FAST_DETAIL(NO_SHAPE, "shape prog %d kind %d: vertex %d at %g,%g not a corner of %g,%g-%g,%g", prog, kind, i,
			    wx[i], wy[i], xa, ya, xb, yb);
			return NO_SHAPE;
		}
		corner[i] = cx[i] | cy[i] << 1;
	}
	for (int t = 0; t < 2; t++) {
		int a = corner[3 * t], b = corner[3 * t + 1], c = corner[3 * t + 2];
		if (a == b || b == c || a == c) {
			FAST_DETAIL(NO_SHAPE, "shape prog %d kind %d: triangle %d repeats a corner (%d%d%d)", prog, kind, t, a, b, c);
			return NO_SHAPE;
		}
	}
	int shared = 0;
	for (int i = 0; i < 3; i++) {
		for (int k = 3; k < 6; k++) {
			if (corner[i] == corner[k]) {
				shared++;
			}
		}
	}
	/* the corner each triangle has alone must be opposite the other's */
	int lone0 = 6 - corner[0] - corner[1] - corner[2], lone1 = 6 - corner[3] - corner[4] - corner[5];
	if (shared != 2 || (lone0 ^ lone1) != 3) {
		FAST_DETAIL(NO_SHAPE, "shape prog %d kind %d: corners %d%d%d %d%d%d are not two triangles on a diagonal", prog,
		    kind, corner[0], corner[1], corner[2], corner[3], corner[4], corner[5]);
		return NO_SHAPE;
	}
	memset(j, 0, sizeof(*j));
	j->kind = kind, j->xa = xa, j->xb = xb, j->ya = ya, j->yb = yb;
	if (in->coords) {
		/* u from one axis and v from the other: as drawn, or turned (u follows y) */
		float uv[4];
		if (fast_uv(cx, cy, tu, tv, 0, uv) == 0) {
			j->swap = 0;
		} else if (fast_uv(cx, cy, tu, tv, 1, uv) == 0) {
			j->swap = 1;
		} else {
			FAST_DETAIL(NO_SHAPE, "shape prog %d kind %d: texcoords %g,%g %g,%g %g,%g %g,%g follow neither axis", prog, kind,
			    tu[0], tv[0], tu[1], tv[1], tu[2], tv[2], tu[3], tv[3]);
			return NO_SHAPE;
		}
		j->ua = uv[0], j->ub = uv[1], j->va = uv[2], j->vb = uv[3];
	}
	if (in->masked) {
		float uv[4];
		if (fast_uv(cx, cy, tu1, tv1, 0, uv) == 0) {
			j->swap2 = 0;
		} else if (fast_uv(cx, cy, tu1, tv1, 1, uv) == 0) {
			j->swap2 = 1;
		} else {
			FAST_DETAIL(NO_SHAPE, "shape prog %d kind %d: mask texcoords follow neither axis", prog, kind);
			return NO_SHAPE;
		}
		j->u2a = uv[0], j->u2b = uv[1], j->v2a = uv[2], j->v2b = uv[3];
	}
	for (int k = 0; k < 4; k++) {
		float c = col0[k] < 0.0f ? 0.0f : col0[k] > 1.0f ? 1.0f : col0[k];
		j->color[k] = (uint8_t)(c * 255.0f + 0.5f);
	}
	j->falpha = col0[3] < 0.0f ? 0.0f : col0[3] > 1.0f ? 1.0f : col0[3];
	/*
	 * softpipe's coverage (sp_setup.c, half-pixel centres): rows from
	 * ceil(y - 0.5), columns from (int)(x + 0.5), each up to the far edge's
	 * value, exclusive. They differ from ceil(x - 0.5) when an edge sits
	 * exactly on a half pixel (scaled layers): verify caught a sliver ending
	 * at x 739.5 where softpipe drew column 739.
	 */
	const GLint *vp = in->vp;
	j->x0 = (int)floorf(xa + 0.5f), j->x1 = (int)floorf(xb + 0.5f);
	j->y0 = (int)ceilf(ya - 0.5f), j->y1 = (int)ceilf(yb - 0.5f);
	j->x0 = j->x0 > vp[0] ? j->x0 : vp[0], j->x1 = j->x1 < vp[0] + vp[2] ? j->x1 : vp[0] + vp[2];
	j->y0 = j->y0 > vp[1] ? j->y0 : vp[1], j->y1 = j->y1 < vp[1] + vp[3] ? j->y1 : vp[1] + vp[3];
	if (in->scissor) {
		const GLint *sc = in->sc;
		j->x0 = j->x0 > sc[0] ? j->x0 : sc[0], j->x1 = j->x1 < sc[0] + sc[2] ? j->x1 : sc[0] + sc[2];
		j->y0 = j->y0 > sc[1] ? j->y0 : sc[1], j->y1 = j->y1 < sc[1] + sc[3] ? j->y1 : sc[1] + sc[3];
	}
	return 0;
}

/*
 * Filter for a job's scale: the minification filter when a texel step
 * exceeds a pixel. -1 when that filter uses mipmaps (only level 0 here).
 */
static int
fast_linear(float ua, float ub, float va, float vb, int swap, const struct fp_job *j, const struct fp_img *src,
    GLint minf, GLint magf)
{
	float rx = fabsf(ub - ua) * (float)src->w / (swap ? j->yb - j->ya : j->xb - j->xa);
	float ry = fabsf(vb - va) * (float)src->h / (swap ? j->xb - j->xa : j->yb - j->ya);
	if (rx > 1.0f || ry > 1.0f) {
		return minf == GL_LINEAR ? 1 : minf == GL_NEAREST ? 0 : -1;
	}
	return magf == GL_LINEAR;
}

#define FAST_RECTS 16

/*
 * Several rectangles in one draw (nine-slices): order the triangles so each
 * pair sharing an edge (two vertices at the same position) is consecutive.
 * fast_rect then checks each pair is a rectangle.
 */
static int
fast_pair(const struct fast_attr *ap, int *idx, int nrect)
{
	int ntri = 2 * nrect, used[2 * FAST_RECTS] = {0}, out[6 * FAST_RECTS], n = 0;
	GLfloat pos[6 * FAST_RECTS][4];
	for (int i = 0; i < 3 * ntri; i++) {
		fast_attr_read(ap, idx[i], pos[i]);
	}
	/* flat triangles (a zero-width or zero-height nine-slice cell) cover nothing: left out */
	for (int a = 0; a < ntri; a++) {
		const GLfloat *p0 = pos[3 * a], *p1 = pos[3 * a + 1], *p2 = pos[3 * a + 2];
		float area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
		used[a] = area == 0.0f;
	}
	for (int a = 0; a < ntri; a++) {
		if (used[a]) {
			continue;
		}
		int mate = -1;
		for (int b = a + 1; b < ntri && mate < 0; b++) {
			if (used[b]) {
				continue;
			}
			int same = 0, other = -1;
			for (int q = 0; q < 3; q++) {
				int hit = 0;
				for (int p = 0; p < 3; p++) {
					hit |= memcmp(pos[3 * a + p], pos[3 * b + q], sizeof(pos[0])) == 0;
				}
				same += hit;
				other = hit ? other : 3 * b + q;
			}
			if (same != 2) {
				continue;
			}
			/* the partner across the diagonal: the four corners lie on two x and two y values */
			const GLfloat *c4[4] = {pos[3 * a], pos[3 * a + 1], pos[3 * a + 2], pos[other]};
			int nx = 0, ny = 0;
			GLfloat xs[4], ys[4];
			for (int k = 0; k < 4; k++) {
				int fx = 0, fy = 0;
				for (int m = 0; m < nx; m++) {
					fx |= xs[m] == c4[k][0];
				}
				for (int m = 0; m < ny; m++) {
					fy |= ys[m] == c4[k][1];
				}
				if (!fx) {
					xs[nx++] = c4[k][0];
				}
				if (!fy) {
					ys[ny++] = c4[k][1];
				}
			}
			mate = nx == 2 && ny == 2 ? b : -1;
		}
		if (mate < 0) {
			return -1;
		}
		used[a] = used[mate] = 1;
		for (int p = 0; p < 3; p++) {
			out[n++] = idx[3 * a + p];
		}
		for (int p = 0; p < 3; p++) {
			out[n++] = idx[3 * mate + p];
		}
	}
	memcpy(idx, out, sizeof(int) * (size_t)n);
	return n / 6;
}

/*
 * Returns 0: softpipe draws; 1: drawn here; 2: qualifies (counting or
 * verifying), softpipe draws.
 */
static int
fast_draw(GLint prog, GLenum mode, GLsizei count, int elements, GLenum itype, const void *indices, GLint first)
{
	int kind = prog > 0 && prog < PROG_MAX ? prog_kind[prog] : 0, idx[6 * FAST_RECTS], nrect;
	fst.draws++;
	if (kind == 0) {
		return fast_no(NO_KIND);
	}
	if (elements) {
		/* indices in client memory only (with a bound buffer they are an offset) */
		GLint eb = 0;
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &eb);
		if (eb != 0 || itype != GL_UNSIGNED_SHORT || indices == NULL) {
			FAST_DETAIL(NO_ARRAY, "array prog %d kind %d: elements buffer %d type %#x indices %s mode %#x n %d", prog, kind,
			    eb, itype, indices != NULL ? "set" : "null", mode, count);
			return fast_no(NO_ARRAY);
		}
	}
	/* one rectangle (6 indices or a 4-vertex strip), or several (6 indices each: nine-slices) */
	if (mode == GL_TRIANGLES && count >= 6 && count % 6 == 0 && count <= 6 * FAST_RECTS) {
		nrect = count / 6;
		for (int i = 0; i < count; i++) {
			idx[i] = elements ? ((const GLushort *)indices)[i] : first + i;
		}
	} else if (mode == GL_TRIANGLE_STRIP && count == 4) {
		int v[4];
		for (int i = 0; i < 4; i++) {
			v[i] = elements ? ((const GLushort *)indices)[i] : first + i;
		}
		idx[0] = v[0], idx[1] = v[1], idx[2] = v[2], idx[3] = v[1], idx[4] = v[2], idx[5] = v[3];
		nrect = 1;
	} else {
		FAST_DETAIL(NO_MODE, "mode prog %d kind %d: mode %#x n %d elements %d", prog, kind, mode, count, elements);
		return fast_no(NO_MODE);
	}
	GLboolean cm[4] = {0};
	glGetBooleanv(GL_COLOR_WRITEMASK, cm);
	if (!cm[0] || !cm[1] || !cm[2] || !cm[3] || glIsEnabled(GL_DEPTH_TEST) || glIsEnabled(GL_STENCIL_TEST) ||
	    glIsEnabled(GL_CULL_FACE) || glIsEnabled(GL_RASTERIZER_DISCARD) || glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE)) {
		FAST_DETAIL(NO_STATE, "state prog %d kind %d: mask %d%d%d%d depth %d stencil %d cull %d", prog, kind, cm[0], cm[1],
		    cm[2], cm[3], glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_CULL_FACE));
		return fast_no(NO_STATE);
	}
	int blend = 0;
	if (glIsEnabled(GL_BLEND)) {
		GLint f[6] = {0};
		glGetIntegerv(GL_BLEND_SRC_RGB, &f[0]);
		glGetIntegerv(GL_BLEND_DST_RGB, &f[1]);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &f[2]);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &f[3]);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &f[4]);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &f[5]);
		if (f[1] == GL_ONE_MINUS_SRC_ALPHA && f[3] == GL_ONE_MINUS_SRC_ALPHA && f[4] == GL_FUNC_ADD &&
		    f[5] == GL_FUNC_ADD) {
			blend = f[0] == GL_ONE && f[2] == GL_ONE ? FPB_PREMUL :
			    f[0] == GL_SRC_ALPHA && f[2] == GL_SRC_ALPHA ? FPB_STRAIGHT :
			    f[0] == GL_SRC_ALPHA && f[2] == GL_ONE ? FPB_MIXED :
			    f[0] == GL_DST_ALPHA && f[2] == GL_DST_ALPHA ? FPB_ATOP :
			    f[0] == GL_ZERO && f[2] == GL_ZERO ? FPB_DSTOUT : FPB_NONE;
		} else if (f[0] == GL_ONE && f[1] == GL_ONE && f[2] == GL_ONE && f[3] == GL_ONE && f[4] == GL_FUNC_ADD &&
		    f[5] == GL_FUNC_ADD) {
			blend = FPB_ADD;
		}
		if (blend == FPB_NONE) {
			FAST_DETAIL(NO_BLEND, "blend prog %d kind %d: src %#x dst %#x srca %#x dsta %#x eq %#x %#x", prog, kind, f[0],
			    f[1], f[2], f[3], f[4], f[5]);
			return fast_no(NO_BLEND);
		}
	}
	GLint fb = 0, type = 0, att = 0, lvl = 0, db = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb);
	if (fb == 0) {
		FAST_DETAIL(NO_FB, "fb prog %d kind %d: window framebuffer", prog, kind);
		return fast_no(NO_FB);
	}
	glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	if (type != GL_TEXTURE) {
		FAST_DETAIL(NO_FB, "fb prog %d kind %d: fb %d attachment type %#x", prog, kind, fb, type);
		return fast_no(NO_FB);
	}
	glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att);
	glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &lvl);
	glGetIntegerv(GL_DRAW_BUFFER0, &db);
	if (att <= 0 || lvl != 0 || db != GL_COLOR_ATTACHMENT0) {
		FAST_DETAIL(NO_FB, "fb prog %d kind %d: fb %d texture %d level %d draw buffer %#x", prog, kind, fb, att, lvl, db);
		return fast_no(NO_FB);
	}
	/* vertices */
	struct fast_attr ap, ac, at, at1;
	struct fast_in in = {.prog = prog, .kind = kind, .colored = FAST_COLORED(kind), .textured = FAST_TEXTURED(kind),
	    .coords = FAST_COORDS(kind), .masked = FAST_COORDS1(kind), .mtextured = FAST_TEXTURED1(kind), .ap = &ap,
	    .ac = &ac, .at = &at, .at1 = &at1};
	const char *an = NULL;
	const struct fast_attr *bad = NULL;
	if (fast_attr_get(prog_loc[prog].pos, &ap)) {
		an = "position", bad = &ap;
	} else if (in.colored && fast_attr_get(prog_loc[prog].color, &ac)) {
		an = "color", bad = &ac;
	} else if (in.coords && fast_attr_get(prog_loc[prog].tc0, &at)) {
		an = "texcoord0", bad = &at;
	} else if (in.masked && fast_attr_get(prog_loc[prog].tc1, &at1)) {
		an = "texcoord1", bad = &at1;
	}
	if (an != NULL) {
		FAST_DETAIL(NO_ARRAY, "array prog %d kind %d: %s buffer %d size %d type %#x stride %d norm %d pointer %s, "
		    "mode %#x n %d elements %d", prog, kind, an, bad->buf, bad->size, bad->type, bad->stride, bad->norm,
		    bad->p != NULL ? "set" : "null", mode, count, elements);
		return fast_no(NO_ARRAY);
	}
	GLfloat M[16], tm[4] = {1, 1, 0, 0}, tmm[4] = {1, 1, 0, 0};
	glGetUniformfv((GLuint)prog, prog_loc[prog].matrix, M);
	if (in.coords) {
		glGetUniformfv((GLuint)prog, prog_loc[prog].texmat0, tm);
	}
	if (in.masked) {
		glGetUniformfv((GLuint)prog, prog_loc[prog].texmat1, tmm);
	}
	GLint vp[4] = {0};
	glGetIntegerv(GL_VIEWPORT, vp);
	in.M = M, in.tm = tm, in.tm1 = tmm, in.vp = vp;
	in.scissor = glIsEnabled(GL_SCISSOR_TEST);
	if (in.scissor) {
		glGetIntegerv(GL_SCISSOR_BOX, in.sc);
	}
	if (nrect > 1) {
		int paired = fast_pair(&ap, idx, nrect);
		if (paired <= 0) {
			FAST_DETAIL(NO_SHAPE, "shape prog %d kind %d: %d triangles do not pair into rectangles", prog, kind, 2 * nrect);
			/* the first few: the triangles (vertex positions as given), for what the mesh is */
			static int mesh_left = 6;
			if (mesh_left > 0) {
				mesh_left--;
				char line[400];
				for (int t0 = 0; t0 < 2 * nrect; t0 += 4) {
					int o = snprintf(line, sizeof(line), "sweetgl: fast mesh prog %d tri %d:", prog, t0);
					for (int t = t0; t < t0 + 4 && t < 2 * nrect && o < (int)sizeof(line) - 60; t++) {
						for (int v = 0; v < 3; v++) {
							GLfloat q[4];
							fast_attr_read(&ap, idx[3 * t + v], q);
							o += snprintf(line + o, sizeof(line) - (size_t)o, "%s%g,%g", v == 0 ? " | " : " ", q[0], q[1]);
						}
					}
					sweet_drm_log("%s", line);
				}
			}
			return fast_no(NO_SHAPE);
		}
		nrect = paired;
	}
	struct fp_job jobs[FAST_RECTS];
	for (int r = 0; r < nrect; r++) {
		int why = fast_rect(&in, idx + 6 * r, &jobs[r]);
		if (why != 0) {
			return fast_no(why);
		}
	}
	/* draw-wide settings of the jobs */
	GLfloat ca[4] = {0}, farg[4] = {0}, cmr[4][3] = {{0}};
	if (kind == FPK_LUMC) {
		glGetUniformfv((GLuint)prog, prog_loc[prog].farg, farg);
	}
	if (kind == FPK_CM4 || kind == FPK_CM3) {
		for (int r = 0; r < (kind == FPK_CM4 ? 4 : 3); r++) {
			glGetUniformfv((GLuint)prog, prog_loc[prog].cm[r], cmr[r]);
		}
	}
	if (prog_clamp[prog]) {
		glGetUniformfv((GLuint)prog, prog_loc[prog].carg, ca);
	}
	/* blur: each tap's offset (sign * offset[k]) and weight, and divide */
	float boff[FP_TAPS][2] = {{0}}, bw[FP_TAPS] = {0};
	int bdiv = 0;
	if (kind == FPK_BLUR) {
		const struct prog_blur *pb = prog_blur[prog];
		GLfloat wv[4][4] = {{0}}, divide = 0;
		if (pb == NULL) {
			return fast_no(NO_KIND);
		}
		for (int i = 0; i < 4; i++) {
			if (pb->w[i] >= 0) {
				glGetUniformfv((GLuint)prog, pb->w[i], wv[i]);
			}
		}
		for (int t = 0; t < pb->n; t++) {
			GLfloat o[2] = {0, 0};
			glGetUniformfv((GLuint)prog, pb->off[pb->k[t]], o);
			boff[t][0] = (float)pb->sign[t] * o[0], boff[t][1] = (float)pb->sign[t] * o[1];
			bw[t] = wv[pb->wi[t] / 4][pb->wi[t] % 4];
		}
		if (pb->div) {
			glGetUniformfv((GLuint)prog, pb->divloc, &divide);
			bdiv = divide != 0.0f;
		}
	}
	/* textures: what the shader's texture0 (and texture1 for masks) samples, clamped, one level */
	GLuint tex = 0, mtex = 0;
	GLint minf = 0, magf = 0, mminf = 0, mmagf = 0;
	if (in.textured && fast_tex(prog, prog_loc[prog].tex0, att, kind, GL_CLAMP_TO_EDGE, 0, "tex", &tex, &minf, &magf) != 0) {
		return fast_no(NO_TEX);
	}
	if (in.mtextured &&
	    fast_tex(prog, prog_loc[prog].tex1, att, kind, GL_CLAMP_TO_EDGE, 0, "mask", &mtex, &mminf, &mmagf) != 0) {
		return fast_no(NO_TEX);
	}
	GLuint ntex = 0;
	GLfloat W[4] = {0}, nscale = 0;
	if (FAST_NOISY(kind)) {
		GLint nmin = 0, nmag = 0;
		if (fast_tex(prog, prog_loc[prog].tex1, att, kind, GL_REPEAT, 1, "noise", &ntex, &nmin, &nmag) != 0) {
			return fast_no(NO_TEX);
		}
		glGetUniformfv((GLuint)prog, prog_loc[prog].texmatw, W);
		glGetUniformfv((GLuint)prog, prog_loc[prog].nscale, &nscale);
	}
	fst.kind[kind]++;
	fst.blend[blend]++;
	/*
	 * Checked draws: all of them in mode 2 (up to the budget), and in mode 1
	 * the first FAST_VERIFY_EACH of each kind and blend; a kind and blend
	 * that ever differs from softpipe goes back to softpipe for good. Draws
	 * of several rectangles are only drawn here once single ones passed.
	 */
	int check = fast_mode == 2 || !FAST_DONE(kind, blend), spot = 0;
	/*
	 * Spot checks, the whole run: one in 50 draws of each kind and blend
	 * (one in 400 of those over 256K pixels, a full-screen softpipe draw
	 * costs up to 2 s), so a case the first checks never met is caught too.
	 */
	if (!check && fast_mode == 1 && !fast_off[kind][blend] && fast_pend.buf == NULL) {
		uint64_t px = 0;
		for (int r = 0; r < nrect; r++) {
			const struct fp_job *j = &jobs[r];
			px += j->x1 > j->x0 && j->y1 > j->y0 ? (uint64_t)(j->x1 - j->x0) * (uint64_t)(j->y1 - j->y0) : 0;
		}
		if (++fast_spot[kind][blend] >= (px > 262144 ? 400u : 50u)) {
			fast_spot[kind][blend] = 0;
			check = spot = 1;
		}
	}
	/* several rectangles are checked together when no two cover the same pixel (nine-slices) */
	int disjoint = 1;
	for (int a = 0; a < nrect && check; a++) {
		for (int b = a + 1; b < nrect; b++) {
			const struct fp_job *p = &jobs[a], *q = &jobs[b];
			disjoint &= p->x1 <= q->x0 || q->x1 <= p->x0 || p->y1 <= q->y0 || q->y1 <= p->y0 || p->x1 <= p->x0 ||
			    p->y1 <= p->y0 || q->x1 <= q->x0 || q->y1 <= q->y0;
		}
	}
	if (spot && !disjoint) {
		check = spot = 0;       /* not checkable: drawn here as usual */
	}
	if (fast_mode == 0 || fast_off[kind][blend] || (check && !disjoint) ||
	    (check && ((fast_mode == 2 && fast_verified[kind][blend] >= FAST_VERIFY_EACH) || fast_pend.buf != NULL))) {
		if (check && fast_mode == 1 && !fast_off[kind][blend]) {
			fst.wait_abs++;
		}
		if (fast_mode == 0) {
			lpf_offer(kind, blend, jobs, nrect, tex);
		}
		return 2;
	}
	struct fp_img dst = {0}, src = {0}, mimg = {0}, nimg = {0};
	uint64_t tm0 = mach_absolute_time();
	if (fp_map_tex((GLuint)att, !check, &dst) != 0) {
		FAST_DETAIL(NO_MAP, "map prog %d kind %d: target texture %d (why %d, format %u)", prog, kind, att, fp_map_why,
		    fp_map_format);
		return fast_no(NO_MAP);
	}
	int mapped = 1;
	if (dst.fmt == FP_A8) {
		mapped = 0;
	} else if (in.textured && fp_map_tex(tex, 0, &src) != 0) {
		FAST_DETAIL(NO_MAP, "map prog %d kind %d: source texture %u (why %d, format %u)", prog, kind, tex, fp_map_why,
		    fp_map_format);
		mapped = 0;
	} else if (in.mtextured && fp_map_tex(mtex, 0, &mimg) != 0) {
		FAST_DETAIL(NO_MAP, "map prog %d kind %d: mask texture %u (why %d, format %u)", prog, kind, mtex, fp_map_why,
		    fp_map_format);
		mapped = 0;
	} else if (ntex != 0 && (fp_map_tex(ntex, 0, &nimg) != 0 || nimg.w <= 0 || nimg.h <= 0)) {
		FAST_DETAIL(NO_MAP, "map prog %d kind %d: noise texture %u (why %d, format %u)", prog, kind, ntex, fp_map_why,
		    fp_map_format);
		mapped = 0;
	}
	if (!mapped) {
		fp_unmap(&nimg);
		fp_unmap(&mimg);
		fp_unmap(&src);
		fp_unmap(&dst);
		return fast_no(NO_MAP);
	}
	for (int r = 0; r < nrect; r++) {
		struct fp_job *j = &jobs[r];
		j->blend = blend;
		if (prog_clamp[prog]) {
			j->tclamp = 1, j->cu0 = ca[0], j->cv0 = ca[1], j->cu1 = ca[2], j->cv1 = ca[3];
		}
		if (in.textured) {
			j->linear = fast_linear(j->ua, j->ub, j->va, j->vb, j->swap, j, &src, minf, magf);
		}
		if (in.mtextured) {
			j->src2 = &mimg;
			j->linear2 = fast_linear(j->u2a, j->u2b, j->v2a, j->v2b, j->swap2, j, &mimg, mminf, mmagf);
		}
		if (j->linear < 0 || j->linear2 < 0) {
			/* a mipmapped texture drawn smaller: needs levels softpipe has and we don't read */
			FAST_DETAIL(NO_TEX, "mip prog %d kind %d: texture minified with a mipmap filter", prog, kind);
			fp_unmap(&nimg);
			fp_unmap(&mimg);
			fp_unmap(&src);
			fp_unmap(&dst);
			return fast_no(NO_TEX);
		}
		if (kind == FPK_LUMC) {
			memcpy(j->farg, farg, sizeof(j->farg));
		}
		if (kind == FPK_CM4 || kind == FPK_CM3) {
			memcpy(j->cm, cmr, sizeof(j->cm));
		}
		if (kind == FPK_BLUR) {
			j->ntap = prog_blur[prog]->n, j->bdiv = bdiv;
			memcpy(j->toff, boff, sizeof(j->toff));
			memcpy(j->tw, bw, sizeof(j->tw));
		}
		if (ntex != 0) {
			/*
			 * texcoordN = (clip.xy * W.xy + W.zw * w, w * NOISE_SIZE), w constant
			 * here: u = (ndc.x * W.x + W.z) / NOISE_SIZE, ndc.x = (x - vp.x) * 2 / vp.w - 1
			 */
			j->noise = &nimg;
			j->nud = 2.0f * W[0] / ((float)vp[2] * FAST_NOISE_SIZE);
			j->nu0 = (W[2] - W[0] - 2.0f * W[0] * (float)vp[0] / (float)vp[2]) / FAST_NOISE_SIZE;
			j->nvd = 2.0f * W[1] / ((float)vp[3] * FAST_NOISE_SIZE);
			j->nv0 = (W[3] - W[1] - 2.0f * W[1] * (float)vp[1] / (float)vp[3]) / FAST_NOISE_SIZE;
			j->nscale = nscale;
		}
		j->x0 = j->x0 > 0 ? j->x0 : 0, j->x1 = j->x1 < dst.w ? j->x1 : dst.w;
		j->y0 = j->y0 > 0 ? j->y0 : 0, j->y1 = j->y1 < dst.h ? j->y1 : dst.h;
	}
	uint64_t tm1 = mach_absolute_time();
	/* the rectangles' union (pixels drawn) */
	int ux0 = INT_MAX, uy0 = INT_MAX, ux1 = INT_MIN, uy1 = INT_MIN;
	uint64_t npx = 0;
	for (int r = 0; r < nrect; r++) {
		const struct fp_job *j = &jobs[r];
		if (j->x1 > j->x0 && j->y1 > j->y0) {
			ux0 = j->x0 < ux0 ? j->x0 : ux0, uy0 = j->y0 < uy0 ? j->y0 : uy0;
			ux1 = j->x1 > ux1 ? j->x1 : ux1, uy1 = j->y1 > uy1 ? j->y1 : uy1;
			npx += (uint64_t)(j->x1 - j->x0) * (uint64_t)(j->y1 - j->y0);
		}
	}
	if (!check) {
		for (int r = 0; r < nrect; r++) {
			fp_run(&jobs[r], &dst, in.textured ? &src : NULL, NULL, 0);
		}
	} else if (ux1 > ux0 && uy1 > uy0) {
		/*
		 * verify: the expected pixels of the rectangles' union plus a
		 * one-pixel border; pixels no rectangle covers stay as they are
		 */
		int bx = ux0 > 0 ? ux0 - 1 : 0, by = uy0 > 0 ? uy0 - 1 : 0;
		int bw = (ux1 < dst.w ? ux1 + 1 : dst.w) - bx, bh = (uy1 < dst.h ? uy1 + 1 : dst.h) - by;
		uint8_t *buf = malloc((size_t)bw * (size_t)bh * 4);
		if (buf != NULL) {
			for (int y = 0; y < bh; y++) {
				for (int x = 0; x < bw; x++) {
					fp_read_rgba(&dst, bx + x, by + y, buf + ((size_t)y * (size_t)bw + (size_t)x) * 4);
				}
			}
			for (int r = 0; r < nrect; r++) {
				struct fp_job *j = &jobs[r];
				if (j->x1 > j->x0 && j->y1 > j->y0) {
					fp_run(j, &dst, in.textured ? &src : NULL,
					    buf + ((size_t)(j->y0 - by) * (size_t)bw + (size_t)(j->x0 - bx)) * 4, bw * 4);
				}
			}
			fast_pend.buf = buf, fast_pend.bx = bx, fast_pend.by = by, fast_pend.bw = bw, fast_pend.bh = bh;
			fast_pend.x0 = ux0, fast_pend.y0 = uy0, fast_pend.x1 = ux1, fast_pend.y1 = uy1;
			fast_pend.kind = kind, fast_pend.blend = blend, fast_pend.tex = (GLuint)att;
			fast_pend.prog = prog, fast_pend.spot = spot, fast_pend.nrect = nrect;
			fst.spot += spot;
			fst.multi_checked += nrect > 1;
		}
	}
	uint64_t tm2 = mach_absolute_time();
	fp_unmap(&nimg);
	fp_unmap(&mimg);
	fp_unmap(&src);
	fp_unmap(&dst);
	fst.map_abs += tm1 - tm0 + mach_absolute_time() - tm2;
	fst.run_abs += tm2 - tm1;
	if (!check) {
		fst.kind_abs[kind] += tm2 - tm1;
		fst.kind_px[kind] += npx;
	}
	return check ? 2 : 1;
}

/* SGLFAST=2: after softpipe drew, compare with what the fast path computed */
static void
fast_verify(void)
{
	struct fp_img dst = {0};
	int bad = 0, maxd = 0, fx = -1, fy = -1;
	uint8_t want[4] = {0}, got[4] = {0};
	fast_verified[fast_pend.kind][fast_pend.blend]++;
	fast_vpix[fast_pend.kind][fast_pend.blend] += (uint64_t)fast_pend.bw * (uint64_t)fast_pend.bh;
	if (fp_map_tex(fast_pend.tex, 0, &dst) == 0) {
		for (int y = 0; y < fast_pend.bh; y++) {
			for (int x = 0; x < fast_pend.bw; x++) {
				uint8_t g[4];
				const uint8_t *w = fast_pend.buf + ((size_t)y * (size_t)fast_pend.bw + (size_t)x) * 4;
				fp_read_rgba(&dst, fast_pend.bx + x, fast_pend.by + y, g);
				int d = 0;
				for (int k = 0; k < 4; k++) {
					int v = abs((int)w[k] - (int)g[k]);
					d = v > d ? v : d;
				}
				maxd = d > maxd ? d : maxd;
				if (d > 2 && bad++ == 0) {
					fx = fast_pend.bx + x, fy = fast_pend.by + y;
					memcpy(want, w, 4);
					memcpy(got, g, 4);
				}
			}
		}
		fp_unmap(&dst);
	}
	if (bad == 0) {
		fst.ok++;
	} else {
		fst.bad++;
		if (!fast_off[fast_pend.kind][fast_pend.blend]) {
			fast_off[fast_pend.kind][fast_pend.blend] = 1;
			sweet_drm_log("sweetgl: fast kind %d blend %d differs from softpipe (%s check, prog %d): softpipe draws it "
			    "from now on", fast_pend.kind, fast_pend.blend, fast_pend.spot ? "spot" : "first", fast_pend.prog);
		}
		if (fast_bad_logged++ < 60) {
			sweet_drm_log("sweetgl: fastverify %s prog %d rects %d kind %d blend %d rect %d,%d-%d,%d: %d px differ (max %d), "
			    "first %d,%d want %02x%02x%02x%02x got %02x%02x%02x%02x", fast_pend.spot ? "spot" : "first", fast_pend.prog,
			    fast_pend.nrect, fast_pend.kind, fast_pend.blend, fast_pend.x0, fast_pend.y0,
			    fast_pend.x1, fast_pend.y1, bad, maxd, fx, fy, want[0], want[1], want[2], want[3], got[0], got[1],
			    got[2], got[3]);
		}
	}
	free(fast_pend.buf);
	fast_pend.buf = NULL;
}

static uint64_t fs_ms(uint64_t abs);

/* libsystem_pthread (iOS 14.2 and later); weak, so a missing one is just not counted */
extern int pthread_cpu_number_np(size_t *cpu_number_out) __attribute__((weak_import));

/* a draw's time (softpipe's, or ours) by outcome and by program */
static void
fast_account(GLint prog, int fp, uint64_t dt)
{
	fst.eligible_abs += fp == 2 ? dt : 0;
	fst.fast_abs += fp == 1 ? dt : 0;
	size_t cpu = 0;
	if (pthread_cpu_number_np != NULL && pthread_cpu_number_np(&cpu) == 0 && cpu < 8) {
		fst.core_abs[cpu] += dt;
	}
	fst.no_abs[fast_why] += fp == 0 ? dt : 0;
	if (prog > 0 && prog < PROG_MAX) {
		fpr[prog].draws++;
		fpr[prog].fast += fp != 0;
		fpr[prog].no |= fp == 0 ? (uint16_t)(1u << fast_why) : 0;
		fpr[prog].abs += dt;
	}
}

/* every 10 s, with the flush statistics */
static void
fast_log(void)
{
	/* two lines: one would pass the 512-byte log line */
	sweet_drm_log("sweetgl: fast mode %d: %u draws; fill %u copy %u tint %u atint %u opaque %u alpha %u copyn %u tintn %u "
	    "atintdk %u copydkn %u mask %u lumc %u cm %u: %llu ms in softpipe, %llu ms here; verify ok %u bad %u", fast_mode,
	    fst.draws,
	    fst.kind[FPK_FILL], fst.kind[FPK_COPY], fst.kind[FPK_TINT], fst.kind[FPK_ATINT], fst.kind[FPK_OPAQUE],
	    fst.kind[FPK_ALPHA], fst.kind[FPK_COPYN], fst.kind[FPK_TINTN], fst.kind[FPK_ATINTDK], fst.kind[FPK_COPYDKN],
	    fst.kind[FPK_MASK], fst.kind[FPK_LUMC], fst.kind[FPK_CM4] + fst.kind[FPK_CM3], fs_ms(fst.eligible_abs),
	    fs_ms(fst.fast_abs), fst.ok, fst.bad);
	sweet_drm_log("sweetgl: fast not: kind %u mode %u state %u blend %u array %u shape %u tex %u fb %u map %u",
	    fst.no[NO_KIND], fst.no[NO_MODE], fst.no[NO_STATE], fst.no[NO_BLEND], fst.no[NO_ARRAY], fst.no[NO_SHAPE],
	    fst.no[NO_TEX], fst.no[NO_FB], fst.no[NO_MAP]);
	sweet_drm_log("sweetgl: fast here: map %llu ms, pixels %llu ms; blend none %u premul %u straight %u mixed %u atop %u "
	    "dstout %u add %u; super %u circle %u masksup %u mixbd %u blur %u",
	    fs_ms(fst.map_abs), fs_ms(fst.run_abs), fst.blend[FPB_NONE], fst.blend[FPB_PREMUL], fst.blend[FPB_STRAIGHT],
	    fst.blend[FPB_MIXED], fst.blend[FPB_ATOP], fst.blend[FPB_DSTOUT], fst.blend[FPB_ADD], fst.kind[FPK_SUPER],
	    fst.kind[FPK_CIRCLE], fst.kind[FPK_MASKSUP], fst.kind[FPK_MIXBD], fst.kind[FPK_BLUR]);
	sweet_drm_log("sweetgl: fast not, ms: kind %llu mode %llu state %llu blend %llu array %llu shape %llu tex %llu fb %llu "
	    "map %llu", fs_ms(fst.no_abs[NO_KIND]), fs_ms(fst.no_abs[NO_MODE]), fs_ms(fst.no_abs[NO_STATE]),
	    fs_ms(fst.no_abs[NO_BLEND]), fs_ms(fst.no_abs[NO_ARRAY]), fs_ms(fst.no_abs[NO_SHAPE]), fs_ms(fst.no_abs[NO_TEX]),
	    fs_ms(fst.no_abs[NO_FB]), fs_ms(fst.no_abs[NO_MAP]));
	/* time and pixels drawn here by kind (kind:ms/kilopixels), and draws that waited for their kind's checks */
	char kl[400];
	int ko = snprintf(kl, sizeof(kl), "sweetgl: fast cpu (kind:ms/kpx): waited %llu, multi checked %u, spot %u;",
	    (unsigned long long)fst.wait_abs, fst.multi_checked, fst.spot);
	for (int k = 1; k < FPK_COUNT && ko < (int)sizeof(kl) - 24; k++) {
		if (fst.kind_px[k] != 0) {
			ko += snprintf(kl + ko, sizeof(kl) - (size_t)ko, " %d:%llu/%llu", k, fs_ms(fst.kind_abs[k]),
			    (unsigned long long)(fst.kind_px[k] / 1000));
		}
	}
	sweet_drm_log("%s", kl);
	sweet_drm_log("sweetgl: fast draws by cpu, ms: %llu %llu %llu %llu | %llu %llu %llu %llu", fs_ms(fst.core_abs[0]),
	    fs_ms(fst.core_abs[1]), fs_ms(fst.core_abs[2]), fs_ms(fst.core_abs[3]), fs_ms(fst.core_abs[4]),
	    fs_ms(fst.core_abs[5]), fs_ms(fst.core_abs[6]), fs_ms(fst.core_abs[7]));
	/* the 6 programs that took the most time; reasons as bits: 1 kind 2 mode 4 state 8 blend 16 array ... */
	for (int r = 0; r < 6; r++) {
		int best = 0;
		for (int p = 1; p < PROG_MAX; p++) {
			best = fpr[p].abs > fpr[best].abs ? p : best;
		}
		if (best == 0 || fpr[best].abs == 0) {
			break;
		}
		sweet_drm_log("sweetgl: fast prog %d kind %d: %llu ms, %u draws, %u qualify, not %#x", best, prog_kind[best],
		    fs_ms(fpr[best].abs), fpr[best].draws, fpr[best].fast, fpr[best].no);
		fpr[best].abs = 0;
	}
	memset(fpr, 0, sizeof(fpr));
	memset(&fst, 0, sizeof(fst));
}

/*
 * Per-frame cost, always on: time spent in the app's draws (softpipe
 * rasterizes inside them, sometimes in the next draw) split by framebuffer
 * and program, plus binds and clears. A frame whose draws and finish take
 * 250 ms or more is logged, at most 400 times:
 *   sweetgl: slow frame N: T ms (draws D ms, finish F ms) wall W ms, C draws,
 *   B fbs (S ms not into the presented one), K binds, L clears, top prog P M ms (Q progs)
 */
#define FS_FBS 16
#define FS_PROGS 64
static struct {
	uint64_t draw_abs;
	uint32_t draws, binds, clears;
	int nfb, nprog;
	GLuint fb[FS_FBS];
	uint64_t fb_abs[FS_FBS];
	GLint prog[FS_PROGS];
	uint64_t prog_abs[FS_PROGS];
} fs;
static GLuint fs_cur_fb;
static uint64_t fs_start;
static uint32_t fs_frame, fs_logged, fs_detailed;

/*
 * Every draw of the frame, kept so the first 4 frames of 2 s or more can be
 * listed draw by draw: program, framebuffer and its texture size, viewport,
 * scissor (-1: off), index count, unit 0's texture and size, time.
 */
#define FS_DRAWS 256
static struct fs_rec {
	GLint prog, vp[4], sc[4];
	GLuint fb, tex0;
	GLsizei count;
	uint16_t fbw, fbh, t0w, t0h;
	uint32_t us, fbsid, t0sid;
	GLbitfield clear;       /* a glClear's mask (prog -1) */
} fs_rec[FS_DRAWS];
static int fs_nrec;

/*
 * SGLFRAMES=A-B: list every frame that ends between A and B seconds after
 * boot draw by draw, clears included (with their scissor: CA's partial
 * redraws), with the IOSurfaces of the framebuffer and of texture unit 0,
 * at most 3000 lines. Videos 22:57 and 07:31: in the scripted swipe from
 * page 2 back to page 1 (about 119-120 s) one frame shows a rectangle of
 * only wallpaper (icons missing) or black: redrawn wrongly, or a stale
 * part of an older frame?
 */
static uint32_t fs_win_from, fs_win_to, fs_win_lines;
static uint64_t fs_ms(uint64_t abs);

static bool
fs_in_window(void)
{
	if (fs_win_to == 0 || fs_win_lines >= 3000) {
		return false;
	}
	uint64_t up = fs_ms(mach_absolute_time()) / 1000;
	return up >= fs_win_from && up < fs_win_to;
}

static void
fs_record(GLint prog, GLsizei count, uint64_t dt, GLbitfield clear)
{
	GLint att = 0;
	struct fs_rec *r = &fs_rec[fs_nrec++];
	r->prog = prog;
	r->fb = fs_cur_fb;
	r->count = count;
	r->clear = clear;
	r->us = (uint32_t)(fs_ms(dt * 1000));
	glGetIntegerv(GL_VIEWPORT, r->vp);
	r->sc[0] = r->sc[1] = r->sc[2] = r->sc[3] = -1;
	if (glIsEnabled(GL_SCISSOR_TEST)) {
		glGetIntegerv(GL_SCISSOR_BOX, r->sc);
	}
	r->fbw = r->fbh = 0;
	r->fbsid = 0;
	if (fs_cur_fb != 0) {
		glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att);
		if (att > 0 && att < TEXSZ_MAX) {
			r->fbw = tex_w[att];
			r->fbh = tex_h[att];
			r->fbsid = tex_sid[att];
		}
	}
	r->tex0 = unit_held[0];
	r->t0w = r->tex0 < TEXSZ_MAX ? tex_w[r->tex0] : 0;
	r->t0h = r->tex0 < TEXSZ_MAX ? tex_h[r->tex0] : 0;
	r->t0sid = r->tex0 < TEXSZ_MAX ? tex_sid[r->tex0] : 0;
}

static uint64_t
fs_ms(uint64_t abs)
{
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return abs * tb.numer / tb.denom / 1000000;
}

static void
fs_draw(uint64_t dt, GLsizei count)
{
	GLint prog = 0;
	int i;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	/* any single draw of 300 ms or more, whichever frame it is in */
	static int big_left = 60;
	if (big_left > 0 && fs_ms(dt) >= 300) {
		GLint vp[4], sc[4] = {-1, -1, -1, -1}, fba = 0;
		big_left--;
		glGetIntegerv(GL_VIEWPORT, vp);
		if (glIsEnabled(GL_SCISSOR_TEST)) {
			glGetIntegerv(GL_SCISSOR_BOX, sc);
		}
		if (fs_cur_fb != 0) {
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &fba);
		}
		GLuint t0 = unit_held[0], t1 = unit_held[1];
		sweet_drm_log("sweetgl: big draw %llu ms prog %d fb %u %ux%u vp %d,%d %dx%d sc %d,%d %dx%d n %d tex0 %u %ux%u "
		    "tex1 %u %ux%u", fs_ms(dt), prog, fs_cur_fb, fba > 0 && fba < TEXSZ_MAX ? tex_w[fba] : 0,
		    fba > 0 && fba < TEXSZ_MAX ? tex_h[fba] : 0, vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3],
		    count, t0, t0 < TEXSZ_MAX ? tex_w[t0] : 0, t0 < TEXSZ_MAX ? tex_h[t0] : 0, t1,
		    t1 < TEXSZ_MAX ? tex_w[t1] : 0, t1 < TEXSZ_MAX ? tex_h[t1] : 0);
	}
	if ((fs_detailed < 4 || fs_in_window()) && fs_nrec < FS_DRAWS) {
		fs_record(prog, count, dt, 0);
	}
	fs.draws++;
	fs.draw_abs += dt;
	for (i = 0; i < fs.nfb && fs.fb[i] != fs_cur_fb; i++) {
	}
	if (i == fs.nfb && i < FS_FBS) {
		fs.fb[fs.nfb++] = fs_cur_fb;
	}
	if (i < fs.nfb) {
		fs.fb_abs[i] += dt;
	}
	for (i = 0; i < fs.nprog && fs.prog[i] != prog; i++) {
	}
	if (i == fs.nprog && i < FS_PROGS) {
		fs.prog[fs.nprog++] = prog;
	}
	if (i < fs.nprog) {
		fs.prog_abs[i] += dt;
	}
}

static void
fs_end(uint64_t finish_abs, uint64_t now)
{
	uint64_t total = fs.draw_abs + finish_abs, presented = 0;
	int top = 0;
	fs_frame++;
	if (fs_ms(total) >= 250 && fs_logged < 400) {
		fs_logged++;
		for (int i = 0; i < fs.nfb; i++) {
			presented = fs.fb[i] == fs_cur_fb ? fs.fb_abs[i] : presented;
		}
		for (int i = 1; i < fs.nprog; i++) {
			top = fs.prog_abs[i] > fs.prog_abs[top] ? i : top;
		}
		sweet_drm_log("sweetgl: slow frame %u: %llu ms (draws %llu ms, finish %llu ms) wall %llu ms, %u draws, "
		    "%d fbs (%llu ms not into the presented one), %u binds, %u clears, top prog %d %llu ms (%d progs)",
		    fs_frame, fs_ms(total), fs_ms(fs.draw_abs), fs_ms(finish_abs), fs_start ? fs_ms(now - fs_start) : 0,
		    fs.draws, fs.nfb, fs_ms(fs.draw_abs - presented), fs.binds, fs.clears,
		    fs.nprog ? fs.prog[top] : 0, fs.nprog ? fs_ms(fs.prog_abs[top]) : 0, fs.nprog);
		if (fs_ms(total) >= 2000 && fs_detailed < 4) {
			fs_detailed++;
			for (int i = 0; i < fs_nrec; i++) {
				struct fs_rec *r = &fs_rec[i];
				sweet_drm_log("sweetgl:  draw %d prog %d fb %u %ux%u vp %d,%d %dx%d sc %d,%d %dx%d n %d tex0 %u %ux%u %u us",
				    i, r->prog, r->fb, r->fbw, r->fbh, r->vp[0], r->vp[1], r->vp[2], r->vp[3], r->sc[0], r->sc[1],
				    r->sc[2], r->sc[3], r->count, r->tex0, r->t0w, r->t0h, r->us);
			}
		}
	}
	if (fs_in_window()) {
		GLint att = 0;
		uint32_t sid = 0;
		if (fs_cur_fb != 0) {
			glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			    GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &att);
			sid = att > 0 && att < TEXSZ_MAX ? tex_sid[att] : 0;
		}
		sweet_drm_log("sweetgl: F %u at %llu ms: %u draws, %u clears, %d listed, ends in fb %u surface %u, %llu ms",
		    fs_frame, fs_ms(now), fs.draws, fs.clears, fs_nrec, fs_cur_fb, sid, fs_ms(total));
		fs_win_lines++;
		for (int i = 0; i < fs_nrec && fs_win_lines < 3000; i++, fs_win_lines++) {
			struct fs_rec *r = &fs_rec[i];
			if (r->prog == -1) {
				sweet_drm_log("sweetgl: F %u.%d clear %#x fb %u s%u %ux%u vp %d,%d %dx%d sc %d,%d %dx%d",
				    fs_frame, i, r->clear, r->fb, r->fbsid, r->fbw, r->fbh, r->vp[0], r->vp[1], r->vp[2], r->vp[3],
				    r->sc[0], r->sc[1], r->sc[2], r->sc[3]);
			} else {
				sweet_drm_log("sweetgl: F %u.%d prog %d fb %u s%u %ux%u vp %d,%d %dx%d sc %d,%d %dx%d n %d tex0 %u s%u %ux%u %u us",
				    fs_frame, i, r->prog, r->fb, r->fbsid, r->fbw, r->fbh, r->vp[0], r->vp[1], r->vp[2], r->vp[3],
				    r->sc[0], r->sc[1], r->sc[2], r->sc[3], r->count, r->tex0, r->t0sid, r->t0w, r->t0h, r->us);
			}
		}
	}
	fs_nrec = 0;
	memset(&fs, 0, sizeof(fs));
	fs_start = now;
}

void
sgl_DrawArrays(void *rctx, GLenum mode, GLint first, GLsizei count)
{
	SGL_CUR(rctx);
	draw_prep(mode, count);
	uint64_t t0 = mach_absolute_time();
	GLint prog = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	int fp = fast_draw(prog, mode, count, 0, 0, NULL, first);
	if (fp != 1) {
#ifdef SWEETGL_LLVMPIPE
		sweet_lp_fbfetch_draw = fbfetch_now;
		sweet_lp_draw_kind = (prog > 0 && prog < PROG_MAX ? prog_kind[prog] : 0) | (fp != 0 ? 0x100u : 0);
#endif
		glDrawArrays(mode, first, count);
#ifdef SWEETGL_LLVMPIPE
		sweet_lp_fbfetch_draw = 0;
#endif
	}
	fbfetch_now = 0;
#ifdef SWEETGL_LLVMPIPE
	sweet_lpf_pending.n = 0;        /* taken or not: never for a later draw */
#endif
	uint64_t dt = mach_absolute_time() - t0;
	fast_account(prog, fp, dt);
	if (fast_pend.buf != NULL) {
		fast_verify();
	}
	fs_draw(dt, count);
	SGL_POST(65);
}

void
sgl_DrawElements(void *rctx, GLenum mode, GLsizei count, GLenum type, const void *indices)
{
	SGL_CUR(rctx);
	draw_prep(mode, count);
	uint64_t t0 = mach_absolute_time();
	GLint prog = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
	int fp = fast_draw(prog, mode, count, 1, type, indices, 0);
	if (fp != 1) {
#ifdef SWEETGL_LLVMPIPE
		sweet_lp_fbfetch_draw = fbfetch_now;
		sweet_lp_draw_kind = (prog > 0 && prog < PROG_MAX ? prog_kind[prog] : 0) | (fp != 0 ? 0x100u : 0);
#endif
		glDrawElements(mode, count, type, indices);
#ifdef SWEETGL_LLVMPIPE
		sweet_lp_fbfetch_draw = 0;
#endif
	}
	fbfetch_now = 0;
#ifdef SWEETGL_LLVMPIPE
	sweet_lpf_pending.n = 0;        /* taken or not: never for a later draw */
#endif
	uint64_t dt = mach_absolute_time() - t0;
	fast_account(prog, fp, dt);
	if (fast_pend.buf != NULL) {
		fast_verify();
	}
	fs_draw(dt, count);
	SGL_POST(67);
}

/* failures logged with the info log and source: CA does not ask for them */
void
sgl_CompileShaderARB(void *rctx, GLhandleARB shader)
{
	SGL_CUR(rctx);
	glCompileShader(H(shader));
	GLint ok = 0;
	glGetShaderiv(H(shader), GL_COMPILE_STATUS, &ok);
	if (!ok) {
		static char log[1024], src[4096];
		glGetShaderInfoLog(H(shader), sizeof(log), NULL, log);
		glGetShaderSource(H(shader), sizeof(src), NULL, src);
		sweet_drm_log("sweetgl: shader %u compile failed: %.400s", H(shader), log);
		for (size_t i = 0, n = strlen(src); i < n; i += 400) {
			sweet_drm_log("sweetgl:   src %.400s", src + i);
		}
	}
}

void
sgl_LinkProgramARB(void *rctx, GLhandleARB program)
{
	SGL_CUR(rctx);
	glLinkProgram(H(program));
	GLint ok = 0;
	glGetProgramiv(H(program), GL_LINK_STATUS, &ok);
	if (ok) {
		rect_link(H(program));
		fbfetch_link(H(program));
		fast_link(H(program));
	}
	if (dump_shaders) {
		GLuint sh[4] = {0};
		GLsizei n = 0;
		glGetAttachedShaders(H(program), 4, &n, sh);
		sweet_drm_log("sweetgl: program %u = shaders %u %u", H(program), sh[0], sh[1]);
	}
	if (!ok) {
		static char log[1024];
		glGetProgramInfoLog(H(program), sizeof(log), NULL, log);
		sweet_drm_log("sweetgl: program %u link failed: %.400s", H(program), log);
	}
}

void
sgl_FramebufferTexture2DEXT(void *rctx, GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)
{
	SGL_CUR(rctx);
	GLint fb = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
	TRACE("FramebufferTexture2D fb %d attachment %#x tex %u", fb, attachment, texture);
	glFramebufferTexture2D(target, attachment, textarget == 0x84F5 ? GL_TEXTURE_2D : textarget, texture, level);
	SGL_POST(677);
}

void
sgl_TexParameteri(void *rctx, GLenum target, GLenum pname, GLint param)
{
	SGL_CUR(rctx);
	target = SGL_TGT(target);
	if (pname >= 0x8E42 && pname <= 0x8E46) {
		TRACE("TexParameteri tex %u swizzle %#x = %#x", unit_held[active_unit()], pname, param);
	}
	glTexParameteri(target, pname, param);
	SGL_POST(304);
}

void
sgl_Clear(void *rctx, GLbitfield mask)
{
	SGL_CUR(rctx);
	glClear(mask);
	fs.clears++;
	if (fs_in_window() && fs_nrec < FS_DRAWS) {
		fs_record(-1, 0, 0, mask);
	}
	SGL_POST(10);
}

void
sgl_BindFramebufferEXT(void *rctx, GLenum target, GLuint framebuffer)
{
	SGL_CUR(rctx);
	glBindFramebuffer(target, framebuffer);
	if (target != GL_READ_FRAMEBUFFER && framebuffer != fs_cur_fb) {
		fs.binds++;
		fs_cur_fb = framebuffer;
	}
	SGL_POST(672);
}

static GLint
unpack_row(void)
{
	GLint row = 0;
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
	return row;
}

void
sgl_TexSubImage2D(void *rctx, GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const void *pixels)
{
	SGL_CUR(rctx);
	target = SGL_TGT(target);
	TRACE("TexSubImage2D tex %u level %d at %d,%d %dx%d fmt %#x type %#x row %d", unit_held[active_unit()],
	    level, x, y, width, height, format, type, unpack_row());
	glTexSubImage2D(target, level, x, y, width, height, format, type, pixels);
	SGL_POST(307);
}

/* Apple's ES takes internalformat GL_RGBA with format GL_BGRA; Mesa wants GL_BGRA for both */
void
sgl_TexImage2D(void *rctx, GLenum target, GLint level, GLint internalformat, GLsizei width,
    GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
	SGL_CUR(rctx);
	GLenum asked = target;
	target = SGL_TGT(target);
	TRACE("TexImage2D target %#x unit %d tex %u level %d ifmt %#x %dx%d fmt %#x type %#x row %d", asked,
	    active_unit(), unit_held[active_unit()], level, internalformat, width, height, format, type, unpack_row());
	copy_forget(unit_held[active_unit()]);
	set_swizzle(target, false);
	if (format == GL_BGRA_EXT && (internalformat == GL_RGBA || internalformat == GL_RGBA8)) {
		internalformat = GL_BGRA_EXT;
	}
	glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
	SGL_POST(301);
	tex_size_bound(target, level, width, height);
}

/*
 * ponytail: flush = finish. CA hands rendered IOSurfaces to the display
 * (IOMFB swap, copied by the kernel at once) without waiting for the GPU;
 * per-surface fences in the swap path would let the GPU run ahead.
 */
/*
 * Frame timing, logged every 10 s: flushes, time blocked in glFinish (GPU
 * still busy) and the CPU time between one flush's return and the next.
 */
static void
flush_stats(uint64_t t0, uint64_t t1)
{
	static uint64_t last, window, n, wait, wait_max, cpu;
	static mach_timebase_info_data_t tb;
	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	if (last != 0) {
		cpu += t0 - last;
	}
	wait += t1 - t0;
	wait_max = t1 - t0 > wait_max ? t1 - t0 : wait_max;
	n++;
	last = t1;
	if (window == 0) {
		window = t1;
	}
	uint64_t span = (t1 - window) * tb.numer / tb.denom;
	if (span >= 10000000000ull) {
		sweet_drm_log("sweetgl: %.1f flushes/s, finish avg %llu us max %llu us, between %llu us",
		    n * 1e9 / span, wait * tb.numer / tb.denom / 1000 / n, wait_max * tb.numer / tb.denom / 1000,
		    cpu * tb.numer / tb.denom / 1000 / n);
		fast_log();
#ifdef SWEETGL_LLVMPIPE
		/* llvmpipe's rasterizer profile and GL-thread waits (lp-sweet.py) */
		extern void sweet_lp_report(void (*lg)(const char *, ...));
		sweet_lp_report(sweet_drm_log);
#endif
		window = t1;
		n = wait = wait_max = cpu = 0;
	}
}

void
sgl_Flush(void *rctx)
{
	SGL_CUR(rctx);
	uint64_t t0 = mach_absolute_time();
	glFinish();
	uint64_t t1 = mach_absolute_time();
	flush_stats(t0, t1);
	fs_end(t1 - t0, t1);
}

void
sgl_FinishObjectAPPLE(void *rctx, GLenum object, GLint name)
{
	(void)object;
	(void)name;
	SGL_CUR(rctx);
	glFinish();
}

GLboolean
sgl_TestObjectAPPLE(void *rctx, GLenum object, GLuint name)
{
	(void)object;
	(void)name;
	SGL_CUR(rctx);
	glFinish();
	return GL_TRUE;
}

/* ---- IOSurfaces as textures ---- */

static bool
get_egl_image(struct pipe_frontend_screen *fscreen, void *image, struct st_egl_image *out)
{
	(void)fscreen;
	struct sgl_image *img = image;
	pipe_resource_reference(&out->texture, img->res);
	out->format = img->format;
	out->level = 0;
	out->layer = 0;
	out->internalformat = img->internalformat;
	return true;
}

static bool
validate_egl_image(struct pipe_frontend_screen *fscreen, void *image)
{
	(void)fscreen;
	return image != NULL;
}

void
sweetgl_image_hooks(struct pipe_frontend_screen *fscreen)
{
	fscreen->get_egl_image = get_egl_image;
	fscreen->validate_egl_image = validate_egl_image;
}

static enum pipe_format
gl_format(GLenum format, GLenum type, unsigned *internalformat)
{
	bool half = type == GL_HALF_FLOAT || type == 0x8D61 /* GL_HALF_FLOAT_OES */;
	switch (format) {
	case GL_BGRA_EXT:
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_B8G8R8A8_UNORM;
	case GL_RGBA:
		*internalformat = half ? GL_RGBA16F : GL_RGBA8;
		return half ? PIPE_FORMAT_R16G16B16A16_FLOAT : PIPE_FORMAT_R8G8B8A8_UNORM;
	case GL_LUMINANCE_ALPHA:
		*internalformat = GL_LUMINANCE_ALPHA;
		return PIPE_FORMAT_L8A8_UNORM;
	case GL_LUMINANCE:
		*internalformat = GL_LUMINANCE;
		return PIPE_FORMAT_L8_UNORM;
	case GL_ALPHA:
		*internalformat = GL_ALPHA;
		return PIPE_FORMAT_A8_UNORM;
	case GL_RED:
		*internalformat = half ? GL_R16F : GL_R8;
		return half ? PIPE_FORMAT_R16_FLOAT : PIPE_FORMAT_R8_UNORM;
	case GL_RG:
		*internalformat = half ? GL_RG16F : GL_RG8;
		return half ? PIPE_FORMAT_R16G16_FLOAT : PIPE_FORMAT_R8G8_UNORM;
	default:
		return PIPE_FORMAT_NONE;
	}
}

static enum pipe_format
surface_format(uint32_t fourcc, size_t bpe, unsigned *internalformat)
{
	switch (fourcc) {
	case 0:                 /* CA's own render surfaces carry no format */
		return bpe == 4 ? gl_format(GL_BGRA_EXT, GL_UNSIGNED_BYTE, internalformat) :
		       bpe == 8 ? gl_format(GL_RGBA, GL_HALF_FLOAT, internalformat) :
		       bpe == 1 ? gl_format(GL_RED, GL_UNSIGNED_BYTE, internalformat) : PIPE_FORMAT_NONE;
	case 'LA08':
		return gl_format(GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, internalformat);
	case 'A008':
		return gl_format(GL_ALPHA, GL_UNSIGNED_BYTE, internalformat);
	case 'BGRA':
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_B8G8R8A8_UNORM;
	case 'RGBA':
		*internalformat = GL_RGBA8;
		return PIPE_FORMAT_R8G8B8A8_UNORM;
	case 'RGhA':
		*internalformat = GL_RGBA16F;
		return PIPE_FORMAT_R16G16B16A16_FLOAT;
	case 'L008':
		*internalformat = GL_R8;
		return PIPE_FORMAT_R8_UNORM;
	case 'L016':
		*internalformat = GL_RG8;
		return PIPE_FORMAT_R8G8_UNORM;
	default:
		return PIPE_FORMAT_NONE;
	}
}

/* fourcc as 4 printable characters ('.' for the rest) */
#define FCC_C(c) ((c) >= 0x20 && (c) < 0x7f ? (c) : '.')
#define FCC(f) FCC_C((f) >> 24), FCC_C(((f) >> 16) & 0xff), FCC_C(((f) >> 8) & 0xff), FCC_C((f) & 0xff)

/*
 * One Mesa resource per surface (and format): CA renders into a surface
 * through one texture (attachImage) and samples it through another
 * (texImageIOSurface); as separate resources Mesa could run the reading
 * batch before the writing one (App Library pods, CC tiles, vibrant glyphs).
 * ponytail: 32 entries, least recently used dropped; a dropped entry keeps
 * nothing alive, a freed surface's pages stay imported until then.
 */
struct surf_res {
	uint32_t id, w, h, pitch;
	enum pipe_format format;
	uintptr_t base;
	struct pipe_resource *res;
	uint64_t used;
};
#define SURF_RES_MAX 32
static struct surf_res surf_res[SURF_RES_MAX];
static uint64_t surf_res_clock;

static bool
surf_res_same(const struct surf_res *a, const struct surf_res *b)
{
	return a->id == b->id && a->format == b->format && a->w == b->w && a->h == b->h && a->pitch == b->pitch &&
	       a->base == b->base;
}

/* a new reference to the cached resource, or NULL */
static struct pipe_resource *
surf_res_get(const struct surf_res *key)
{
	for (int i = 0; i < SURF_RES_MAX; i++) {
		if (surf_res[i].res != NULL && surf_res_same(&surf_res[i], key)) {
			struct pipe_resource *res = NULL;
			surf_res[i].used = ++surf_res_clock;
			pipe_resource_reference(&res, surf_res[i].res);
			return res;
		}
	}
	return NULL;
}

static void
surf_res_put(const struct surf_res *key, struct pipe_resource *res)
{
	if (res == NULL) {
		return;
	}
	int victim = 0;
	for (int i = 1; i < SURF_RES_MAX; i++) {
		if (surf_res[i].used < surf_res[victim].used) {
			victim = i;
		}
	}
	pipe_resource_reference(&surf_res[victim].res, NULL);
	surf_res[victim] = *key;
	surf_res[victim].used = ++surf_res_clock;
	pipe_resource_reference(&surf_res[victim].res, res);
}


static void
set_swizzle(GLenum target, bool argb)
{
	GLuint tex = unit_held[active_unit()];
	if (tex >= TEXSZ_MAX || tex_argb[tex] == argb) {
		return;
	}
	tex_argb[tex] = argb;
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_R, argb ? GL_ALPHA : GL_RED);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, argb ? GL_BLUE : GL_GREEN);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, argb ? GL_GREEN : GL_BLUE);
	glTexParameteri(target, GL_TEXTURE_SWIZZLE_A, argb ? GL_RED : GL_ALPHA);
}

/* GL upload format/type of a surface CA gave none for (as surface_format) */
static void
upload_format(uint32_t fourcc, size_t bpe, GLenum *format, GLenum *type)
{
	*type = GL_UNSIGNED_BYTE;
	switch (fourcc) {
	case 0:
		*format = bpe == 4 ? GL_BGRA_EXT : bpe == 8 ? GL_RGBA : bpe == 1 ? GL_RED : 0;
		*type = bpe == 8 ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
		break;
	case 'LA08': *format = GL_LUMINANCE_ALPHA; break;
	case 'A008': *format = GL_ALPHA; break;
	case 'BGRA': *format = GL_BGRA_EXT; break;
	case 'RGBA': *format = GL_RGBA; break;
	case 'RGhA': *format = GL_RGBA; *type = GL_HALF_FLOAT; break;
	case 'L008': *format = GL_RED; break;
	case 'L016': *format = GL_RG; break;
	default: *format = 0;
	}
}

/*
 * Rows the GPU cannot sample in place (A5xx pitch is 64-byte aligned; CA's
 * own bitmaps are not): copied into the texture instead.
 * ponytail: a snapshot, later CPU writes to the surface are not seen (CA's
 * images are immutable).
 */
static bool
copy_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, size_t w, size_t h, size_t pitch,
    const char *why)
{
	size_t bpe = IOSurfaceGetBytesPerElement(s);
	const void *base = IOSurfaceGetBaseAddress(s);
	if (base == NULL || bpe == 0 || pitch % bpe != 0) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu: cannot copy", why, IOSurfaceGetID(s), w, h, pitch);
		return false;
	}
	GLint row = 0, align = 4, skip_rows = 0, skip_pixels = 0;
	uint32_t seed = IOSurfaceGetSeed(s);
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
	glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch / bpe));
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	GLint ifmt = format == GL_RED ? GL_R8 : format == GL_RG ? GL_RG8 :
	    type == GL_HALF_FLOAT ? GL_RGBA16F : (GLint)format;
	/*
	 * A texture CA made with glTexStorage cannot take new storage:
	 * glTexImage2D and glEGLImageTargetTexture2DOES both fail with 0x502
	 * (Mesa: "texture is immutable"), and the texture kept what it held
	 * before (black bars over the News app's text at launch, a Today view
	 * widget gone plain blue). Write the pixels into its storage instead.
	 */
	GLint imm = 0;
	glGetTexParameteriv(target, GL_TEXTURE_IMMUTABLE_FORMAT, &imm);
	(void)glGetError();
	GLenum e;
	if (imm) {
		glTexSubImage2D(target, 0, 0, 0, (GLsizei)w, (GLsizei)h, format, type, base);
		e = glGetError();
		if (e == GL_INVALID_OPERATION && format == GL_BGRA_EXT && type == GL_UNSIGNED_BYTE) {
			/* RGBA8 storage takes RGBA only: swap B and R into a packed copy */
			uint8_t *t = malloc(w * h * 4);
			if (t != NULL) {
				for (size_t y = 0; y < h; y++) {
					const uint8_t *src = (const uint8_t *)base + y * pitch;
					uint8_t *dst = t + y * w * 4;
					for (size_t x = 0; x < w; x++, src += 4, dst += 4) {
						dst[0] = src[2], dst[1] = src[1], dst[2] = src[0], dst[3] = src[3];
					}
				}
				glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
				glTexSubImage2D(target, 0, 0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, t);
				e = glGetError();
				free(t);
			}
		}
		static int imm_logged;
		if (imm_logged++ < 40) {
			sweet_drm_log("sweetgl: %s: surface %u %zux%zu fmt %#x/%#x: texture %u immutable, written in place (%s %#x)",
			    why, IOSurfaceGetID(s), w, h, format, type, unit_held[active_unit()], e == GL_NO_ERROR ? "ok" : "error",
			    e);
		}
	} else {
		glTexImage2D(target, 0, ifmt, (GLsizei)w, (GLsizei)h, 0, format, type, base);
		e = glGetError();
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row);
	glPixelStorei(GL_UNPACK_ALIGNMENT, align);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
	tex_size_bound(target, 0, (GLsizei)w, (GLsizei)h);
	GLuint tex = unit_held[active_unit()];
	if (tex < TEXSZ_MAX) {
		struct copied *c = &copies[tex];
		if (c->surface != s) {
			CFRetain(s);
			copy_forget(tex);
			c->surface = s;
		}
		c->seed = seed;
		c->format = format;
		c->type = type;
	}
	size_t lit = 0, any = 0;
	const uint8_t *b8 = base, *mid = b8 + (h / 2) * pitch;
	for (size_t i = 0; trace_left > 0 && i < pitch * h; i++) {
		lit += i % bpe == bpe - 1 && b8[i] != 0;
		any += b8[i] != 0;
	}
	TRACE("%s surf %u tex %u %zux%zu pitch %zu fmt %#x/%#x copied (seed %u, %zu of %zu last bytes set, %zu bytes set, "
	    "mid row %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x), err %#x",
	    why, IOSurfaceGetID(s), tex, w, h, pitch, format, type, seed, lit, pitch * h / bpe, any,
	    mid[0], mid[1], mid[2], mid[3], mid[4], mid[5], mid[6], mid[7], mid[8], mid[9], mid[10], mid[11],
	    mid[12], mid[13], mid[14], mid[15], e);
	if (e != GL_NO_ERROR) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu fmt %#x/%#x: copy error %#x", why,
		    IOSurfaceGetID(s), w, h, pitch, format, type, e);
	}
	return e == GL_NO_ERROR;
}

/* The surface's pages as the storage of the texture bound to target. */
static bool
bind_surface(IOSurfaceRef s, GLenum target, GLenum format, GLenum type, uint32_t plane, const char *why)
{
	static int logged;
	uint32_t fourcc = IOSurfaceGetPixelFormat(s);
	size_t w = IOSurfaceGetWidthOfPlane(s, plane), h = IOSurfaceGetHeightOfPlane(s, plane);
	size_t pitch = IOSurfaceGetBytesPerRowOfPlane(s, plane);
	struct sgl_image img = {0};
	bool multi = IOSurfaceGetPlaneCount(s) > 1 || plane != 0;   /* ponytail: YUV surfaces not yet */
	{
		GLint bt = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bt);
		if (bt > 0 && bt < TEXSZ_MAX) {
			tex_sid[bt] = IOSurfaceGetID(s);
		}
	}
	/*
	 * Format-0 IOSurfaces with 4 bytes per pixel (backboardd's own surfaces)
	 * are B,G,R,A in memory, as upload_format() says. The app's texImage
	 * asks for GL_RGBA/UNSIGNED_BYTE, and those copies fail with 0x502
	 * (log: "fmt 0x1908/0x1401: copy error 0x502"). Upload them as BGRA
	 * instead; copy_surface() then passes GL_BGRA_EXT as both format and
	 * internal format. Untested until the shim runs on the device.
	 */
	if (format == GL_RGBA && type == GL_UNSIGNED_BYTE && fourcc == 0 && !multi &&
	    IOSurfaceGetBytesPerElementOfPlane(s, plane) == 4) {
		format = GL_BGRA_EXT;
	}
	img.format = format && !multi ? gl_format(format, type, &img.internalformat) : PIPE_FORMAT_NONE;
	if (img.format == PIPE_FORMAT_NONE && !multi) {
		img.format = surface_format(fourcc, IOSurfaceGetBytesPerElementOfPlane(s, plane), &img.internalformat);
	}
	/*
	 * CA's own 32-bit images without an IOSurface format (attached, no GL
	 * format) are A,B,G,R in memory (the Settings "+" circle, systemBlue:
	 * fc fc 79 00) and sampled as RGBA by its shaders.
	 */
	bool argb = format == 0 && fourcc == 0 && !multi && IOSurfaceGetBytesPerElement(s) == 4;
	if (argb) {
		format = GL_RGBA;
		type = GL_UNSIGNED_BYTE;
		img.format = gl_format(format, type, &img.internalformat);
	}
	set_swizzle(target, argb);
	/* immutable storage (glTexStorage) cannot be replaced by the surface's pages: copy into it */
	GLint imm = 0;
	glGetTexParameteriv(target, GL_TEXTURE_IMMUTABLE_FORMAT, &imm);
	(void)glGetError();
	if (!multi && (pitch % 64 != 0 || imm)) {
		if (format == 0) {
			upload_format(fourcc, IOSurfaceGetBytesPerElement(s), &format, &type);
		}
		if (format != 0) {
			return copy_surface(s, target, format, type, w, h, pitch, why);
		}
	}
	uint32_t handle = 0;
	uint64_t size = 0;
	const char *step = img.format == PIPE_FORMAT_NONE ? "format" : "kernel import";
	struct surf_res key = {
		.id = IOSurfaceGetID(s),
		.format = img.format,
		.w = (uint32_t)w,
		.h = (uint32_t)h,
		.pitch = (uint32_t)pitch,
		.base = (uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, plane),
	};
	img.res = img.format != PIPE_FORMAT_NONE ? surf_res_get(&key) : NULL;
	if (img.res == NULL && img.format != PIPE_FORMAT_NONE &&
	    sweet_drm_import_iosurface(IOSurfaceGetID(s), (uint64_t)(uintptr_t)IOSurfaceGetBaseAddress(s),
	    &handle, &size) == 0) {
		struct pipe_screen *screen = sweetgl_screen();
		struct pipe_resource templ = {
			.target = PIPE_TEXTURE_2D,
			.format = img.format,
			.width0 = (uint32_t)w,
			.height0 = (uint16_t)h,
			.depth0 = 1,
			.array_size = 1,
			.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED,
		};
		struct winsys_handle wh = {
			.type = WINSYS_HANDLE_TYPE_KMS,
			.handle = handle,
			.stride = (unsigned)pitch,
			/* client-memory surfaces start inside their first page (kernel 0157) */
			.offset = (unsigned)((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, plane) & vm_page_mask),
			.format = img.format,
			.modifier = DRM_FORMAT_MOD_LINEAR,
		};
		/* takes over the handle: closed with the resource, or now on failure */
		step = "mesa layout";
		img.res = screen->resource_from_handle(screen, &templ, &wh, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
		surf_res_put(&key, img.res);
	}
	if (img.res == NULL) {
		if (logged++ < 50) {
			sweet_drm_log("sweetgl: %s: surface %u %c%c%c%c %zux%zu pitch %zu plane %u: %s failed",
			    why, IOSurfaceGetID(s), FCC(fourcc), w, h, pitch, plane, step);
		}
		return false;
	}
	copy_forget(unit_held[active_unit()]);
	_mesa_EGLImageTargetTexture2DOES(target, (GLeglImageOES)&img);
	GLenum e = glGetError();
	tex_size_bound(target, 0, (GLsizei)w, (GLsizei)h);
	pipe_resource_reference(&img.res, NULL);
	if (trace_left > 0) {
		const uint8_t *base = IOSurfaceGetBaseAddress(s);
		const uint8_t *c = base ? base + (h / 2) * pitch + (w / 2) * IOSurfaceGetBytesPerElement(s) : NULL;
		GLint tex = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
		TRACE("%s surf %u %c%c%c%c %zux%zu pitch %zu fmt %#x/%#x -> pipe %d tex %d, center %02x%02x%02x%02x, err %#x",
		    why, IOSurfaceGetID(s), FCC(fourcc),
		    w, h, pitch, format, type, img.format, tex, c ? c[0] : 0, c ? c[1] : 0, c ? c[2] : 0, c ? c[3] : 0, e);
	}
	if (e != GL_NO_ERROR && logged++ < 50) {
		sweet_drm_log("sweetgl: %s: surface %u %zux%zu pitch %zu: EGLImageTargetTexture2D error %#x", why,
		    IOSurfaceGetID(s), w, h, pitch, e);
	}
	return e == GL_NO_ERROR;
}

/* ---- EAGLContext ---- */

static id
m_init(id self, SEL cmd, NSUInteger api, id props)
{
	struct sweetgl_ctx *ctx = api >= 2 && sweetgl_init() ? sweetgl_create((int)api, NULL) : NULL;
	struct sgl_context *c = ctx ? calloc(1, sizeof(*c)) : NULL;
	int slot = -1;
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX && slot < 0 && c; i++) {
		if (ctxs[i] == NULL) {
			slot = i;
			ctxs[i] = c;
		}
	}
	if (slot >= 0) {
		c->rctx = c;
		memcpy(c->disp, sgl_dispatch, sizeof(c->disp));
		c->eagl = self;
		c->api = (unsigned)api;
		c->ctx = ctx;
	}
	pthread_mutex_unlock(&ctx_lock);
	if (slot < 0) {
		free(c);
		sweetgl_destroy(ctx);
		sweet_drm_log("sweetgl: EAGLContext API %lu: no Mesa context, Apple's EAGL instead", (unsigned long)api);
		return ((id(*)(id, SEL, NSUInteger, id))orig_init)(self, cmd, api, props);
	}
	sweet_drm_log("sweetgl: EAGLContext %p API %lu on Mesa (GLI %p)", self, (unsigned long)api, c);
	return self;
}

static void
m_dealloc(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		((void (*)(id, SEL))orig_dealloc)(self, cmd);
		return;
	}
	pthread_mutex_lock(&ctx_lock);
	for (int i = 0; i < MAX_CTX; i++) {
		if (ctxs[i] == c) {
			ctxs[i] = NULL;
		}
	}
	pthread_mutex_unlock(&ctx_lock);
	if (sgl_current == c) {
		sgl_bind(NULL);
	}
	sweetgl_destroy(c->ctx);
	free(c);
	/* EAGLContext's own init never ran: skip its dealloc */
	struct objc_super sup = {self, class_getSuperclass(objc_getClass("EAGLContext"))};
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, cmd);
}

static NSUInteger
m_api(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	return c ? c->api : ((NSUInteger(*)(id, SEL))orig_api)(self, cmd);
}

static BOOL
m_set_param(id self, SEL cmd, uint32_t pname, const int32_t *value)
{
	if (lookup(self) == NULL) {
		return ((BOOL(*)(id, SEL, uint32_t, const int32_t *))orig_set_param)(self, cmd, pname, value);
	}
	sweet_drm_log("sweetgl: setParameter %#x to %d (ignored)", pname, value ? *value : 0);
	return YES;
}

static void *
m_macro(id self, SEL cmd)
{
	struct sgl_context *c = lookup(self);
	return c ? (void *)c : ((void *(*)(id, SEL))orig_macro)(self, cmd);
}

static BOOL
m_tex_iosurface(id self, SEL cmd, IOSurfaceRef s, NSUInteger target, NSUInteger internalformat, uint32_t width,
    uint32_t height, NSUInteger format, NSUInteger type, uint32_t plane, BOOL invert)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		return ((BOOL(*)(id, SEL, IOSurfaceRef, NSUInteger, NSUInteger, uint32_t, uint32_t, NSUInteger,
		    NSUInteger, uint32_t, BOOL))orig_tex_iosurface)(self, cmd, s, target, internalformat, width,
		    height, format, type, plane, invert);
	}
	SGL_CUR(c);
	return bind_surface(s, SGL_TGT((GLenum)target), (GLenum)format, (GLenum)type, plane, "texImageIOSurface");
}

static BOOL
m_attach(id self, SEL cmd, NSUInteger target, IOSurfaceRef s, BOOL inverted)
{
	struct sgl_context *c = lookup(self);
	if (c == NULL) {
		return ((BOOL(*)(id, SEL, NSUInteger, IOSurfaceRef, BOOL))orig_attach)(self, cmd, target, s, inverted);
	}
	SGL_CUR(c);
	return bind_surface(s, SGL_TGT((GLenum)target), 0, 0, 0, "attachImage");
}

static __thread id current_eagl;

static BOOL
m_set_current(id cls, SEL cmd, id eagl)
{
	struct sgl_context *c = lookup(eagl);
	if (c) {
		sgl_bind(c);
		current_eagl = eagl;
		return YES;
	}
	if (sgl_current) {
		sgl_bind(NULL);
	}
	current_eagl = nil;
	return ((BOOL(*)(id, SEL, id))orig_set_current)(cls, cmd, eagl);
}

static id
m_current(id cls, SEL cmd)
{
	return current_eagl ? current_eagl : ((id(*)(id, SEL))orig_current)(cls, cmd);
}

static IMP
swap(Class cls, const char *sel, void *imp)
{
	Method m = class_getInstanceMethod(cls, sel_registerName(sel));
	if (m == NULL) {
		sweet_drm_log("sweetgl: EAGLContext has no %s", sel);
		return NULL;
	}
	return method_setImplementation(m, (IMP)imp);
}

/*
 * Research hook (SWEETGL_TRACE): which image formats CA's GL renderer asks
 * its format table for (QuartzCore 0x18840433c in dsc-8: (ctx, format,
 * flag, &internalformat, &format, &type, &flags, &x)), with the caller.
 * The first 4 instructions become a jump here; they are replayed before
 * continuing. Needs cs_enforcement_disable (text page written).
 */
#include <mach/mach.h>
extern const void *_dyld_get_shared_cache_range(size_t *length);
void sgl_fmt_log(uint32_t fmt, uint32_t flag, uint64_t caller);
uint64_t sgl_fmt_cont;
static uint64_t sgl_cache_slide;

void
sgl_fmt_log(uint32_t fmt, uint32_t flag, uint64_t caller)
{
	static uint64_t seen[256];
	static int n;
	uint64_t key = (uint64_t)fmt << 48 ^ (uint64_t)flag << 40 ^ (caller - sgl_cache_slide);
	for (int i = 0; i < n; i++) {
		if (seen[i] == key) {
			return;
		}
	}
	if (n < 256) {
		seen[n++] = key;
		sweet_drm_log("sweetgl: CA format %u flag %u from %#llx", fmt, flag,
		    (unsigned long long)(caller - sgl_cache_slide));
	}
}

__attribute__((naked)) static void
sgl_fmt_hook(void)
{
	__asm__ volatile(
	    "stp x29, x30, [sp, #-96]!\n"
	    "mov x29, sp\n"
	    "stp x0, x1, [sp, #16]\n"
	    "stp x2, x3, [sp, #32]\n"
	    "stp x4, x5, [sp, #48]\n"
	    "stp x6, x7, [sp, #64]\n"
	    "mov x0, x1\n"
	    "mov x1, x2\n"
	    "mov x2, x30\n"
	    "bl _sgl_fmt_log\n"
	    "ldp x0, x1, [sp, #16]\n"
	    "ldp x2, x3, [sp, #32]\n"
	    "ldp x4, x5, [sp, #48]\n"
	    "ldp x6, x7, [sp, #64]\n"
	    "ldp x29, x30, [sp], #96\n"
	    "str wzr, [x5]\n"
	    "str wzr, [x4]\n"
	    "str wzr, [x3]\n"
	    "strb wzr, [x6]\n"
	    "adrp x16, _sgl_fmt_cont@PAGE\n"
	    "ldr x16, [x16, _sgl_fmt_cont@PAGEOFF]\n"
	    "br x16\n");
}

/*
 * Research entry hooks: log calls of QuartzCore renderer functions (first
 * calls in full, then counts), replaying the 4 overwritten prologue
 * instructions (position independent stack stores only).
 */
#define SGL_HOOKS 12
static const struct {
	uint64_t addr;          /* dsc-8, unslid */
	uint32_t code[4];
	const char *name;
} sgl_hook_def[SGL_HOOKS] = {
	{0x188424e54, {0xd103c3ff, 0x6d072beb, 0x6d0823e9, 0xa9096ffc}, "emit_filter"},
	{0x188427248, {0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8}, "render_vibrant_color_matrix"},
	{0x188427018, {0xd102c3ff, 0x6d0533ed, 0x6d062beb, 0x6d0723e9}, "render_vibrant_filter"},
	{0x1883900d0, {0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd}, "filter_node"},
	{0x188372f74, {0xd10703ff, 0x6d1523e9, 0xa9166ffc, 0xa91767fa}, "MaskNode::apply"},
	{0x18834d888, {0x6db63bef, 0x6d0133ed, 0x6d022beb, 0x6d0323e9}, "prepare_filters"},
	{0x18834f480, {0xd10303ff, 0x6d0623e9, 0xa90767fa, 0xa9085ff8}, "emit_filter_quads_color"},
	{0x1882d30b8, {0xd10243ff, 0x6d0323e9, 0xa90467fa, 0xa9055ff8}, "render_background"},
	{0x188311534, {0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8}, "render_solid_background"},
	{0x1884d504c, {0xd10483ff, 0x6d0d23e9, 0xa90e6ffc, 0xa90f57f6}, "fill_round_rect"},
	{0x1882f1298, {0xd10203ff, 0xa9045ff8, 0xa90557f6, 0xa9064ff4}, "fill_color_rect"},
	{0x1884af0d8, {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6}, "create_shader"},
};
uint64_t sgl_hook_cont[SGL_HOOKS];
void sgl_hook_log(uint64_t id, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t lr);

void
sgl_hook_log(uint64_t id, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t lr)
{
	static unsigned calls[SGL_HOOKS];
	static __thread int armed;
	unsigned c = ++calls[id];
	if (id == 7 || id == 8) {
		/* x1 OGL::Layer: +0x18 Render::Layer (bg colour float4 at +0x10, bounds 4 doubles at +0x40) */
		static unsigned shown;
		uint64_t rl = *(const uint64_t *)(uintptr_t)(x1 + 0x18);
		const double *b = (const double *)(uintptr_t)(rl + 0x40);
		const float *bg = (const float *)(uintptr_t)(rl + 0x10);
		armed = 0;
		if (b[2] >= 40 && b[2] <= 160 && b[2] == b[3] && shown++ < 600) {
			uint64_t gs = *(const uint64_t *)(uintptr_t)(x0 + 0x100);
			const __fp16 *g = (const __fp16 *)(uintptr_t)(gs + 0x20);
			const __fp16 *col = (const __fp16 *)(uintptr_t)x2;
			sweet_drm_log("sweetgl: %s L %#llx %gx%g bg %g,%g,%g,%g fl %#x/%#x mode %u gcol %g,%g,%g,%g arg %g,%g,%g,%g "
			    "from %#llx", id == 7 ? "bg" : "solid", (unsigned long long)x1, b[2], b[3], bg[0], bg[1], bg[2], bg[3],
			    *(const uint32_t *)(uintptr_t)(x1 + 0x20), *(const uint16_t *)(uintptr_t)(x1 + 0x24),
			    *(const uint8_t *)(uintptr_t)(x1 + 0x26), id == 8 ? (float)g[0] : 0.f, id == 8 ? (float)g[1] : 0.f,
			    id == 8 ? (float)g[2] : 0.f, id == 8 ? (float)g[3] : 0.f, id == 8 ? (float)col[0] : 0.f,
			    id == 8 ? (float)col[1] : 0.f, id == 8 ? (float)col[2] : 0.f, id == 8 ? (float)col[3] : 0.f,
			    (unsigned long long)(lr - sgl_cache_slide));
			armed = id == 8 ? 4 : 0;
		}
		return;
	}
	if (id == 11) {
		/* x1 GLShader: byte 0 = texture function (tex_fragment_shaders index); log each variant once */
		static uint8_t seen[256][16];
		static int n;
		const uint8_t *sh = (const uint8_t *)(uintptr_t)x1;
		for (int i = 0; i < n; i++) {
			if (memcmp(seen[i], sh, 16) == 0) {
				return;
			}
		}
		if (n < 256) {
			memcpy(seen[n++], sh, 16);
		}
		const char *const *tex = (const char *const *)(uintptr_t)(0x1d5b44a10ull + sgl_cache_slide);
		char hex[3 * 0x30 + 1];
		for (int i = 0; i < 0x30; i++) {
			snprintf(hex + 3 * i, 4, "%02x ", sh[i]);
		}
		sweet_drm_log("sweetgl: create_shader fn %#x%s: %s", sh[0], sh[0] < 90 && tex[sh[0]] == NULL ? " MISSING" : "",
		    hex);
		return;
	}
	if (id == 9 || id == 10) {
		if (armed > 0) {
			armed--;
			uint64_t st = *(const uint64_t *)(uintptr_t)(x0 + 0x10);
			sweet_drm_log("sweetgl:   %s shader %u blend %u from %#llx", sgl_hook_def[id].name,
			    *(const uint8_t *)(uintptr_t)(st + 0x10), *(const uint8_t *)(uintptr_t)(st + 0x1b0),
			    (unsigned long long)(lr - sgl_cache_slide));
		}
		return;
	}
	if (id == 6) {
		/* x0 Context*, x1 Surface*: surface rect at +0x20 and the context clip shape */
		static unsigned shown;
		const int32_t *r = (const int32_t *)(uintptr_t)(x1 + 0x20);
		uint64_t clip = *(const uint64_t *)(uintptr_t)(*(const uint64_t *)(uintptr_t)(x0 + 0x100) + 8);
		if (x1 != 0 && r[2] >= 60 && r[2] <= 700 && r[3] >= 60 && r[3] <= 700 && shown++ < 300) {
			if (clip & 1 || clip == 0) {
				sweet_drm_log("sweetgl: quads_color #%u surface %d,%d %dx%d clip %#llx", c, r[0], r[1], r[2], r[3],
				    (unsigned long long)clip);
			} else {
				const int32_t *sh = (const int32_t *)(uintptr_t)clip;
				sweet_drm_log("sweetgl: quads_color #%u surface %d,%d %dx%d clip type %d [%d %d %d %d %d]", c, r[0],
				    r[1], r[2], r[3], sh[1], sh[5], sh[6], sh[7], sh[8], sh[9]);
			}
		}
	}
	if (c <= 12 || (c & (c - 1)) == 0) {
		sweet_drm_log("sweetgl: hook %s #%u from %#llx x0 %#llx x1 %#llx x2 %#llx x3 %#llx", sgl_hook_def[id].name, c,
		    (unsigned long long)(lr - sgl_cache_slide), (unsigned long long)x0, (unsigned long long)x1,
		    (unsigned long long)x2, (unsigned long long)x3);
	}
}

#define SGL_HOOK_STUB(n, w0, w1, w2, w3)                                                        \
	__attribute__((naked)) static void sgl_hook_stub##n(void)                               \
	{                                                                                       \
		__asm__ volatile(                                                               \
		    "stp x29, x30, [sp, #-224]!\n"                                              \
		    "mov x29, sp\n"                                                             \
		    "stp x0, x1, [sp, #16]\n"                                                   \
		    "stp x2, x3, [sp, #32]\n"                                                   \
		    "stp x4, x5, [sp, #48]\n"                                                   \
		    "stp x6, x7, [sp, #64]\n"                                                   \
		    "stp q0, q1, [sp, #80]\n"                                                   \
		    "stp q2, q3, [sp, #112]\n"                                                  \
		    "stp q4, q5, [sp, #144]\n"                                                  \
		    "stp q6, q7, [sp, #176]\n"                                                  \
		    "str x8, [sp, #208]\n"                                                      \
		    "mov x5, x30\n"                                                             \
		    "mov x4, x3\n"                                                              \
		    "mov x3, x2\n"                                                              \
		    "mov x2, x1\n"                                                              \
		    "mov x1, x0\n"                                                              \
		    "mov x0, #" #n "\n"                                                         \
		    "bl _sgl_hook_log\n"                                                        \
		    "ldp x0, x1, [sp, #16]\n"                                                   \
		    "ldp x2, x3, [sp, #32]\n"                                                   \
		    "ldp x4, x5, [sp, #48]\n"                                                   \
		    "ldp x6, x7, [sp, #64]\n"                                                   \
		    "ldp q0, q1, [sp, #80]\n"                                                   \
		    "ldp q2, q3, [sp, #112]\n"                                                  \
		    "ldp q4, q5, [sp, #144]\n"                                                  \
		    "ldp q6, q7, [sp, #176]\n"                                                  \
		    "ldr x8, [sp, #208]\n"                                                      \
		    "ldp x29, x30, [sp], #224\n"                                                \
		    ".long " #w0 "\n.long " #w1 "\n.long " #w2 "\n.long " #w3 "\n"              \
		    "adrp x16, _sgl_hook_cont@PAGE\n"                                           \
		    "add x16, x16, _sgl_hook_cont@PAGEOFF\n"                                    \
		    "ldr x16, [x16, #" #n " * 8]\n"                                             \
		    "br x16\n");                                                                \
	}
SGL_HOOK_STUB(0, 0xd103c3ff, 0x6d072beb, 0x6d0823e9, 0xa9096ffc)
SGL_HOOK_STUB(1, 0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8)
SGL_HOOK_STUB(2, 0xd102c3ff, 0x6d0533ed, 0x6d062beb, 0x6d0723e9)
SGL_HOOK_STUB(3, 0xa9bc5ff8, 0xa90157f6, 0xa9024ff4, 0xa9037bfd)
SGL_HOOK_STUB(4, 0xd10703ff, 0x6d1523e9, 0xa9166ffc, 0xa91767fa)
SGL_HOOK_STUB(5, 0x6db63bef, 0x6d0133ed, 0x6d022beb, 0x6d0323e9)
SGL_HOOK_STUB(6, 0xd10303ff, 0x6d0623e9, 0xa90767fa, 0xa9085ff8)
SGL_HOOK_STUB(7, 0xd10243ff, 0x6d0323e9, 0xa90467fa, 0xa9055ff8)
SGL_HOOK_STUB(8, 0x6db923e9, 0xa9016ffc, 0xa90267fa, 0xa9035ff8)
SGL_HOOK_STUB(9, 0xd10483ff, 0x6d0d23e9, 0xa90e6ffc, 0xa90f57f6)
SGL_HOOK_STUB(10, 0xd10203ff, 0xa9045ff8, 0xa90557f6, 0xa9064ff4)
SGL_HOOK_STUB(11, 0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6)
static void (*const sgl_hook_stub[SGL_HOOKS])(void) = {sgl_hook_stub0, sgl_hook_stub1, sgl_hook_stub2, sgl_hook_stub3,
    sgl_hook_stub4, sgl_hook_stub5, sgl_hook_stub6, sgl_hook_stub7, sgl_hook_stub8, sgl_hook_stub9, sgl_hook_stub10,
    sgl_hook_stub11};

/* 16 bytes at t become ldr x16, #8; br x16; .quad to (after checking they are want) */
static bool
sgl_patch(uint32_t *t, const uint32_t want[4], void (*to)(void))
{
	if (memcmp(t, want, 16) != 0) {
		sweet_drm_log("sweetgl: patch: unexpected code at %p (%08x)", (void *)t, t[0]);
		return false;
	}
	vm_address_t page = (vm_address_t)t & ~(vm_address_t)vm_page_mask;
	if (vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
	    KERN_SUCCESS) {
		return false;
	}
	uint32_t patch[4] = {0x58000050, 0xd61f0200, 0, 0};
	uint64_t target = (uint64_t)(uintptr_t)to;
	memcpy(&patch[2], &target, 8);
	memcpy(t, patch, sizeof(patch));
	vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(t, sizeof(patch));
	return true;
}

static void
sgl_install_hooks(void)
{
	for (int i = 0; i < SGL_HOOKS; i++) {
		uint32_t *t = (uint32_t *)(uintptr_t)(sgl_hook_def[i].addr + sgl_cache_slide);
		sgl_hook_cont[i] = (uint64_t)(uintptr_t)t + 16;
		bool ok = sgl_patch(t, sgl_hook_def[i].code, sgl_hook_stub[i]);
		sweet_drm_log("sweetgl: hook %s at %p: %s", sgl_hook_def[i].name, (void *)t, ok ? "installed" : "FAILED");
	}
}

static void
sgl_install_fmt_hook(void)
{
	size_t len = 0;
	const uint8_t *cache = _dyld_get_shared_cache_range(&len);
	if (cache == NULL) {
		return;
	}
	sgl_cache_slide = (uint64_t)(uintptr_t)cache - 0x180000000ull;
	uint32_t *t = (uint32_t *)(uintptr_t)(0x18840433cull + sgl_cache_slide);
	static const uint32_t want[4] = {0xb90000bf, 0xb900009f, 0xb900007f, 0x390000df};
	if (memcmp(t, want, sizeof(want)) != 0) {
		sweet_drm_log("sweetgl: format hook: unexpected code at %p (%08x), not hooked", (void *)t, t[0]);
		return;
	}
	sgl_fmt_cont = (uint64_t)(uintptr_t)t + 16;
	vm_address_t page = (vm_address_t)t & ~(vm_address_t)vm_page_mask;
	kern_return_t kr = vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
	if (kr != KERN_SUCCESS) {
		sweet_drm_log("sweetgl: format hook: vm_protect rw %d", kr);
		return;
	}
	uint32_t patch[4] = {0x58000050, 0xd61f0200, 0, 0};    /* ldr x16, #8; br x16; .quad hook */
	uint64_t target = (uint64_t)(uintptr_t)sgl_fmt_hook;
	memcpy(&patch[2], &target, 8);
	memcpy(t, patch, sizeof(patch));
	kr = vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
	sys_icache_invalidate(t, sizeof(patch));
	sweet_drm_log("sweetgl: format hook installed at %p (slide %#llx), protect back %d", (void *)t,
	    (unsigned long long)sgl_cache_slide, kr);
	sgl_install_hooks();
}

/*
 * QuartzCore (dsc-8) GL renderer fixes, see vcm_func and nop_func:
 * tex_fragment_shaders (0x1d5b44a10) entries, and create_shader's
 * per-function uniform switch (byte table 0x18855a1e4, index function -
 * 0x11) set to look up the uniform names the function uses instead of
 * nothing (case 0xde).
 */
static const struct {
	uint8_t fn;
	const char *expr;
	uint8_t uniforms;       /* uniform switch case: 0 color_matrix, 0x95 minmax_dir/bgcolor/map/args, 0xde none */
} sgl_texfns[] = {{0x2d, "sgl_vcm(s0)", 0}, {0x14, "sgl_cm4(s0)", 0}, {0x26, "sgl_nop()", 0xde},
    {0x27, "sgl_vblur(s1)", 0x95}};

static bool
sgl_write_const(void *at, const void *val, size_t len, vm_prot_t back)
{
	vm_address_t page = (vm_address_t)at & ~(vm_address_t)vm_page_mask;
	if (vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) !=
	    KERN_SUCCESS) {
		return false;
	}
	memcpy(at, val, len);
	vm_protect(mach_task_self(), page, vm_page_size * 2, FALSE, back);
	return true;
}

static void
sgl_install_texfns(void)
{
	size_t len = 0;
	const uint8_t *cache = _dyld_get_shared_cache_range(&len);
	if (cache == NULL) {
		return;
	}
	uint64_t slide = (uint64_t)(uintptr_t)cache - 0x180000000ull;
	const char **table = (const char **)(uintptr_t)(0x1d5b44a10ull + slide);
	uint8_t *cases = (uint8_t *)(uintptr_t)(0x18855a1e4ull - 0x11 + slide);
	if (table[0x13] == NULL || strcmp(table[0x13], "colorMatrix4x4Bias(s0)") != 0 || cases[0x13] != 0) {
		sweet_drm_log("sweetgl: texture function fixes: unexpected QuartzCore, not installed");
		return;
	}
	for (size_t i = 0; i < sizeof(sgl_texfns) / sizeof(sgl_texfns[0]); i++) {
		uint8_t fn = sgl_texfns[i].fn, uniforms = sgl_texfns[i].uniforms;
		if (table[fn] != NULL || cases[fn] != 0xde) {
			sweet_drm_log("sweetgl: texture function %#x: unexpected (%p %#x), not installed", fn,
			    (void *)table[fn], cases[fn]);
			continue;
		}
		bool ok = sgl_write_const(&table[fn], &sgl_texfns[i].expr, sizeof(char *), VM_PROT_READ) &&
		    (uniforms == 0xde || sgl_write_const(&cases[fn], &uniforms, 1, VM_PROT_READ | VM_PROT_EXECUTE));
		sweet_drm_log("sweetgl: texture function %#x = %s: %s", fn, sgl_texfns[i].expr, ok ? "installed" : "FAILED");
	}
}

/* Called at load. CA picks its GLES renderer by itself once initWithAPI:3 succeeds. */
void
sweetgl_eagl_install(void)
{
	const char *e = getenv("SWEETGL_GLERR");
	sgl_check_errors = e && e[0] == '1';
	const char *fm = getenv("SGLFAST");
	fast_mode = fm != NULL && fm[0] >= '0' && fm[0] <= '2' ? fm[0] - '0' : 0;
	fp_threads = !(fm != NULL && fm[0] != 0 && fm[1] == 's');
#ifdef SWEETGL_LLVMPIPE
	/*
	 * The fast path writes IOSurface memory itself between the driver's
	 * draws: fine with softpipe, which has finished each draw by then, not
	 * with llvmpipe, whose threads rasterize binned scenes later.
	 */
	const char *drv = getenv("SGLDRIVER");
	if ((drv == NULL || strcmp(drv, "softpipe") != 0) && fast_mode != 0) {
		sweet_drm_log("sweetgl: fast path off under llvmpipe (SGLFAST=%d ignored)", fast_mode);
		fast_mode = 0;
	}
#endif
	const char *fw = getenv("SGLFRAMES");
	if (fw != NULL) {
		char *end = NULL;
		fs_win_from = (uint32_t)strtoul(fw, &end, 10);
		fs_win_to = end != NULL && *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 10) : 0;
		sweet_drm_log("sweetgl: frames from %u s to %u s listed", fs_win_from, fs_win_to);
	}
	const char *t = getenv("SWEETGL_TRACE");
	trace_left = t && t[0] == '1' ? 20000 : 0;
	dump_shaders = trace_left > 0;
	sgl_install_texfns();
	if (trace_left > 0) {
		sgl_install_fmt_hook();
	}
	Class cls = objc_getClass("EAGLContext");
	if (cls == Nil) {
		sweet_drm_log("sweetgl: no EAGLContext class");
		return;
	}
	Class meta = object_getClass((id)cls);
	orig_init = swap(cls, "initWithAPI:properties:", m_init);
	orig_dealloc = swap(cls, "dealloc", m_dealloc);
	orig_api = swap(cls, "API", m_api);
	orig_set_param = swap(cls, "setParameter:to:", m_set_param);
	orig_macro = swap(cls, "getMacroContextPrivate", m_macro);
	orig_tex_iosurface = swap(cls, "texImageIOSurface:target:internalFormat:width:height:format:type:plane:invert:",
	    m_tex_iosurface);
	orig_attach = swap(cls, "attachImage:toCoreSurface:invertedRender:", m_attach);
	orig_set_current = swap(meta, "setCurrentContext:", m_set_current);
	orig_current = swap(meta, "currentContext", m_current);
	sweet_drm_log("sweetgl: EAGL shim installed%s", sgl_check_errors ? " (GL error checks on)" : "");
}
