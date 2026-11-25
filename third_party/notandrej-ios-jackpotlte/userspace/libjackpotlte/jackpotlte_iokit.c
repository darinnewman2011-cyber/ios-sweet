/* SPDX-License-Identifier: MIT */
/*
 * jackpotlte_iokit: the iOS backend of jackpotlte_gpu.
 *
 * This is the half of M4 that only runs on the phone: it finds the
 * `JackpotlteMali` service (xnu/patches/0154), opens a connection to it, and
 * answers the two things the request layer needs of IOKit - one
 * externalMethod with the request's structure in and out, and one mapping of a
 * buffer's memory into this process.
 *
 * Everything here is the same call maliprobe makes, so the ABI is already
 * exercised by something that shipped in the root.  The difference is that a
 * tool leaks what it likes and a library must not: the connection and the
 * service object are released, and the mapping is unmapped by the handle whose
 * client-memory type it was made under (that is how Darwin finds it again -
 * there is no address to look it up by).
 *
 * Like maliprobe, IOKit's entry points are declared here rather than included:
 * the iOS SDK ships no IOKit headers, and what links this against them is the
 * device's own IOKit at load time.
 */
#include "jackpotlte_gpu.h"

#include <errno.h>
#include <stdlib.h>

typedef unsigned int mach_port_t;
typedef unsigned int io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;

#define MACH_PORT_NULL 0

extern mach_port_t mach_task_self_;
extern const mach_port_t kIOMainPortDefault;
extern void *IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t masterPort, void *matching);
extern int IOObjectRelease(io_object_t object);
extern int IOServiceOpen(io_service_t service, mach_port_t owningTask, uint32_t type,
    io_connect_t *connection);
extern int IOServiceClose(io_connect_t connection);
extern int IOConnectCallStructMethod(io_connect_t connection, uint32_t selector,
    const void *inputStruct, size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
extern int IOConnectMapMemory64(io_connect_t connection, uint32_t memoryType, mach_port_t intoTask,
    uint64_t *address, uint64_t *size, uint32_t options);
extern int IOConnectUnmapMemory64(io_connect_t connection, uint32_t memoryType, mach_port_t fromTask,
    uint64_t address);

struct jt_iokit {
	io_connect_t connection;
};

/*
 * One request.  The kernel reads `structureInput` and, when the output buffer
 * is at least as big, copies the same structure back out of `structureOutput`
 * (0154:586-590), so one buffer is both, exactly as externalMethod() is
 * written.  `size` is the structure's real size - never the union's 40 bytes -
 * and the kernel refuses more than that (0154:461).
 */
static uint32_t
iokit_call(void *ctx, uint32_t selector, void *data, size_t size)
{
	struct jt_iokit *io = ctx;
	size_t returned = size;
	int kr;

	if (io == NULL || io->connection == MACH_PORT_NULL) {
		return JT_IORETURN_NOT_READY;
	}
	kr = IOConnectCallStructMethod(io->connection, selector, data, size, data, &returned);
	return (uint32_t) kr;
}

/*
 * The buffer's memory.  The handle is the client-memory type (0154:422-437):
 * `clientMemoryForType(type)` hands back that buffer's descriptor, and the
 * kernel mapping is what makes the same bytes the GPU's and ours.
 */
static uint32_t
iokit_map(void *ctx, uint32_t handle, void **address, uint64_t *size)
{
	struct jt_iokit *io = ctx;
	uint64_t mapped = 0, mapped_size = 0;
	int kr;

	if (io == NULL || io->connection == MACH_PORT_NULL) {
		return JT_IORETURN_NOT_READY;
	}
	kr = IOConnectMapMemory64(io->connection, handle, mach_task_self_, &mapped, &mapped_size, 0);
	if (kr != 0) {
		return (uint32_t) kr;
	}
	if (mapped == 0 || mapped_size == 0) {
		(void) IOConnectUnmapMemory64(io->connection, handle, mach_task_self_, mapped);
		return JT_IORETURN_NO_MEMORY;
	}
	*address = (void *) (uintptr_t) mapped;
	*size = mapped_size;
	return JT_IORETURN_SUCCESS;
}

static void
iokit_unmap(void *ctx, uint32_t handle, void *address, uint64_t size)
{
	struct jt_iokit *io = ctx;

	(void) size;
	if (io == NULL || io->connection == MACH_PORT_NULL || address == NULL) {
		return;
	}
	(void) IOConnectUnmapMemory64(io->connection, handle, mach_task_self_,
	    (uint64_t) (uintptr_t) address);
}

/*
 * Close the connection and drop the context.  The kernel is expecting this:
 * clientClose() frees every buffer the client still holds, waiting for the last
 * job each was in (0154:404-418), so a client that goes away leaves no job
 * pointing at memory that is no longer there.
 */
static void
iokit_close(void *ctx)
{
	struct jt_iokit *io = ctx;

	if (io == NULL) {
		return;
	}
	if (io->connection != MACH_PORT_NULL) {
		(void) IOServiceClose(io->connection);
		io->connection = MACH_PORT_NULL;
	}
	free(io);
}

static const struct jt_backend iokit_backend = {
	iokit_call, iokit_map, iokit_unmap, iokit_close,
};

int
jt_iokit_open(struct jt_gpu **out)
{
	struct jt_iokit *io;
	io_service_t service;
	int kr, ret;

	if (out == NULL) {
		return -EINVAL;
	}
	*out = NULL;

	service = IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching("JackpotlteMali"));
	if (service == MACH_PORT_NULL) {
		/* No service: the GPU did not come up (0154 starts it only once
		 * the probe has brought the engine up), or this is not an A8. */
		return -ENOENT;
	}
	io = calloc(1, sizeof(*io));
	if (io == NULL) {
		(void) IOObjectRelease(service);
		return -ENOMEM;
	}
	kr = IOServiceOpen(service, mach_task_self_, 0, &io->connection);
	/* The connection is what is used from here on; the service name is not
	 * needed again. */
	(void) IOObjectRelease(service);
	if (kr != 0 || io->connection == MACH_PORT_NULL) {
		free(io);
		return -ENODEV;
	}

	ret = jt_gpu_open(out, &iokit_backend, io);
	if (ret != 0) {
		iokit_close(io);
	}
	return ret;
}
