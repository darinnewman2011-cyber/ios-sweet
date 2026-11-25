/* SPDX-License-Identifier: MIT */
/*
 * test_gpu_host: check the request layer against the ABI, on a build machine.
 *
 * There is no GPU here and no IOKit: a fake backend records the selector, the
 * size and the bytes that jt_gpu hands it, and answers the way 0154 does.  So
 * this checks the half of M4 that can be checked without the phone - that the
 * right request carries the right structure with the right fields - and it
 * runs on Linux, so it also runs in the userspace CI job.
 *
 * It is not a test of the kernel.  "Green" here means the layer asks correctly,
 * nothing about whether the GPU answers; that needs the phone.
 */
#include "jackpotlte_drm.h"
#include "jackpotlte_gpu.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(cond, ...) do {                                   \
	if (!(cond)) {                                          \
		failures++;                                     \
		printf("FAIL %s:%d: ", __func__, __LINE__);     \
		printf(__VA_ARGS__);                            \
		printf("\n");                                   \
	}                                                       \
} while (0)

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The fake IOKit.
 */
struct fake {
	unsigned calls;
	uint32_t selector;
	size_t   size;
	unsigned char sent[64];         /* what the layer sent, before any reply */
	uint32_t kr;                    /* what call() answers with */
	uint64_t job_seq;               /* what the "kernel" gives the next job */

	unsigned maps, unmaps, closes;
	uint32_t unmap_handle;          /* the type the unmap came back with */
	void    *map_address;
	uint64_t map_size;
	uint32_t map_kr;

	/* Fill in the kernel's answer for this selector, and only this one: a
	 * reply armed for another selector would write its own structure's
	 * bytes into a request of a different size. */
	void (*reply)(struct fake *f, unsigned char *data, size_t size);
	uint32_t reply_selector;
};

static uint32_t
fake_call(void *ctx, uint32_t selector, void *data, size_t size)
{
	struct fake *f = ctx;

	f->calls++;
	f->selector = selector;
	f->size = size;
	if (size <= sizeof(f->sent)) {
		memcpy(f->sent, data, size);
	}
	if (f->kr == JT_IORETURN_SUCCESS) {
		if (f->reply != NULL && f->selector == f->reply_selector) {
			f->reply(f, data, size);
		}
		/* A submit always comes back with the job's number in `jc`, 0 when
		 * the kernel refused it (0154:541) - so the layer can read 0 as
		 * "not queued" rather than the chain it sent. */
		if (f->selector == JT_PANFROST_SUBMIT && size >= sizeof(struct jt_submit_req)) {
			((struct jt_submit_req *)data)->jc = f->job_seq;
		}
	}
	return f->kr;
}

static uint32_t
fake_map(void *ctx, uint32_t handle, void **address, uint64_t *size)
{
	struct fake *f = ctx;

	f->maps++;
	f->selector = handle;
	if (f->map_kr != JT_IORETURN_SUCCESS) {
		return f->map_kr;
	}
	*address = f->map_address;
	*size = f->map_size;
	return JT_IORETURN_SUCCESS;
}

static void
fake_unmap(void *ctx, uint32_t handle, void *address, uint64_t size)
{
	struct fake *f = ctx;

	(void)address;
	(void)size;
	f->unmaps++;
	f->unmap_handle = handle;
}

static void
fake_close(void *ctx)
{
	struct fake *f = ctx;

	f->closes++;
}

static const struct jt_backend fake_backend = {
	fake_call, fake_map, fake_unmap, fake_close,
};

/* One fake at a time - the tests run one after another, and the layer holds
 * the pointer, so it has to outlive any open. */
static struct fake G;

/* Arm the fake's answer for one selector. */
static void
arm(struct fake *f, uint32_t selector,
    void (*reply)(struct fake *, unsigned char *, size_t))
{
	f->reply = reply;
	f->reply_selector = selector;
}

static struct jt_gpu *
open_with(struct fake **fp)
{
	struct jt_gpu *g = NULL;

	memset(&G, 0, sizeof(G));
	G.kr = JT_IORETURN_SUCCESS;
	G.map_kr = JT_IORETURN_SUCCESS;
	CHECK(jt_gpu_open(&g, &fake_backend, &G) == 0, "open failed");
	*fp = &G;
	return g;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The kernel's answers.
 */
static void
reply_create(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_create_bo_req *r = (struct jt_create_bo_req *)data;

	(void)f;
	(void)size;
	r->handle = 7;
	r->offset = 0x01000000;         /* JT_FIRST_JOB_GPU: where 0153 starts */
}

static void
reply_param(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_get_param_req *r = (struct jt_get_param_req *)data;

	(void)f;
	(void)size;
	r->value = 0x7010;              /* a plausible id */
}

static void
reply_offset(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_get_bo_offset_req *r = (struct jt_get_bo_offset_req *)data;

	(void)f;
	(void)size;
	r->offset = 0x02000000;
}

/* 0154:551: the kernel keeps every buffer and says so. */
static void
reply_madvise(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_madvise_req *r = (struct jt_madvise_req *)data;

	(void)f;
	(void)size;
	r->retained = 1;
}

/* A SweetSurface as the kernel describes it (0154:328-351). */
static void
reply_surface(struct fake *f, unsigned char *data, size_t size)
{
	struct jt_surface_req *r = (struct jt_surface_req *)data;

	(void)f;
	(void)size;
	r->handle = 9;
	r->size = 0x400000;
	r->offset = 0;
	r->rowBytes = 1080 * 4;
	r->width = 1080;
	r->height = 2220;
	r->pixelFormat = 0x4;
	r->planes = 1;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static void
test_open(void)
{
	struct jt_gpu *g = NULL;
	struct fake *f = NULL;
	struct jt_backend broken = fake_backend;

	CHECK(jt_gpu_open(&g, NULL, NULL) == -EINVAL, "a NULL backend was accepted");
	broken.map = NULL;
	CHECK(jt_gpu_open(&g, &broken, NULL) == -EINVAL, "a backend without map was accepted");

	g = open_with(&f);
	CHECK(g != NULL, "open_with returned nothing");
	CHECK(f->closes == 0, "the connection was closed before it was used");
	if (g != NULL) {
		jt_gpu_close(g);
		CHECK(f->closes == 1, "close did not reach the backend");
	}
}

static void
test_create_bo(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_create_bo_req sent;
	uint32_t handle = 0;
	uint64_t address = 0;

	arm(f, JT_PANFROST_CREATE_BO, reply_create);
	CHECK(jt_bo_create(g, 16384, &handle, &address) == 0, "create_bo failed");
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK(f->selector == JT_PANFROST_CREATE_BO, "selector %#x, not create_bo", f->selector);
	CHECK(f->size == sizeof(struct jt_create_bo_req), "sent %zu bytes", f->size);
	CHECK(sent.size == 16384, "asked for %u bytes", sent.size);
	CHECK(handle == 7, "handle %u", handle);
	CHECK(address == 0x01000000, "address %#llx", (unsigned long long)address);

	/* A buffer with no address is not usable and must not look like success. */
	f->reply = NULL;
	CHECK(jt_bo_create(g, 4096, &handle, &address) == -EIO,
	    "a handle of 0 was accepted");
	CHECK(jt_bo_create(g, 0, &handle, &address) == -EINVAL, "size 0 was accepted");

	/* The kernel's refusals arrive as errno. */
	f->kr = JT_IORETURN_NOT_READY;
	CHECK(jt_bo_create(g, 4096, &handle, &address) == -ENODEV,
	    "not-ready did not become -ENODEV");
	jt_gpu_close(g);
}

static void
test_map(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	static int memory[4];
	void *address = NULL;
	uint64_t size = 0;

	f->map_address = memory;
	f->map_size = sizeof(memory);
	CHECK(jt_bo_map(g, 3, &address, &size) == 0, "map failed");
	CHECK(f->maps == 1, "%u maps", f->maps);
	CHECK(f->selector == 3, "the client-memory type was %u, not the handle", f->selector);
	CHECK(address == memory && size == sizeof(memory), "address/size wrong");
	CHECK(jt_bo_unmap(g, 3, address, size) == 0, "unmap failed");
	CHECK(f->unmaps == 1, "%u unmaps", f->unmaps);
	CHECK(f->unmap_handle == 3, "the unmap carried type %u, not the handle", f->unmap_handle);

	f->map_kr = JT_IORETURN_NOT_FOUND;
	CHECK(jt_bo_map(g, 4, &address, &size) == -ENOENT,
	    "a missing buffer did not become -ENOENT");
	jt_gpu_close(g);
}

static void
test_submit(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	uint32_t handles[2] = { 7, 8 };
	struct jt_submit_req sent;
	uint64_t seq;
	int error = 0;

	f->job_seq = 42;                        /* the job the "kernel" queues */
	seq = jt_submit(g, 0x01000000, true, handles, 2, &error);
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK(error == 0, "submit reported %d", error);
	CHECK(seq == 42, "job number %llu", (unsigned long long)seq);
	CHECK(f->selector == JT_PANFROST_SUBMIT, "selector %#x", f->selector);
	CHECK(f->size == sizeof(struct jt_submit_req), "sent %zu bytes", f->size);
	CHECK(sent.jc == 0x01000000, "chain %#llx", (unsigned long long)sent.jc);
	CHECK(sent.bo_handles == (uint64_t)(uintptr_t)handles,
	    "the handle array did not travel by address");
	CHECK(sent.bo_handle_count == 2, "count %u", sent.bo_handle_count);
	CHECK((sent.requirements & JT_JD_REQ_FS) != 0, "the fragment bit is missing");

	/* A vertex job must not carry the fragment bit. */
	(void)jt_submit(g, 0x01000000, false, NULL, 0, &error);
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK((sent.requirements & JT_JD_REQ_FS) == 0, "a vertex job asked to be a fragment job");
	CHECK(sent.bo_handle_count == 0 && sent.bo_handles == 0, "no-handle submit is not empty");

	/* The kernel answering "no job number" is a failure, not job 0. */
	f->job_seq = 0;
	seq = jt_submit(g, 0x01000000, false, NULL, 0, &error);
	CHECK(seq == 0 && error == -EIO, "an answer of 0 was taken as a job (error %d, seq %llu)",
	    error, (unsigned long long)seq);

	f->kr = JT_IORETURN_UNSUPPORTED;
	(void)jt_submit(g, 0x01000000, false, NULL, 0, &error);
	CHECK(error == -ENOTTY, "an unsupported submit became %d", error);
	jt_gpu_close(g);
}

static void
test_waits(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_wait_bo_req bo;
	struct jt_wait_req seqreq;

	CHECK(jt_wait_bo(g, 5, 1000000000) == 0, "wait_bo failed");
	memcpy(&bo, f->sent, sizeof(bo));
	CHECK(f->selector == JT_PANFROST_WAIT_BO, "selector %#x", f->selector);
	CHECK(bo.handle == 5 && bo.timeout_ns == 1000000000, "wait_bo fields wrong");

	CHECK(jt_wait_seq(g, 42, 2000000000) == 0, "wait_seq failed");
	memcpy(&seqreq, f->sent, sizeof(seqreq));
	CHECK(f->selector == JT_WAIT, "selector %#x", f->selector);
	CHECK(seqreq.seq == 42 && seqreq.timeout_ns == 2000000000, "wait_seq fields wrong");

	f->kr = JT_IORETURN_TIMEOUT;
	CHECK(jt_wait_seq(g, 42, 1) == -ETIMEDOUT, "a timeout did not become -ETIMEDOUT");
	jt_gpu_close(g);
}

static void
test_param_and_release(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	uint64_t value = 0;
	struct jt_get_param_req param;
	struct jt_gem_close close_req;

	arm(f, JT_PANFROST_GET_PARAM, reply_param);
	CHECK(jt_get_param(g, JT_PARAM_GPU_ID_HI, &value) == 0, "get_param failed");
	memcpy(&param, f->sent, sizeof(param));
	CHECK(param.param == JT_PARAM_GPU_ID_HI, "param %u", param.param);
	CHECK(value == 0x7010, "value %#llx", (unsigned long long)value);

	arm(f, JT_PANFROST_GET_BO_OFFSET, reply_offset);
	CHECK(jt_bo_offset(g, 7, &value) == 0 && value == 0x02000000, "get_bo_offset wrong");

	CHECK(jt_bo_release(g, 7) == 0, "release failed");
	memcpy(&close_req, f->sent, sizeof(close_req));
	CHECK(f->selector == JT_DRM_GEM_CLOSE, "selector %#x", f->selector);
	CHECK(close_req.handle == 7, "closed handle %u", close_req.handle);

	f->kr = JT_IORETURN_NOT_FOUND;
	CHECK(jt_bo_release(g, 9) == -ENOENT, "a missing handle did not become -ENOENT");
	jt_gpu_close(g);
}

static void
test_madvise(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_madvise_req sent;
	uint32_t retained = 0;

	arm(f, JT_PANFROST_MADVISE, reply_madvise);
	CHECK(jt_bo_madvise(g, 7, 1 /* PANFROST_MADV_DONTNEED */, &retained) == 0,
	    "madvise failed");
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK(f->selector == JT_PANFROST_MADVISE, "selector %#x", f->selector);
	CHECK(f->size == sizeof(struct jt_madvise_req), "sent %zu bytes", f->size);
	CHECK(sent.handle == 7 && sent.madv == 1, "madvise fields wrong");
	CHECK(retained == 1, "retained %u, not the kernel's answer", retained);

	CHECK(jt_bo_madvise(g, 0, 0, &retained) == -EINVAL, "handle 0 was accepted");
	jt_gpu_close(g);
}

static void
test_surface(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_surface_req info;
	struct jt_surface_req sent;
	uint32_t handle = JT_SURFACE_DESCRIBE;

	/* Describe only: no buffer is wanted, so the describe value travels. */
	f->reply = NULL;
	CHECK(jt_surface_buffer(g, 12, &handle, &info) == 0, "surface describe failed");
	memcpy(&sent, f->sent, sizeof(sent));
	CHECK(f->selector == JT_SURFACE, "selector %#x", f->selector);
	CHECK(sent.surface == 12 && sent.handle == JT_SURFACE_DESCRIBE,
	    "surface %u handle %#x", sent.surface, sent.handle);
	jt_gpu_close(g);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The libdrm face (jackpotlte_drm.c): the same fake, reached through the
 * request codes Mesa uses.  What is checked is the translation - that a DRM
 * request becomes the kernel's structure, with the right fields, and that the
 * kernel's answer comes back in the caller's structure.
 */
static void
test_drm_ioctl(void)
{
	struct fake *f;
	struct jt_gpu *g = open_with(&f);
	struct jt_create_bo_req create;
	struct jt_get_param_req param;
	struct jt_mmap_bo_req mmap_bo;
	struct jt_madvise_req madvise;
	struct jt_submit_req submit;
	struct jt_wait_req waited;
	uint32_t handles[2] = { 7, 8 };
	unsigned char dummy[16] = { 0 };

	jackpotlte_drm_attach(g);

	/* The code's size field is the structure's size: drmIoctl() has only
	 * the code, so that is all it can tell the kernel. */
	memset(&create, 0, sizeof(create));
	create.size = 16384;
	arm(f, JT_PANFROST_CREATE_BO, reply_create);
	CHECK(drmIoctl(-1, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_CREATE_BO, sizeof(create)),
	    &create) == 0, "create_bo through drmIoctl failed");
	CHECK(f->selector == JT_PANFROST_CREATE_BO, "selector %#x", f->selector);
	CHECK(f->size == sizeof(struct jt_create_bo_req), "sent %zu bytes", f->size);
	CHECK(create.handle == 7 && create.offset == 0x01000000, "the answer was not written back");

	/* A request unknown to the kernel is refused here, without a call. */
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(0x46, 8), dummy, 8) == -ENOTTY,
	    "the performance counter request was not -ENOTTY");
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(0x0a, 8), dummy, 8) == -ENOTTY,
	    "a JM context request was not -ENOTTY");
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(0x40, 16), dummy, 16) == -EINVAL,
	    "a short submit structure was accepted");

	memset(&param, 0, sizeof(param));
	param.param = JT_PARAM_GPU_ID_HI;
	arm(f, JT_PANFROST_GET_PARAM, reply_param);
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_GET_PARAM, sizeof(param)),
	    &param, sizeof(param)) == 0, "get_param failed");
	CHECK(param.value == 0x7010, "value %#llx", (unsigned long long)param.value);

	/* MMAP_BO is answered here: the offset is bookkeeping (the mapping is by
	 * handle), and the kernel is not asked. */
	memset(&mmap_bo, 0, sizeof(mmap_bo));
	mmap_bo.handle = 5;
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_MMAP_BO, sizeof(mmap_bo)),
	    &mmap_bo, sizeof(mmap_bo)) == 0, "mmap_bo failed");
	CHECK(mmap_bo.offset == (uint64_t)5 << 12, "offset %#llx", (unsigned long long)mmap_bo.offset);

	memset(&madvise, 0, sizeof(madvise));
	madvise.handle = 7;
	madvise.madv = 1;
	arm(f, JT_PANFROST_MADVISE, reply_madvise);
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_MADVISE, sizeof(madvise)),
	    &madvise, sizeof(madvise)) == 0, "madvise failed");
	CHECK(madvise.retained == 1, "retained was not written back");

	/* The submit: the kernel's number comes back in `jc`, and the sync
	 * object the caller named is given that number. */
	memset(&submit, 0, sizeof(submit));
	submit.jc = 0x01000000;
	submit.out_sync = 3;
	submit.bo_handles = (uint64_t)(uintptr_t)handles;
	submit.bo_handle_count = 2;
	submit.requirements = JT_JD_REQ_FS;
	f->job_seq = 42;
	CHECK(jackpotlte_drm_ioctl(g, JACKPOTLTE_DRM_REQUEST(JT_PANFROST_SUBMIT, sizeof(submit)),
	    &submit, sizeof(submit)) == 0, "submit failed");
	CHECK(f->selector == JT_PANFROST_SUBMIT, "selector %#x", f->selector);
	CHECK(f->size == 40, "the kernel was sent %zu bytes, not its 40-byte submit", f->size);
	CHECK(submit.jc == 42, "the job's number was not written back in jc");
	CHECK(jackpotlte_drm_last_seq() == 42, "last_seq %llu",
	    (unsigned long long)jackpotlte_drm_last_seq());

	CHECK(drmSyncobjWait(-1, &submit.out_sync, 1, 0, 0, NULL) == 0,
	    "the sync object did not wait for the job");
	memcpy(&waited, f->sent, sizeof(waited));
	CHECK(f->selector == JT_WAIT, "selector %#x", f->selector);
	CHECK(waited.seq == 42, "waited for job %llu", (unsigned long long)waited.seq);

	/* A surface: described, and then made.  The field names differ from the
	 * kernel's; the structure that travels is the kernel's. */
	{
		struct jackpotlte_drm_surface described;
		struct jackpotlte_drm_surface made;
		struct jt_surface_req sent;

		memset(&described, 0, sizeof(described));
		described.surface = 12;
		described.handle = JACKPOTLTE_DRM_DESCRIBE;
		CHECK(jackpotlte_drm_surface(-1, &described) == 0, "surface describe failed");
		memcpy(&sent, f->sent, sizeof(sent));
		CHECK(sent.surface == 12 && sent.handle == JT_SURFACE_DESCRIBE,
		    "surface %u handle %#x", sent.surface, sent.handle);

		memset(&made, 0, sizeof(made));
		made.surface = 12;
		made.handle = 0;
		arm(f, JT_SURFACE, reply_surface);
		CHECK(jackpotlte_drm_surface(-1, &made) == 0, "surface describe-and-make failed");
		CHECK(made.handle == 9 && made.size == 0x400000, "the buffer was not written back");
		CHECK(made.row_bytes == 1080 * 4 && made.width == 1080 && made.height == 2220,
		    "row_bytes/width/height were not translated");
		CHECK(made.pixel_format == 0x4 && made.planes == 1,
		    "pixel_format/planes were not translated");
	}

	/* A buffer given back takes its mapping with it. */
	{
		static int memory[2];

		f->map_address = memory;
		f->map_size = sizeof(memory);
		CHECK(jackpotlte_drm_map(-1, 5, 8) == memory, "map failed");
		CHECK(jackpotlte_drm_map(-1, 5, 8) == memory, "the second map failed");
		CHECK(f->maps == 1, "the second map called the kernel again (%u maps)", f->maps);
		jackpotlte_drm_unmap(-1, 5, memory);
		CHECK(f->unmaps == 0, "unmap did not keep the mapping");
		CHECK(drmCloseBufferHandle(-1, 5) == 0, "close failed");
		CHECK(f->unmaps == 1 && f->unmap_handle == 5, "the mapping was not unmapped");
		CHECK(f->selector == JT_DRM_GEM_CLOSE, "selector %#x", f->selector);
	}

	jackpotlte_drm_attach(NULL);
	jt_gpu_close(g);
}

static void
test_drm_misc(void)
{
	drmVersionPtr version;
	drmDevicePtr device = NULL;
	uint64_t capability = 1;
	uint32_t a = 0, b = 0;
	int file = -1;

	jackpotlte_drm_attach(NULL);
	CHECK(drmOpenWithType("panfrost", NULL, DRM_NODE_RENDER) == -1,
	    "a device was opened without a GPU");
	CHECK(drmOpenWithType("msm", NULL, DRM_NODE_RENDER) == -1, "an msm device was opened");

	version = drmGetVersion(-1);
	CHECK(version != NULL, "no version");
	if (version != NULL) {
		CHECK(version->version_major == 1 && version->version_minor == 2,
		    "version %d.%d", version->version_major, version->version_minor);
		CHECK(version->name != NULL && strcmp(version->name, "panfrost") == 0,
		    "name %s", version->name == NULL ? "(null)" : version->name);
		drmFreeVersion(version);
	}

	CHECK(drmGetCap(-1, 0, &capability) == 0 && capability == 0, "get_cap");
	CHECK(drmGetNodeTypeFromFd(-1) == DRM_NODE_RENDER, "node type");
	CHECK(drmGetDevices2(0, NULL, 0) == 0, "device enumeration is not empty");
	CHECK(drmGetDevice2(-1, 0, &device) == -ENODEV, "a device was found");
	CHECK(drmPrimeHandleToFD(-1, 1, 0, &file) == -1 && errno == ENOSYS,
	    "a buffer was exported as a descriptor");

	CHECK(drmSyncobjCreate(-1, 0, &a) == 0 && a != 0, "syncobj create");
	CHECK(drmSyncobjCreate(-1, 0, &b) == 0 && b != a, "the second syncobj is not new");
	CHECK(drmSyncobjExportSyncFile(-1, a, &file) == 0 && file >= 0, "sync file export");
	if (file >= 0) {
		CHECK(drmSyncobjImportSyncFile(-1, b, file) == 0, "sync file import");
		close(file);
	}
	CHECK(drmSyncobjTransfer(-1, b, 0, a, 0, 0) == 0, "syncobj transfer");
	CHECK(drmSyncobjWait(-1, &b, 1, 0, 0, NULL) == 0, "an unsubmitted sync object did not wait");
	CHECK(drmSyncobjReset(-1, &a, 1) == 0, "syncobj reset");
	CHECK(drmSyncobjDestroy(-1, a) == 0, "syncobj destroy");
	CHECK(drmSyncobjFDToHandle(-1, -1, &a) == -ENOSYS, "syncobj fd import is not ENOSYS");
}

static void
test_errno_table(void)
{
	CHECK(jt_errno(JT_IORETURN_SUCCESS) == 0, "success");
	CHECK(jt_errno(JT_IORETURN_BAD_ARGUMENT) == -EINVAL, "bad argument");
	CHECK(jt_errno(JT_IORETURN_UNSUPPORTED) == -ENOTTY, "unsupported");
	CHECK(jt_errno(JT_IORETURN_NOT_READY) == -ENODEV, "not ready");
	CHECK(jt_errno(JT_IORETURN_TIMEOUT) == -ETIMEDOUT, "timeout");
}

int
main(void)
{
	test_open();
	test_create_bo();
	test_map();
	test_submit();
	test_waits();
	test_param_and_release();
	test_madvise();
	test_surface();
	test_drm_ioctl();
	test_drm_misc();
	test_errno_table();

	if (failures != 0) {
		printf("FAIL: %u check%s failed\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("PASS: the request layer asks 0154 what 0154 expects\n");
	return 0;
}
