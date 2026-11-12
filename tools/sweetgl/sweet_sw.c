/*
 * sweet_sw: the sweet_drm.h interface without a GPU, for Mesa softpipe
 * (wp210: MT6895 with a Mali-G610 and no GPU driver).
 *
 * sweet_drm_import_iosurface() hands out a small integer "handle" that stands
 * for a CPU pointer to the IOSurface's memory. eagl.c passes it on in a
 * winsys_handle to resource_from_handle; softpipe asks the sw_winsys below
 * for a display target, which is just that pointer plus the surface pitch.
 * softpipe then reads and writes the IOSurface memory directly: no copies,
 * no kernel calls.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach/mach.h>

#include "pipe/p_state.h"
#include "frontend/sw_winsys.h"
#include "frontend/winsys_handle.h"
#include "util/format/u_format.h"
#include "util/u_math.h"
#include "sweet_drm.h"

/* ---- handle table: handle -> page-aligned base of the surface's memory ---- */

#define MAX_HANDLES 4096
static pthread_mutex_t tab_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t tab[MAX_HANDLES];      /* 0 = free; handle = index + 1 */

int
sweet_drm_open(void)
{
	return open("/dev/null", O_RDWR);
}

void *
sweet_drm_bo_map(int fd, uint64_t offset)
{
	(void)fd;
	(void)offset;
	return MAP_FAILED;
}

int
sweet_drm_import_iosurface(uint32_t surface_id, uint64_t map, uint32_t *handle, uint64_t *size)
{
	(void)surface_id;
	if (map == 0) {
		return -EINVAL;
	}
	pthread_mutex_lock(&tab_lock);
	for (uint32_t i = 0; i < MAX_HANDLES; i++) {
		if (tab[i] == 0) {
			/* eagl.c adds the in-page offset of the plane through winsys_handle.offset */
			tab[i] = (uintptr_t)(map & ~(uint64_t)vm_page_mask);
			pthread_mutex_unlock(&tab_lock);
			*handle = i + 1;
			if (size != NULL) {
				*size = 0;
			}
			return 0;
		}
	}
	pthread_mutex_unlock(&tab_lock);
	return -ENOSPC;
}

static uintptr_t
take_handle(uint32_t handle)
{
	uintptr_t base = 0;
	pthread_mutex_lock(&tab_lock);
	if (handle >= 1 && handle <= MAX_HANDLES) {
		base = tab[handle - 1];
		tab[handle - 1] = 0;
	}
	pthread_mutex_unlock(&tab_lock);
	return base;
}

/* ---- sw_winsys ---- */

struct sweet_dt {
	uint8_t *data;
	unsigned stride;
	bool owned;             /* malloc'ed by displaytarget_create */
};

static void
ws_destroy(struct sw_winsys *ws)
{
	free(ws);
}

static int
ws_get_fd(struct sw_winsys *ws)
{
	(void)ws;
	return -1;
}

static bool
ws_format_supported(struct sw_winsys *ws, unsigned tex_usage, enum pipe_format format)
{
	(void)ws;
	(void)tex_usage;
	(void)format;
	return true;
}

static struct sw_displaytarget *
ws_dt_create(struct sw_winsys *ws, unsigned tex_usage, enum pipe_format format,
    unsigned width, unsigned height, unsigned alignment, const void *front_private,
    unsigned *stride)
{
	(void)ws;
	(void)tex_usage;
	(void)front_private;
	struct sweet_dt *dt = calloc(1, sizeof(*dt));
	if (dt == NULL) {
		return NULL;
	}
	unsigned a = alignment ? alignment : 64;
	dt->stride = align(util_format_get_stride(format, width), a);
	size_t size = (size_t)dt->stride * util_format_get_nblocksy(format, height);
	dt->data = aligned_alloc(64, align(size ? size : 64, 64));
	if (dt->data == NULL) {
		free(dt);
		return NULL;
	}
	dt->owned = true;
	*stride = dt->stride;
	return (struct sw_displaytarget *)dt;
}

static struct sw_displaytarget *
ws_dt_from_handle(struct sw_winsys *ws, const struct pipe_resource *templ,
    struct winsys_handle *wh, unsigned *stride)
{
	(void)ws;
	(void)templ;
	uintptr_t base = take_handle(wh->handle);
	if (base == 0 || wh->stride == 0) {
		return NULL;
	}
	struct sweet_dt *dt = calloc(1, sizeof(*dt));
	if (dt == NULL) {
		return NULL;
	}
	dt->data = (uint8_t *)base + wh->offset;
	dt->stride = wh->stride;
	dt->owned = false;
	*stride = dt->stride;
	return (struct sw_displaytarget *)dt;
}

static bool
ws_dt_get_handle(struct sw_winsys *ws, struct sw_displaytarget *dt, struct winsys_handle *wh)
{
	(void)ws;
	(void)dt;
	(void)wh;
	return false;
}

static void *
ws_dt_map(struct sw_winsys *ws, struct sw_displaytarget *dt, unsigned flags)
{
	(void)ws;
	(void)flags;
	return ((struct sweet_dt *)dt)->data;
}

static void
ws_dt_unmap(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
	(void)ws;
	(void)dt;
}

static void
ws_dt_display(struct sw_winsys *ws, struct sw_displaytarget *dt, void *context_private,
    unsigned nboxes, struct pipe_box *box)
{
	(void)ws;
	(void)dt;
	(void)context_private;
	(void)nboxes;
	(void)box;
}

static void
ws_dt_destroy(struct sw_winsys *ws, struct sw_displaytarget *dt)
{
	(void)ws;
	struct sweet_dt *d = (struct sweet_dt *)dt;
	if (d->owned) {
		free(d->data);
	}
	free(d);
}

struct sw_winsys *
sweet_sw_winsys_create(void)
{
	struct sw_winsys *ws = calloc(1, sizeof(*ws));
	if (ws == NULL) {
		return NULL;
	}
	ws->destroy = ws_destroy;
	ws->get_fd = ws_get_fd;
	ws->is_displaytarget_format_supported = ws_format_supported;
	ws->displaytarget_create = ws_dt_create;
	ws->displaytarget_from_handle = ws_dt_from_handle;
	ws->displaytarget_get_handle = ws_dt_get_handle;
	ws->displaytarget_map = ws_dt_map;
	ws->displaytarget_unmap = ws_dt_unmap;
	ws->displaytarget_display = ws_dt_display;
	ws->displaytarget_destroy = ws_dt_destroy;
	return ws;
}
