/* SPDX-License-Identifier: MIT */
/*
 * jackpotlte_gpu: the request layer.  See jackpotlte_gpu.h for what it is for.
 *
 * Nothing here knows what IOKit is: it builds the structures the kernel reads
 * (0154), hands them to the backend, and unpacks what comes back.  That is what
 * test_gpu_host.c checks, and it is the part that must not drift from 0154.
 */
#include "jackpotlte_gpu.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct jt_gpu {
	const struct jt_backend *backend;
	void                    *ctx;
};

/*
 * kIOReturn -> errno, once, in the shape a Linux-shaped caller expects: an
 * unsupported request is -ENOTTY because that is what an unknown ioctl is.
 */
int
jt_errno(uint32_t kreturn)
{
	switch (kreturn) {
	case JT_IORETURN_SUCCESS:      return 0;
	case JT_IORETURN_NO_MEMORY:    return -ENOMEM;
	case JT_IORETURN_BAD_ARGUMENT: return -EINVAL;
	case JT_IORETURN_NOT_FOUND:    return -ENOENT;
	case JT_IORETURN_UNSUPPORTED:  return -ENOTTY;
	case JT_IORETURN_NOT_READY:    return -ENODEV;
	case JT_IORETURN_TIMEOUT:      return -ETIMEDOUT;
	case JT_IORETURN_ERROR:        return -EIO;
	default:                       return -EIO;
	}
}

/* One request: the same structure in and out, as externalMethod() does it. */
static int
request(struct jt_gpu *g, uint32_t selector, void *data, size_t size)
{
	return jt_errno(g->backend->call(g->ctx, selector, data, size));
}

int
jt_gpu_open(struct jt_gpu **out, const struct jt_backend *backend, void *ctx)
{
	struct jt_gpu *g;

	if (out == NULL || backend == NULL || backend->call == NULL ||
	    backend->map == NULL || backend->unmap == NULL || backend->close == NULL) {
		return -EINVAL;
	}
	g = calloc(1, sizeof(*g));
	if (g == NULL) {
		return -ENOMEM;
	}
	g->backend = backend;
	g->ctx = ctx;
	*out = g;
	return 0;
}

void
jt_gpu_close(struct jt_gpu *g)
{
	if (g == NULL) {
		return;
	}
	g->backend->close(g->ctx);
	free(g);
}

/*
 * A buffer.  The kernel rounds the size up to its page (16 KB) and allocates
 * it in kernel_task as kIOMemoryKernelUserShared, so the same memory is the
 * GPU's and this process's once it is mapped (0154:294-317).  The GPU address
 * that comes back is where the buffer lives in the GPU's own address space,
 * which is the number a job chain has to name.
 */
int
jt_bo_create(struct jt_gpu *g, uint64_t size, uint32_t *handle, uint64_t *gpu_address)
{
	struct jt_create_bo_req r;
	int ret;

	if (g == NULL || size == 0 || size > 0xffffffffull) {
		return -EINVAL;
	}
	memset(&r, 0, sizeof(r));
	r.size = (uint32_t)size;
	ret = request(g, JT_PANFROST_CREATE_BO, &r, sizeof(r));
	if (ret != 0) {
		return ret;
	}
	if (r.handle == 0 || r.offset == 0) {
		/* A buffer with no handle or no address is not usable, and both
		 * mean the kernel and this layer disagree about the answer. */
		return -EIO;
	}
	if (handle != NULL) {
		*handle = r.handle;
	}
	if (gpu_address != NULL) {
		*gpu_address = r.offset;
	}
	return 0;
}

/* What address the kernel thinks the buffer has.  Cheap; panfrost asks too. */
int
jt_bo_offset(struct jt_gpu *g, uint32_t handle, uint64_t *gpu_address)
{
	struct jt_get_bo_offset_req r;
	int ret;

	if (g == NULL || handle == 0) {
		return -EINVAL;
	}
	memset(&r, 0, sizeof(r));
	r.handle = handle;
	ret = request(g, JT_PANFROST_GET_BO_OFFSET, &r, sizeof(r));
	if (ret == 0 && gpu_address != NULL) {
		*gpu_address = r.offset;
	}
	return ret;
}

/*
 * The buffer's memory in this process.  Darwin has no mmap-by-offset for a
 * driver's memory, so the handle itself is the client-memory type
 * (0154:422-437); kPanfrostMmapBO's `offset = handle << 12` exists only so a
 * drm-shaped layer can turn a handle back into one.
 */
int
jt_bo_map(struct jt_gpu *g, uint32_t handle, void **address, uint64_t *size)
{
	void *addr = NULL;
	uint64_t len = 0;
	int ret;

	if (g == NULL || handle == 0 || address == NULL) {
		return -EINVAL;
	}
	ret = jt_errno(g->backend->map(g->ctx, handle, &addr, &len));
	if (ret != 0) {
		return ret;
	}
	if (addr == NULL || len == 0) {
		return -ENOMEM;
	}
	*address = addr;
	if (size != NULL) {
		*size = len;
	}
	return 0;
}

int
jt_bo_unmap(struct jt_gpu *g, uint32_t handle, void *address, uint64_t size)
{
	if (g == NULL || handle == 0 || address == NULL) {
		return -EINVAL;
	}
	g->backend->unmap(g->ctx, handle, address, size);
	return 0;
}

/* Ask the kernel to drop the memory, and read back whether it did.  This
 * kernel keeps it (retained = 1, 0154:551), which is a valid answer: panfrost
 * then keeps the buffer rather than assuming it was freed. */
int
jt_bo_madvise(struct jt_gpu *g, uint32_t handle, uint32_t madv, uint32_t *retained)
{
	struct jt_madvise_req r;
	int ret;

	if (g == NULL || handle == 0) {
		return -EINVAL;
	}
	memset(&r, 0, sizeof(r));
	r.handle = handle;
	r.madv = madv;
	ret = request(g, JT_PANFROST_MADVISE, &r, sizeof(r));
	if (ret == 0 && retained != NULL) {
		*retained = r.retained;
	}
	return ret;
}

/*
 * Give the buffer back.  The kernel frees the memory and the GPU address with
 * it; a mapped buffer must be unmapped first (jt_bo_unmap), or the caller's
 * mapping outlives the buffer.
 */
int
jt_bo_release(struct jt_gpu *g, uint32_t handle)
{
	struct jt_gem_close r;

	if (g == NULL || handle == 0) {
		return -EINVAL;
	}
	memset(&r, 0, sizeof(r));
	r.handle = handle;
	return request(g, JT_DRM_GEM_CLOSE, &r, sizeof(r));
}

int
jt_get_param(struct jt_gpu *g, uint32_t which, uint64_t *value)
{
	struct jt_get_param_req r;
	int ret;

	if (g == NULL) {
		return -EINVAL;
	}
	memset(&r, 0, sizeof(r));
	r.param = which;
	ret = request(g, JT_PANFROST_GET_PARAM, &r, sizeof(r));
	if (ret == 0 && value != NULL) {
		*value = r.value;
	}
	return ret;
}

/*
 * Hand the GPU a job chain and come back with its number.
 *
 * The chain is GPU memory the caller built; `handles` names the buffers the
 * chain reads and writes, so the kernel can keep them alive for as long as the
 * job runs (0154:527-540).  It is the caller's array, and only its address
 * travels - fitting, because the kernel copyin()s it.
 *
 * Returns the job's number, or 0 with *error set: 0 is never a valid number.
 */
uint64_t
jt_submit(struct jt_gpu *g, uint64_t chain, bool fragment,
    const uint32_t *handles, uint32_t count, int *error)
{
	struct jt_submit_req r;
	int ret;

	if (error != NULL) {
		*error = 0;
	}
	if (g == NULL || chain == 0 || (count != 0 && handles == NULL)) {
		if (error != NULL) {
			*error = -EINVAL;
		}
		return 0;
	}
	memset(&r, 0, sizeof(r));
	r.jc = chain;
	r.bo_handles = (uint64_t)(uintptr_t)handles;
	r.bo_handle_count = count;
	r.requirements = fragment ? JT_JD_REQ_FS : 0;
	ret = request(g, JT_PANFROST_SUBMIT, &r, sizeof(r));
	if (ret != 0) {
		if (error != NULL) {
			*error = ret;
		}
		return 0;
	}
	if (r.jc == 0) {
		/* The kernel queues and returns the job's number in `jc`; 0 means
		 * it did not (0154:521-541). */
		if (error != NULL) {
			*error = -EIO;
		}
		return 0;
	}
	return r.jc;
}

/* Wait until the job that last used `handle` has finished. */
int
jt_wait_bo(struct jt_gpu *g, uint32_t handle, int64_t timeout_ns)
{
	struct jt_wait_bo_req r;

	if (g == NULL || handle == 0) {
		return -EINVAL;
	}
	memset(&r, 0, sizeof(r));
	r.handle = handle;
	r.timeout_ns = timeout_ns;
	return request(g, JT_PANFROST_WAIT_BO, &r, sizeof(r));
}

/* Wait for one job by its number. */
int
jt_wait_seq(struct jt_gpu *g, uint64_t seq, int64_t timeout_ns)
{
	struct jt_wait_req r;

	if (g == NULL) {
		return -EINVAL;
	}
	memset(&r, 0, sizeof(r));
	r.seq = seq;
	r.timeout_ns = timeout_ns;
	return request(g, JT_WAIT, &r, sizeof(r));
}

/*
 * A SweetSurface's pixels as a buffer - the display's own memory, which is
 * what makes presenting without a copy possible later.  With handle =
 * JT_SURFACE_DESCRIBE only the description comes back and no buffer is made.
 */
int
jt_surface_buffer(struct jt_gpu *g, uint32_t surface, uint32_t *handle,
    struct jt_surface_req *info)
{
	struct jt_surface_req r;
	int ret;

	if (g == NULL || surface == 0 || info == NULL) {
		return -EINVAL;
	}
	memset(&r, 0, sizeof(r));
	r.surface = surface;
	r.handle = handle != NULL ? *handle : JT_SURFACE_DESCRIBE;
	ret = request(g, JT_SURFACE, &r, sizeof(r));
	if (ret != 0) {
		return ret;
	}
	if (handle != NULL) {
		*handle = r.handle;
	}
	*info = r;
	return 0;
}
