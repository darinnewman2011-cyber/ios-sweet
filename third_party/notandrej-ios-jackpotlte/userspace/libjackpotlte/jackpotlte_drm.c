/* SPDX-License-Identifier: MIT */
/*
 * jackpotlte_drm: Linux's DRM requests over our kernel's GPU user client.
 *
 * See jackpotlte_drm.h for what this is.  The shape of it is the reference's
 * (hakios/mtk tools/mesa/redwood-drm): one connection in the process, a request
 * number's low byte as the call's selector, and sync objects kept here because
 * a Darwin client has none.  What is different is that the structures are
 * translated instead of passed through, because our kernel reads exactly the
 * 40-byte structures in 0154 and refuses a request larger than that.
 */
#include "jackpotlte_drm.h"
#include "jackpotlte_gpu.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Two of Linux's panfrost requests that our kernel does not answer (0154 has
 * no counters).  They are named here so the refusal is explicit and costs no
 * round trip.
 */
#define JT_PERFCNT_ENABLE       0x46
#define JT_PERFCNT_DUMP         0x47

/* drmCommandWrite(index): Linux's DRM_COMMAND_BASE, which libdrm adds. */
#define JT_DRM_COMMAND_BASE     0x40

/* Buffers and sync objects by handle.  Both are per-connection state, and a
 * handle at or past the end simply is not remembered (0154's handles are small;
 * MALI_BUFFERS is 4096). */
#define JT_MAPS                 4096
#define JT_SYNCOBJS             1024

#define JT_REQUEST_SIZE(request) ((size_t) (((request) >> 16) & 0x3ffful))

/*
 * The one GPU and the fd Mesa holds for it.  `device_fd` is /dev/null: the fd
 * means nothing here, but Mesa stores it and hands it back, and closing it must
 * not close the connection, so it is opened once and dup()ed per open.
 */
static struct jt_gpu *gpu;
static int device_fd = -1;
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;

/* A buffer's mapping, kept until the buffer is given back. */
static struct { void *address; uint64_t size; } maps[JT_MAPS];

/*
 * Sync objects, kept in this process over the kernel's job numbers.  A job is
 * submitted synchronously (0154: "every job has finished before its submission
 * returned"), so a sync object is a number to wait for and nothing else: what
 * panfrost needs is that its out_sync means its job, and that waiting on it
 * waits for that job.
 */
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t syncobj_seq[JT_SYNCOBJS];
static uint64_t syncfile_seq[JT_SYNCOBJS];
static uint64_t last_seq;
static uint32_t next_syncobj = 1;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The connection.
 */
struct jt_gpu *
jackpotlte_drm_gpu(void)
{
	struct jt_gpu *g;

	pthread_mutex_lock(&gpu_lock);
	if (gpu == NULL) {
#ifdef JACKPOTLTE_DRM_IOS
		struct jt_gpu *opened = NULL;

		if (jt_iokit_open(&opened) == 0) {
			gpu = opened;
		}
#endif
	}
	g = gpu;
	pthread_mutex_unlock(&gpu_lock);
	return g;
}

void
jackpotlte_drm_attach(struct jt_gpu *g)
{
	struct jt_gpu *old;

	pthread_mutex_lock(&gpu_lock);
	old = gpu;
	gpu = g;
	for (uint32_t i = 0; i < JT_MAPS; i++) {
		if (maps[i].address == NULL) {
			continue;
		}
		/* The previous connection's mappings go with it. */
		if (old != NULL && old != g) {
			(void)jt_bo_unmap(old, i, maps[i].address, maps[i].size);
		}
		maps[i].address = NULL;
		maps[i].size = 0;
	}
	pthread_mutex_unlock(&gpu_lock);

	pthread_mutex_lock(&sync_lock);
	memset(syncobj_seq, 0, sizeof(syncobj_seq));
	memset(syncfile_seq, 0, sizeof(syncfile_seq));
	last_seq = 0;
	next_syncobj = 1;
	pthread_mutex_unlock(&sync_lock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The mappings.  A buffer's memory is mapped by its handle (0154's
 * clientMemoryForType), and unmapped only when the buffer goes, so panfrost's
 * map/unmap of every use of its cache does not call the kernel each time.
 */
static void
maps_forget(uint32_t handle, void **address, uint64_t *size)
{
	*address = NULL;
	*size = 0;
	if (handle >= JT_MAPS) {
		return;
	}
	pthread_mutex_lock(&gpu_lock);
	*address = maps[handle].address;
	*size = maps[handle].size;
	maps[handle].address = NULL;
	maps[handle].size = 0;
	pthread_mutex_unlock(&gpu_lock);
}

void *
jackpotlte_drm_map(int fd, uint32_t handle, uint64_t size)
{
	struct jt_gpu *g = jackpotlte_drm_gpu();
	void *address = NULL;
	uint64_t mapped = 0;
	int ret;

	(void)fd;
	if (g == NULL) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	pthread_mutex_lock(&gpu_lock);
	if (handle < JT_MAPS && maps[handle].address != NULL && maps[handle].size >= size) {
		address = maps[handle].address;
		pthread_mutex_unlock(&gpu_lock);
		return address;
	}
	if (handle < JT_MAPS && maps[handle].address != NULL) {
		/* A smaller mapping than is wanted: give it back and map again. */
		(void)jt_bo_unmap(g, handle, maps[handle].address, maps[handle].size);
		maps[handle].address = NULL;
		maps[handle].size = 0;
	}
	ret = jt_bo_map(g, handle, &address, &mapped);
	if (ret == 0 && handle < JT_MAPS) {
		maps[handle].address = address;
		maps[handle].size = mapped;
	}
	pthread_mutex_unlock(&gpu_lock);
	if (ret != 0) {
		errno = -ret;
		return MAP_FAILED;
	}
	return address;
}

void
jackpotlte_drm_unmap(int fd, uint32_t handle, void *address)
{
	struct jt_gpu *g;

	(void)fd;
	if (handle < JT_MAPS) {
		return;         /* kept for the next map: maps_forget at GEM_CLOSE */
	}
	g = jackpotlte_drm_gpu();
	if (g != NULL && address != NULL) {
		(void)jt_bo_unmap(g, handle, address, 0);
	}
}

/* Give a buffer back: its mapping first, then the buffer and its address. */
static int
buffer_close(struct jt_gpu *g, uint32_t handle)
{
	void *address = NULL;
	uint64_t size = 0;

	if (handle == 0) {
		return -EINVAL;
	}
	maps_forget(handle, &address, &size);
	if (address != NULL) {
		(void)jt_bo_unmap(g, handle, address, size);
	}
	return jt_bo_release(g, handle);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The requests, one function each.  Each reads the fields the kernel reads and
 * writes back the ones it answers; 0154 is the authority for both.
 */
static int
gem_close(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_gem_close *request = arg;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	return buffer_close(g, request->handle);
}

static int
submit(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_submit_req *request = arg;    /* Linux's struct's first 40 bytes */
	const uint32_t *handles;
	uint64_t seq;
	int error = 0;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	handles = (const uint32_t *) (uintptr_t) request->bo_handles;
	/* The kernel copyin()s the handle array itself, so only its address
	 * travels (0154:527-540).  It reads just the fragment bit of
	 * `requirements`. */
	seq = jt_submit(g, request->jc, (request->requirements & JT_JD_REQ_FS) != 0,
	    request->bo_handle_count != 0 ? handles : NULL, request->bo_handle_count, &error);
	if (seq == 0) {
		return error != 0 ? error : -EIO;
	}
	__atomic_store_n(&last_seq, seq, __ATOMIC_RELEASE);
	if (request->out_sync != 0) {
		pthread_mutex_lock(&sync_lock);
		syncobj_seq[request->out_sync % JT_SYNCOBJS] = seq;
		pthread_mutex_unlock(&sync_lock);
	}
	request->jc = seq;
	return 0;
}

static int
wait_bo(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_wait_bo_req *request = arg;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	return jt_wait_bo(g, request->handle, request->timeout_ns);
}

static int
create_bo(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_create_bo_req *request = arg;
	uint32_t handle = 0;
	uint64_t address = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	ret = jt_bo_create(g, request->size, &handle, &address);
	if (ret != 0) {
		return ret;
	}
	request->handle = handle;
	request->offset = address;
	return 0;
}

static int
mmap_bo(void *arg, size_t size)
{
	struct jt_mmap_bo_req *request = arg;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	/* Darwin maps a buffer by its handle, not by an offset.  0154 answers
	 * Linux's offset anyway - the handle shifted 12 (0154:505-508) - and
	 * nothing here hands it to mmap(): jackpotlte_drm_map does the mapping. */
	request->offset = (uint64_t) request->handle << 12;
	return 0;
}

static int
get_param(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_get_param_req *request = arg;
	uint64_t value = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	ret = jt_get_param(g, request->param, &value);
	if (ret != 0) {
		return ret;
	}
	request->value = value;
	return 0;
}

static int
get_bo_offset(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_get_bo_offset_req *request = arg;
	uint64_t address = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	ret = jt_bo_offset(g, request->handle, &address);
	if (ret != 0) {
		return ret;
	}
	request->offset = address;
	return 0;
}

static int
bo_madvise(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_madvise_req *request = arg;
	uint32_t retained = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	/* The kernel keeps every buffer and says so (0154:551); panfrost reads
	 * that answer to decide whether the buffer still has its memory. */
	ret = jt_bo_madvise(g, request->handle, request->madv, &retained);
	if (ret != 0) {
		return ret;
	}
	request->retained = retained;
	return 0;
}

static int
wait_seq(struct jt_gpu *g, void *arg, size_t size)
{
	struct jt_wait_req *request = arg;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	return jt_wait_seq(g, request->seq, request->timeout_ns);
}

static int
surface(struct jt_gpu *g, void *arg, size_t size)
{
	struct jackpotlte_drm_surface *request = arg;
	struct jt_surface_req info;
	uint32_t handle = 0;
	int ret;

	if (arg == NULL || size < sizeof(*request)) {
		return -EINVAL;
	}
	handle = request->handle;
	ret = jt_surface_buffer(g, request->surface,
	    request->handle == JACKPOTLTE_DRM_DESCRIBE ? NULL : &handle, &info);
	if (ret != 0) {
		return ret;
	}
	request->handle = info.handle;
	request->size = info.size;
	request->offset = info.offset;
	request->row_bytes = info.rowBytes;
	request->width = info.width;
	request->height = info.height;
	request->pixel_format = info.pixelFormat;
	request->planes = info.planes;
	return 0;
}

int
jackpotlte_drm_ioctl(struct jt_gpu *g, unsigned long request, void *arg, size_t size)
{
	unsigned int number = (unsigned int) (request & 0xff);

	if (g == NULL) {
		return -ENODEV;
	}
	switch (number) {
	case JT_DRM_GEM_CLOSE:          return gem_close(g, arg, size);
	case JT_PANFROST_SUBMIT:        return submit(g, arg, size);
	case JT_PANFROST_WAIT_BO:       return wait_bo(g, arg, size);
	case JT_PANFROST_CREATE_BO:     return create_bo(g, arg, size);
	case JT_PANFROST_MMAP_BO:       return mmap_bo(arg, size);
	case JT_PANFROST_GET_PARAM:     return get_param(g, arg, size);
	case JT_PANFROST_GET_BO_OFFSET: return get_bo_offset(g, arg, size);
	case JT_PANFROST_MADVISE:       return bo_madvise(g, arg, size);
	case JT_SURFACE:                return surface(g, arg, size);
	case JT_WAIT:                   return wait_seq(g, arg, size);
	case JT_PERFCNT_ENABLE:         /* our driver does not count, */
	case JT_PERFCNT_DUMP:           /* and has nothing to dump */
	case 0x0a:                      /* JM_CTX_CREATE: the kernel has one context */
	case 0x0b:                      /* JM_CTX_DESTROY: and it lives as long as the client */
		return -ENOTTY;
	default:
		return -ENOTTY;
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * What Mesa calls.
 */
int
drmOpenWithType(const char *name, const char *busid, int type)
{
	int fd;

	(void)busid;
	(void)type;
	if (name == NULL || strcmp(name, "panfrost") != 0) {
		/* One GPU, and it is a Mali. */
		errno = ENODEV;
		return -1;
	}
	if (jackpotlte_drm_gpu() == NULL) {
		errno = ENODEV;         /* no service: the GPU did not come up */
		return -1;
	}
	pthread_mutex_lock(&gpu_lock);
	if (device_fd < 0) {
		device_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	}
	fd = device_fd >= 0 ? dup(device_fd) : -1;
	pthread_mutex_unlock(&gpu_lock);
	return fd;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error;

	(void)fd;
	error = jackpotlte_drm_ioctl(jackpotlte_drm_gpu(), request, arg,
	    JT_REQUEST_SIZE(request));
	if (error != 0) {
		errno = -error;
		return -1;
	}
	return 0;
}

/*
 * libdrm's drmCommandWrite(index): `index` is the command's number without
 * DRM_COMMAND_BASE, and the size is the structure's.  Returns -errno, as
 * libdrm does (not -1).
 */
static int
command(unsigned long index, void *data, unsigned long size)
{
	return jackpotlte_drm_ioctl(jackpotlte_drm_gpu(),
	    JACKPOTLTE_DRM_REQUEST(JT_DRM_COMMAND_BASE + index, size), data, (size_t) size);
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return command(index, (void *) (uintptr_t) data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return command(index, data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose
 * interface it has: 1.2, buffers at addresses the kernel chooses and tells
 * ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version == NULL) {
		return NULL;
	}
	version->version_major = 1;
	version->version_minor = 2;
	version->name = strdup("panfrost");
	version->date = strdup("");
	version->desc = strdup("Mali on Darwin");
	if (version->name == NULL || version->date == NULL || version->desc == NULL) {
		drmFreeVersion(version);
		return NULL;
	}
	version->name_len = (int) strlen(version->name);
	version->date_len = (int) strlen(version->date);
	version->desc_len = (int) strlen(version->desc);
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version == NULL) {
		return;
	}
	free(version->name);
	free(version->date);
	free(version->desc);
	free(version);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	if (value != NULL) {
		*value = 0;
	}
	return 0;
}

/* A SweetSurface's pixels as a buffer, or only what they would be like. */
int
jackpotlte_drm_surface(int fd, struct jackpotlte_drm_surface *surface)
{
	int error;

	(void)fd;
	if (surface == NULL) {
		return -EINVAL;
	}
	error = jackpotlte_drm_ioctl(jackpotlte_drm_gpu(),
	    JACKPOTLTE_DRM_REQUEST(JT_SURFACE, sizeof(*surface)), surface, sizeof(*surface));
	if (error != 0) {
		errno = -error;
		return -1;
	}
	return 0;
}

/* panfrost imports a surface as a dmabuf: its ID is the "descriptor". */
int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct jackpotlte_drm_surface request;
	int error;

	(void)fd;
	if (handle == NULL) {
		return -EINVAL;
	}
	memset(&request, 0, sizeof(request));
	request.surface = (uint32_t) surface;
	error = jackpotlte_drm_ioctl(jackpotlte_drm_gpu(),
	    JACKPOTLTE_DRM_REQUEST(JT_SURFACE, sizeof(request)), &request, sizeof(request));
	if (error != 0) {
		errno = -error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

/* Buffers as descriptors, for handing one to another program or driver: none. */
int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	(void)prime_fd;
	errno = ENOSYS;
	return -1;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct jt_gem_close request;

	(void)fd;
	memset(&request, 0, sizeof(request));
	request.handle = handle;
	return jackpotlte_drm_ioctl(jackpotlte_drm_gpu(),
	    JACKPOTLTE_DRM_REQUEST(JT_DRM_GEM_CLOSE, sizeof(request)), &request, sizeof(request));
}

uint64_t
jackpotlte_drm_last_seq(void)
{
	return __atomic_load_n(&last_seq, __ATOMIC_ACQUIRE);
}

int
jackpotlte_drm_wait_seq(uint64_t seq, int64_t relative_ns)
{
	struct jt_gpu *g;

	if (seq == 0) {
		return 0;               /* nothing was submitted: nothing to wait for */
	}
	g = jackpotlte_drm_gpu();
	if (g == NULL) {
		return -ENODEV;
	}
	return jt_wait_seq(g, seq, relative_ns);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Sync objects.  A job finishes before its submission returns, so every one of
 * them is signalled by the time it can be asked about; each is the number of
 * the job it was signalled with, and waiting is the kernel's wait for it.
 */
static int
syncobj_none(void)
{
	errno = ENOSYS;
	return -ENOSYS;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	(void)flags;
	if (handle == NULL) {
		return -EINVAL;
	}
	pthread_mutex_lock(&sync_lock);
	*handle = next_syncobj;
	if (++next_syncobj == 0) {
		next_syncobj = 1;       /* 0 means "not a sync object" */
	}
	syncobj_seq[*handle % JT_SYNCOBJS] = 0;
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	pthread_mutex_lock(&sync_lock);
	syncobj_seq[handle % JT_SYNCOBJS] = 0;
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	if (handles == NULL) {
		return -EINVAL;
	}
	pthread_mutex_lock(&sync_lock);
	for (uint32_t i = 0; i < count; i++) {
		syncobj_seq[handles[i] % JT_SYNCOBJS] = 0;
	}
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	(void)fd;
	(void)points;
	if (handles == NULL) {
		return -EINVAL;
	}
	pthread_mutex_lock(&sync_lock);
	for (uint32_t i = 0; i < count; i++) {
		/* Every point is reached: the jobs are done.  0 is "nothing to
		 * wait for", which is what a signalled sync object is here. */
		syncobj_seq[handles[i] % JT_SYNCOBJS] = 0;
	}
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	return drmSyncobjTimelineSignal(fd, handles, NULL, count);
}

int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
	struct jt_gpu *g;

	(void)fd;
	(void)points;                   /* one point per sync object here */
	(void)flags;                    /* WAIT_ALL and WAIT_FOR_SUBMIT: all are submitted */
	if (handles == NULL) {
		return -EINVAL;
	}
	if (first_signaled != NULL) {
		*first_signaled = 0;
	}
	g = jackpotlte_drm_gpu();
	for (unsigned i = 0; i < count; i++) {
		uint64_t seq;

		pthread_mutex_lock(&sync_lock);
		seq = syncobj_seq[handles[i] % JT_SYNCOBJS];
		pthread_mutex_unlock(&sync_lock);
		if (seq == 0) {
			continue;       /* nothing was submitted with it: nothing to wait for */
		}
		if (g == NULL) {
			return -ENODEV;
		}
		int ret = jt_wait_seq(g, seq, timeout_nsec);

		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	return drmSyncobjTimelineWait(fd, handles, NULL, count, timeout_nsec, flags,
	    first_signaled);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
	(void)fd;
	(void)flags;
	if (handles == NULL || points == NULL) {
		return -EINVAL;
	}
	for (uint32_t i = 0; i < count; i++) {
		/* A binary sync object's signalled point is 1; here every one
		 * of them is signalled. */
		points[i] = 1;
	}
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	return drmSyncobjQuery2(fd, handles, points, count, 0);
}

int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags)
{
	(void)fd;
	(void)dst_point;
	(void)src_point;
	(void)flags;
	pthread_mutex_lock(&sync_lock);
	syncobj_seq[dst_handle % JT_SYNCOBJS] = syncobj_seq[src_handle % JT_SYNCOBJS];
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

/*
 * A sync object as a sync file, and back.  panfrost makes one of every flush
 * (panfrost_fence_create) and polls it; a job is done before the submission
 * returns, so a fence that is always ready can be any descriptor - /dev/null's.
 * Without this every frame of the window server had no fence at all.
 */
int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	int exported;

	(void)fd;
	if (sync_file_fd == NULL) {
		return -EINVAL;
	}
	exported = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (exported < 0) {
		return -errno;
	}
	pthread_mutex_lock(&sync_lock);
	syncfile_seq[exported % JT_SYNCOBJS] = syncobj_seq[handle % JT_SYNCOBJS];
	pthread_mutex_unlock(&sync_lock);
	*sync_file_fd = exported;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	if (sync_file_fd < 0) {
		return -EINVAL;
	}
	pthread_mutex_lock(&sync_lock);
	syncobj_seq[handle % JT_SYNCOBJS] = syncfile_seq[sync_file_fd % JT_SYNCOBJS];
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd;
	(void)obj_fd;
	(void)handle;
	return syncobj_none();
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	(void)fd;
	(void)handle;
	(void)obj_fd;
	return syncobj_none();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Linux's device nodes and device list, which the Vulkan runtime walks: one
 * GPU, a render node, and nothing that has to be enumerated.
 */
int
drmGetNodeTypeFromFd(int fd)
{
	(void)fd;
	return DRM_NODE_RENDER;
}

int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	(void)devices;
	(void)max_devices;
	return 0;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	(void)devices;
	(void)count;
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	if (device != NULL) {
		*device = NULL;
	}
	return -ENODEV;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	(void)device;
}

/* The structures are the kernel's, field for field (0154:82-95). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(struct jackpotlte_drm_surface, row_bytes) ==
    offsetof(struct jt_surface_req, rowBytes), "0154: surface.rowBytes");
_Static_assert(offsetof(struct jackpotlte_drm_surface, pixel_format) ==
    offsetof(struct jt_surface_req, pixelFormat), "0154: surface.pixelFormat");
#endif
