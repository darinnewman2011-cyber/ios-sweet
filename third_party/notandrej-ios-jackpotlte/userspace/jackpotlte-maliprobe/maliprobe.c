/* SPDX-License-Identifier: MIT */
/*
 * jackpotlte-maliprobe: the first jackpotlte userspace.
 *
 * An iOS arm64 tool that speaks the ABI JackpotlteMaliUserClient answers
 * (xnu/patches/0154-jackpotlte-mali-user-client.patch): find the
 * "JackpotlteMali" service, open it, make a buffer, map it, read the GPU
 * address the kernel gave it, ask for a few parameters, close the buffer.
 *
 * It proves the userland half of M3 - that a Darwin client can reach the GPU
 * through the service, that the structures are the sizes the kernel reads, and
 * that a buffer's memory is really shareable - before any Metal, any Mesa and
 * any shim exist.  The reference's own shim (redwood_drm.c) does the same
 * things through libdrm; this is the smallest version of it.
 *
 * Run modes:
 *   maliprobe            probe the service (on the phone; needs the GPU up,
 *                        so boot with jtmalipwr=1 and the engine args)
 *   maliprobe --abi      print the ABI numbers and exit; touches no IOKit, so
 *                        it runs on the build machine.  The CI job greps these
 *                        against the kernel patch: if a structure or a
 *                        selector drifts, the build goes red.
 *   maliprobe --help
 *
 * IOKit's user entry points are declared here on purpose.  The iOS SDK ships
 * no IOKit headers, and this links with -Wl,-undefined,dynamic_lookup, so the
 * device's own IOKit resolves them when the tool loads.
 */
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* mach_port_t and libSystem's task port: no <mach/mach.h>, so that this file
 * also parses with a host compiler that has no Darwin headers. */
typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t mach_task_self_;
#define KERN_SUCCESS 0

/* IOKit, userspace side. */
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
extern const mach_port_t kIOMainPortDefault;
extern void *IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t master, void *matching);
extern kern_return_t IOServiceOpen(io_service_t service, mach_port_t task,
    uint32_t type, io_connect_t *connection);
extern kern_return_t IOServiceClose(io_connect_t connection);
extern kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector,
    const void *input, size_t inputSize, void *output, size_t *outputSize);
extern kern_return_t IOConnectMapMemory64(io_connect_t connection, uint32_t type,
    mach_port_t task, uint64_t *address, uint64_t *size, uint32_t options);
/* libSystem's, for kern.bootargs (no <sys/sysctl.h>: see the note above). */
extern int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * The ABI.  Every number and every field here mirrors 0154 exactly; the
 * kernel is the authority and the --abi check exists to keep them in step.
 */
enum {
	kDrmGemClose         = 0x09,
	kPanfrostSubmit      = 0x40,
	kPanfrostWaitBO      = 0x41,
	kPanfrostCreateBO    = 0x42,
	kPanfrostMmapBO      = 0x43,
	kPanfrostGetParam    = 0x44,
	kPanfrostGetBOOffset = 0x45,
	kPanfrostMadvise     = 0x48,
	kJackpotlteWait      = 0x7d,
	kJackpotlteSurface   = 0x7e,
};

struct drm_gem_close {
	uint32_t handle, pad;
};

struct drm_panfrost_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};
#define PANFROST_JD_REQ_FS (1u << 0)

struct drm_panfrost_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

struct drm_panfrost_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};

struct drm_panfrost_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};

struct drm_panfrost_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct drm_panfrost_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};

struct drm_panfrost_madvise {
	uint32_t handle, madv, retained;
};

struct jackpotlte_surface {
	uint32_t surface;                       /* the SweetSurface's ID */
	uint32_t handle;                        /* kSurfaceDescribe: describe only */
	uint64_t size;
	uint32_t offset, rowBytes;
	uint32_t width, height, pixelFormat, planes;
};
#define kSurfaceDescribe 0xffffffffu

struct jackpotlte_wait {
	uint64_t seq;
	int64_t timeout_ns;
};

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define PREFIX "JACKPOTLTE MALIPROBE: "

static int console_fd = -1;

/* Print to stdout and mirror to the panel: a launchd job's stdout is a file
 * nobody can read back, the kernel console is photographable. */
static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	fputs(line, stdout);
	fflush(stdout);
	if (console_fd == -1) {
		console_fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	}
	if (console_fd >= 0) {
		ssize_t written = write(console_fd, line, (size_t)n);
		(void)written;
	}
}

/* The same structure in and out, which is what externalMethod() implements. */
static kern_return_t
call(io_connect_t connection, uint32_t selector, void *data, size_t size)
{
	size_t returned = size;

	return IOConnectCallStructMethod(connection, selector, data, size, data, &returned);
}

static const char *
kr_name(kern_return_t kr)
{
	static char unknown[32];

	switch (kr) {
	case KERN_SUCCESS:
		return "success";
	case 0xe00002c2: /* kIOReturnBadArgument */
		return "bad argument";
	case 0xe00002c7: /* kIOReturnUnsupported */
		return "unsupported";
	case 0xe00002c0: /* kIOReturnNoMemory */
		return "no memory";
	case 0xe00002bc: /* kIOReturnError */
		return "error";
	case 0xe00002c9: /* kIOReturnNotReady */
		return "not ready (the GPU is not up)";
	case 0xe00002c4: /* kIOReturnNotFound */
		return "not found";
	case 0xe00002d6: /* kIOReturnTimeout */
		return "timeout";
	default:
		snprintf(unknown, sizeof(unknown), "%#x", (unsigned int)kr);
		return unknown;
	}
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static int
abi(void)
{
	say("ABI selector drm_gem_close=%#x panfrost_submit=%#x wait_bo=%#x create_bo=%#x\n",
	    kDrmGemClose, kPanfrostSubmit, kPanfrostWaitBO, kPanfrostCreateBO);
	say("ABI selector mmap_bo=%#x get_param=%#x get_bo_offset=%#x madvise=%#x\n",
	    kPanfrostMmapBO, kPanfrostGetParam, kPanfrostGetBOOffset, kPanfrostMadvise);
	say("ABI selector wait=%#x surface=%#x describe=%#x\n",
	    kJackpotlteWait, kJackpotlteSurface, kSurfaceDescribe);
	say("ABI sizeof(drm_gem_close)=%zu\n", sizeof(struct drm_gem_close));
	say("ABI sizeof(drm_panfrost_submit)=%zu\n", sizeof(struct drm_panfrost_submit));
	say("ABI sizeof(drm_panfrost_wait_bo)=%zu\n", sizeof(struct drm_panfrost_wait_bo));
	say("ABI sizeof(drm_panfrost_create_bo)=%zu offsetof(offset)=%zu\n",
	    sizeof(struct drm_panfrost_create_bo), offsetof(struct drm_panfrost_create_bo, offset));
	say("ABI sizeof(drm_panfrost_mmap_bo)=%zu sizeof(drm_panfrost_get_param)=%zu\n",
	    sizeof(struct drm_panfrost_mmap_bo), sizeof(struct drm_panfrost_get_param));
	say("ABI sizeof(drm_panfrost_get_bo_offset)=%zu sizeof(drm_panfrost_madvise)=%zu\n",
	    sizeof(struct drm_panfrost_get_bo_offset), sizeof(struct drm_panfrost_madvise));
	say("ABI sizeof(jackpotlte_surface)=%zu offsetof(handle)=%zu offsetof(size)=%zu\n",
	    sizeof(struct jackpotlte_surface), offsetof(struct jackpotlte_surface, handle),
	    offsetof(struct jackpotlte_surface, size));
	say("ABI sizeof(jackpotlte_wait)=%zu\n", sizeof(struct jackpotlte_wait));
	return 0;
}

/* The daemon runs on every boot; the report is only wanted on a GPU boot
 * (the user client answers kIOReturnNotReady otherwise, and a line on the
 * panel on every ordinary boot is noise).  kern.bootargs says which boot this
 * is, so a normal boot stays quiet. */
static bool
gpu_boot(void)
{
	char args[1024];
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return false;
	}
	args[size] = '\0';
	return strstr(args, "jtmalimetal") != NULL || strstr(args, "jtmalipwr") != NULL;
}

static int
probe(void)
{
	static const uint32_t params[] = { 0, 1, 2, 3, 4, 22, 38, 39, 40 };
	io_service_t service;
	io_connect_t connection = 0;
	kern_return_t kr;
	struct drm_panfrost_create_bo create;
	struct drm_panfrost_get_bo_offset offset;
	struct drm_gem_close close;
	uint64_t mapped = 0, mapped_size = 0;
	volatile uint32_t *pixels;
	int failures = 0;

	if (!gpu_boot()) {
		printf(PREFIX "not a GPU boot (no jtmalimetal/jtmalipwr "
		    "in kern.bootargs); nothing to probe\n");
		return 0;
	}
	say(PREFIX "probe start\n");

	service = IOServiceGetMatchingService(kIOMainPortDefault,
	    IOServiceMatching("JackpotlteMali"));
	if (service == 0) {
		say(PREFIX "FAIL: no JackpotlteMali service (is the GPU up: jtmalipwr=1?)\n");
		return 1;
	}
	say(PREFIX "the service JackpotlteMali is there\n");

	kr = IOServiceOpen(service, mach_task_self_, 0, &connection);
	if (kr != KERN_SUCCESS) {
		say(PREFIX "FAIL: IOServiceOpen: %s\n", kr_name(kr));
		return 1;
	}
	say(PREFIX "IOServiceOpen ok\n");

	/* A buffer, the way panfrost makes one. */
	memset(&create, 0, sizeof(create));
	create.size = 16384;
	kr = call(connection, kPanfrostCreateBO, &create, sizeof(create));
	if (kr != KERN_SUCCESS) {
		say(PREFIX "FAIL: create_bo: %s\n", kr_name(kr));
		failures++;
		goto out;
	}
	say(PREFIX "create_bo: handle=%u gpu=%#" PRIx64 "\n", create.handle, create.offset);
	if (create.handle == 0 || create.offset == 0) {
		say(PREFIX "FAIL: create_bo gave no handle or no GPU address\n");
		failures++;
	}

	/* The address the kernel thinks it has must be the one it just gave. */
	memset(&offset, 0, sizeof(offset));
	offset.handle = create.handle;
	kr = call(connection, kPanfrostGetBOOffset, &offset, sizeof(offset));
	if (kr != KERN_SUCCESS || offset.offset != create.offset) {
		say(PREFIX "FAIL: get_bo_offset: %s, %#" PRIx64 " != %#" PRIx64 "\n",
		    kr_name(kr), offset.offset, create.offset);
		failures++;
	} else {
		say(PREFIX "get_bo_offset agrees: %#" PRIx64 "\n", offset.offset);
	}

	/* The buffer's memory, mapped into this process. */
	kr = IOConnectMapMemory64(connection, create.handle, mach_task_self_,
	    &mapped, &mapped_size, 0);
	if (kr != KERN_SUCCESS || mapped == 0) {
		say(PREFIX "FAIL: IOConnectMapMemory64: %s\n", kr_name(kr));
		failures++;
	} else {
		pixels = (volatile uint32_t *)(uintptr_t)mapped;
		pixels[0] = 0x5eed5eed;
		pixels[4095] = 0xc0ffee01;      /* the last word of the 16 KB */
		if (pixels[0] != 0x5eed5eed || pixels[4095] != 0xc0ffee01) {
			say(PREFIX "FAIL: the mapped buffer does not read back\n");
			failures++;
		} else {
			say(PREFIX "mapped %#" PRIx64 " bytes at %p and it reads back\n",
			    mapped_size, (void *)(uintptr_t)mapped);
		}
	}

	/* What the kernel knows about the GPU, through the one request that
	 * needs no GPU memory at all. */
	for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		struct drm_panfrost_get_param param;
		const char *name;

		memset(&param, 0, sizeof(param));
		param.param = params[i];
		kr = call(connection, kPanfrostGetParam, &param, sizeof(param));
		switch (params[i]) {
		case 0:  name = "gpu_id"; break;
		case 1:  name = "gpu_revision"; break;
		case 2:  name = "shader_present"; break;
		case 3:  name = "shader_ready"; break;
		case 4:  name = "tiler_present"; break;
		case 22: name = "l2_features"; break;
		case 38: name = "nr_core_groups"; break;
		case 39: name = "thread_tls_alloc"; break;
		default: name = "afbc_features"; break;
		}
		say(PREFIX "get_param %-16s = %#" PRIx64 "%s\n", name, param.value,
		    kr == KERN_SUCCESS ? "" : " (refused)");
	}

	close.handle = create.handle;
	kr = call(connection, kDrmGemClose, &close, sizeof(close));
	if (kr != KERN_SUCCESS) {
		say(PREFIX "FAIL: gem_close: %s\n", kr_name(kr));
		failures++;
	} else {
		say(PREFIX "gem_close ok\n");
	}

out:
	IOServiceClose(connection);
	say(PREFIX "%s: %d check%s failed\n", failures == 0 ? "PASS" : "FAIL",
	    failures, failures == 1 ? "" : "s");
	return failures == 0 ? 0 : 1;
}

int
main(int argc, char **argv)
{
	if (argc > 1 && (strcmp(argv[1], "--abi") == 0 || strcmp(argv[1], "-a") == 0)) {
		return abi();
	}
	if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
		printf("usage: %s [--abi | --help]\n"
		    "  (no option) talk to the JackpotlteMali service on the phone\n", argv[0]);
		return 0;
	}
	return probe();
}
