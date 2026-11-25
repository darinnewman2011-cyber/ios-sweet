/* SPDX-License-Identifier: MIT */
/*
 * jackpotlte_gpu: the GPU as a Linux-shaped userspace sees it.
 *
 * This is the layer M4 sits on.  Our kernel exposes the GPU through one IOKit
 * service, `JackpotlteMali`, whose user client answers the panfrost requests
 * of Linux's DRM (xnu/patches/0154-jackpotlte-mali-user-client.patch).  Above
 * this header sits either Mesa's panfrost - which wants drmIoctl() - or the
 * Metal plug-in `0155` names, and both want the same things: buffers with GPU
 * addresses, a mapping of their memory, submit a job chain, wait for it.
 *
 * So this is that surface, without libdrm and without a file descriptor:
 *
 *   jt_gpu_open                 find the service, open a connection
 *   jt_bo_create                a buffer, plus the GPU address it got
 *   jt_bo_map                   its memory in this process
 *   jt_bo_release               give it back
 *   jt_submit                   a job chain; returns the job's number
 *   jt_wait_bo / jt_wait_seq    and waiting for it (Darwin has no sync objects)
 *   jt_get_param                what the GPU is (id, masks, TLS alloc, ...)
 *   jt_surface_buffer           a SweetSurface's pixels as a buffer
 *
 * Everything below the `struct jt_backend` line is a vtable, so the whole
 * layer runs on a build machine against a fake IOKit (test_gpu_host.c) and the
 * requests it builds are checked there.  That is deliberate: the phone is the
 * only place this can run for real, and every mistake costs a flash.
 *
 * No Darwin headers are included here; the iOS backend declares its own
 * externs, the same way maliprobe does.
 */
#ifndef JACKPOTLTE_GPU_H
#define JACKPOTLTE_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The ABI.  Every number here mirrors xnu/patches/0154 (and the get_param
 * values 0153).  The kernel is the authority; userspace.yml greps the numbers
 * back out of this header against the patch so the two cannot drift apart
 * quietly.
 */
enum {
	JT_DRM_GEM_CLOSE         = 0x09,
	JT_PANFROST_SUBMIT       = 0x40,
	JT_PANFROST_WAIT_BO      = 0x41,
	JT_PANFROST_CREATE_BO    = 0x42,
	JT_PANFROST_MMAP_BO      = 0x43,
	JT_PANFROST_GET_PARAM    = 0x44,
	JT_PANFROST_GET_BO_OFFSET = 0x45,
	JT_PANFROST_MADVISE      = 0x48,
	JT_WAIT                  = 0x7d,
	JT_SURFACE               = 0x7e,
};

/* PANFROST_JD_REQ_FS: the job chain is a fragment shader. */
#define JT_JD_REQ_FS             (1u << 0)

/* struct drm_gem_close */
struct jt_gem_close {
	uint32_t handle, pad;
};

/* struct drm_panfrost_submit.  `bo_handles` is a pointer to the caller's
 * array of handles; the kernel copyin()s it (0154:531). */
struct jt_submit_req {
	uint64_t jc;                    /* in: the job chain; out: the job's number */
	uint64_t in_syncs;              /* unused by this kernel */
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};

/* struct drm_panfrost_wait_bo */
struct jt_wait_bo_req {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

/* struct drm_panfrost_create_bo */
struct jt_create_bo_req {
	uint32_t size, flags, handle, pad;
	uint64_t offset;                /* out: the buffer's GPU address */
};

/* struct drm_panfrost_mmap_bo: the kernel answers `offset = handle << 12`. */
struct jt_mmap_bo_req {
	uint32_t handle, flags;
	uint64_t offset;
};

/* struct drm_panfrost_get_param */
struct jt_get_param_req {
	uint32_t param, pad;
	uint64_t value;
};

/* struct drm_panfrost_get_bo_offset */
struct jt_get_bo_offset_req {
	uint32_t handle, pad;
	uint64_t offset;
};

/* struct drm_panfrost_madvise */
struct jt_madvise_req {
	uint32_t handle, madv, retained;        /* out: 1 - the memory was kept */
};

/* struct jackpotlte_wait: our own, since a Darwin client has no sync objects. */
struct jt_wait_req {
	uint64_t seq;
	int64_t timeout_ns;
};

/* struct jackpotlte_surface (0154:82): a SweetSurface's pixels as a buffer. */
struct jt_surface_req {
	uint32_t surface;                       /* the SweetSurface's ID */
	uint32_t handle;                        /* JT_SURFACE_DESCRIBE: describe only */
	uint64_t size;
	uint32_t offset, rowBytes;
	uint32_t width, height, pixelFormat, planes;    /* in and out: what they are */
};
#define JT_SURFACE_DESCRIBE     0xffffffffu

/* What jt_get_param answers (0153:259): the numbers panfrost asks with. */
enum {
	JT_PARAM_GPU_ID_HI       = 0,   /* product id, upper 16 bits */
	JT_PARAM_GPU_ID_LO       = 1,
	JT_PARAM_SHADER_PRESENT  = 2,   /* present / ready / tiler masks, a 64-bit */
	JT_PARAM_SHADER_READY    = 3,   /* value each, from a pair of registers */
	JT_PARAM_TILER_PRESENT   = 4,   /* (0153:265-269) */
	JT_PARAM_NR_CORE_GROUPS  = 38,
	JT_PARAM_THREAD_TLS_ALLOC = 39,
	JT_PARAM_AFBC_FEATURES   = 40,
	JT_PARAM_FIRST_REGISTER  = 5,   /* 5..21: single registers */
	JT_PARAM_FIRST_READ      = 22,  /* 22..37: pairs, low half then high */
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The backend: two ways into IOKit, which is all this layer needs of it.
 * `call` runs one externalMethod with its structure in and out; `map` maps a
 * buffer's client memory.  Both return 0 or a raw kIOReturn, so the layer can
 * translate them to errno in one place.
 */
#define JT_IORETURN_SUCCESS       0x00000000u
#define JT_IORETURN_NO_MEMORY     0xe00002c0u
#define JT_IORETURN_BAD_ARGUMENT  0xe00002c2u
#define JT_IORETURN_NOT_FOUND     0xe00002c4u
#define JT_IORETURN_UNSUPPORTED   0xe00002c7u
#define JT_IORETURN_NOT_READY     0xe00002c9u
#define JT_IORETURN_TIMEOUT       0xe00002d6u
#define JT_IORETURN_ERROR         0xe00002bcu

struct jt_backend {
	/* One externalMethod; the structure goes in and comes back in `data`. */
	uint32_t (*call)(void *ctx, uint32_t selector, void *data, size_t size);
	/* The buffer's memory, mapped into this process. */
	uint32_t (*map)(void *ctx, uint32_t handle, void **address, uint64_t *size);
	/* And unmapped again.  Darwin needs the type back to find the mapping,
	 * so the handle travels here too. */
	void     (*unmap)(void *ctx, uint32_t handle, void *address, uint64_t size);
	void     (*close)(void *ctx);
};

struct jt_gpu;

/* Opens the GPU over `backend`, whose per-connection state is `ctx` (the iOS
 * one is jt_iokit_open() in jackpotlte_iokit.c, which makes the ctx itself;
 * the host test passes its fake).  Returns 0 or -errno.  `ctx` may be NULL for
 * a backend that keeps its state elsewhere. */
int  jt_gpu_open(struct jt_gpu **out, const struct jt_backend *backend, void *ctx);
void jt_gpu_close(struct jt_gpu *g);

int  jt_bo_create(struct jt_gpu *g, uint64_t size, uint32_t *handle, uint64_t *gpu_address);
int  jt_bo_offset(struct jt_gpu *g, uint32_t handle, uint64_t *gpu_address);
int  jt_bo_map(struct jt_gpu *g, uint32_t handle, void **address, uint64_t *size);
int  jt_bo_unmap(struct jt_gpu *g, uint32_t handle, void *address, uint64_t size);
int  jt_bo_madvise(struct jt_gpu *g, uint32_t handle, uint32_t madv, uint32_t *retained);
int  jt_bo_release(struct jt_gpu *g, uint32_t handle);

int  jt_get_param(struct jt_gpu *g, uint32_t which, uint64_t *value);

uint64_t jt_submit(struct jt_gpu *g, uint64_t chain, bool fragment,
    const uint32_t *handles, uint32_t count, int *error);
int  jt_wait_bo(struct jt_gpu *g, uint32_t handle, int64_t timeout_ns);
int  jt_wait_seq(struct jt_gpu *g, uint64_t seq, int64_t timeout_ns);

int  jt_surface_buffer(struct jt_gpu *g, uint32_t surface, uint32_t *handle,
    struct jt_surface_req *info);

/* The errno the last failure maps to, for a caller that wants it directly. */
int  jt_errno(uint32_t kreturn);

/* The iOS backend (jackpotlte_iokit.c): find the `JackpotlteMali` service,
 * open it, and answer with the GPU over it.  Declared here so both halves of
 * the library and everything that links them share one header. */
int  jt_iokit_open(struct jt_gpu **out);

/*
 * The structures, checked where they are declared.  These are the same numbers
 * userspace.yml greps out of xnu/patches/0154, so a drift on either side is a
 * compile error here rather than a wrong request on the phone.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct jt_gem_close) == 8, "0154: drm_gem_close");
_Static_assert(sizeof(struct jt_submit_req) == 40, "0154: drm_panfrost_submit");
_Static_assert(sizeof(struct jt_wait_bo_req) == 16, "0154: drm_panfrost_wait_bo");
_Static_assert(sizeof(struct jt_create_bo_req) == 24, "0154: drm_panfrost_create_bo");
_Static_assert(offsetof(struct jt_create_bo_req, offset) == 16, "0154: create_bo.offset");
_Static_assert(sizeof(struct jt_mmap_bo_req) == 16, "0154: drm_panfrost_mmap_bo");
_Static_assert(sizeof(struct jt_get_param_req) == 16, "0154: drm_panfrost_get_param");
_Static_assert(sizeof(struct jt_get_bo_offset_req) == 16, "0154: drm_panfrost_get_bo_offset");
_Static_assert(sizeof(struct jt_madvise_req) == 12, "0154: drm_panfrost_madvise");
_Static_assert(sizeof(struct jt_wait_req) == 16, "0154: jackpotlte_wait");
_Static_assert(sizeof(struct jt_surface_req) == 40, "0154: jackpotlte_surface");
_Static_assert(offsetof(struct jt_surface_req, handle) == 4, "0154: surface.handle");
_Static_assert(offsetof(struct jt_surface_req, size) == 8, "0154: surface.size");
#endif

#endif /* JACKPOTLTE_GPU_H */
