/*
 * libsweet-gl-loader.dylib: GPU step 7 (docs/GPU_JACKPOTLTE.md). backboardd's launchd job inserts it
 * (DYLD_INSERT_LIBRARIES; dyld honours that because of the kernel's "AMFI" dyld policy, patch 0119).
 * In backboardd, when the kernel has the Mali (the RedwoodMali service, sweetgpu=4), it loads
 * /sweet/lib/libredwood-gl.dylib (src/eagl.c), which puts OpenGL ES on Mesa's panfrost behind
 * EAGLContext, so CoreAnimation's render server draws with the GPU instead of its software
 * rasterizer. Anywhere else, or without the GPU, it does nothing.
 *
 * The idea and the library it loads come from y2sios' src/inject.c (stormtroopxyz/y2sios, MIT); its
 * other hooks there are addresses inside iOS 16.5's shared cache and are not used here.
 *
 * Boot-arg sweetgl=0: leave CoreAnimation on its software renderer.
 * The library's log lines ("redwood-gl: ...") go to stderr, which is sent to the console when it was
 * /dev/null, so they reach the kernel's log.
 */
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define SWEET_GL "/sweet/lib/libredwood-gl.dylib"

static void
say(const char *format, ...)
{
	char line[512];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	if (n > 0) {
		write(2, line, (size_t)n);
	}
}

/* stderr to the console if it goes nowhere (launchd gives a job /dev/null) */
static void
stderr_to_console(void)
{
	struct stat err, null;
	int fd;

	if (fstat(2, &err) != 0 || stat("/dev/null", &null) != 0) {
		return;
	}
	if (!S_ISCHR(err.st_mode) || err.st_rdev != null.st_rdev) {
		return;
	}
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		dup2(fd, 2);
		if (fd != 2) {
			close(fd);
		}
	}
}

__attribute__((constructor))
static void
sweet_gl_loader(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *name = getprogname();
	io_service_t gpu;

	if (name == NULL || strcmp(name, "backboardd") != 0 || access(SWEET_GL, R_OK) != 0) {
		return;
	}
	stderr_to_console();
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		const char *off = strstr(arguments, "sweetgl=0");

		if (off != NULL && (off == arguments || off[-1] == ' ')) {
			say("sweet-gl: sweetgl=0: CoreAnimation keeps its software renderer\n");
			return;
		}
	}
	gpu = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("RedwoodMali"));
	if (gpu == 0) {
		say("sweet-gl: no GPU (RedwoodMali): CoreAnimation keeps its software renderer\n");
		return;
	}
	IOObjectRelease(gpu);
	if (dlopen(SWEET_GL, RTLD_NOW) == NULL) {
		say("sweet-gl: %s: %s\n", SWEET_GL, dlerror());
		return;
	}
	say("sweet-gl: %s loaded into backboardd: CoreAnimation draws with OpenGL ES on the Mali\n", SWEET_GL);
}
