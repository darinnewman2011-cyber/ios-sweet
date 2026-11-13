/* Copyright (c) 2026 Darin. MIT License (see LICENSE); keep this notice. Written by Claude (Anthropic) for the ios-sweet Galaxy A8 port. */
/*
 * mali2metal's channel from user space (the kernel's side: bsd/dev/redwood_m2m.c; the record format
 * and the categories: PLANS/10-mali2metal.md).
 *
 *   m2m_client("metal")              this process is a Metal client: the kernel reports how it ends
 *   m2m_watch_crashes()              ... and where (m2m_crash_record), where the process has no handler of its own
 *   m2m_log('E', "PSO", "...", ...)  one record: severity I/W/E/F, a category of 2-5 capitals
 *
 * One write(2) per record, so records from the threads of a process never interleave. The time,
 * the process name and the pid are the kernel's. A record is one line: newlines inside the message
 * become " | ". Where /dev/m2m cannot be opened (an older kernel), records go to stdout as
 * "m2m-nodev: <sev> <CAT> <message>" so that nothing is lost, and the reason is said once.
 */
#ifndef REDWOOD_M2M_H
#define REDWOOD_M2M_H

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <IOKit/IOKitLib.h>

#define M2M_DEVICE              "/dev/m2m"
#define M2M_RECORD_MAX          1024

struct m2m_client {
	char role[16];
};
#define M2M_IOC_CLIENT          _IOW('m', 1, struct m2m_client)

static int m2m_fd = -1;
static int m2m_con = -1;	/* ios-sweet: no /dev/m2m and stdout is /dev/null (a daemon): the console */
static io_connect_t m2m_mali;	/* ios-sweet: RedwoodMali's request 0x79, one line into the kernel log (xnu 0129) */
static pthread_once_t m2m_once = PTHREAD_ONCE_INIT;
#define M2M_MALI_LOG            0x79
#define M2M_MALI_LOG_MAX        480

static void
m2m_open_once(void)
{
	struct stat out, null;
	io_service_t mali;

	m2m_fd = open(M2M_DEVICE, O_WRONLY | O_CLOEXEC);
	if (m2m_fd >= 0) {
		return;
	}
	/* ios-sweet: the kernel log through the GPU service, which every Metal process may open; a kernel
	 * before 0129 answers 0x79 with an error, and then the console or stdout below is used */
	mali = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("RedwoodMali"));
	if (mali != IO_OBJECT_NULL) {
		static const char hello[] = "I M2M records follow in the kernel log (RedwoodMali 0x79)";

		if (IOServiceOpen(mali, mach_task_self(), 0, &m2m_mali) != KERN_SUCCESS) {
			m2m_mali = IO_OBJECT_NULL;
		} else if (IOConnectCallStructMethod(m2m_mali, M2M_MALI_LOG, hello, sizeof(hello) - 1, NULL, NULL) !=
		    KERN_SUCCESS) {
			IOServiceClose(m2m_mali);
			m2m_mali = IO_OBJECT_NULL;
		}
		IOObjectRelease(mali);
		if (m2m_mali != IO_OBJECT_NULL) {
			return;
		}
	}
	if (fstat(STDOUT_FILENO, &out) != 0 || (stat("/dev/null", &null) == 0 && S_ISCHR(out.st_mode) &&
	    out.st_rdev == null.st_rdev)) {
		m2m_con = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	}
	if (m2m_con >= 0) {
		dprintf(m2m_con, "m2m[%s:%d]: W M2M %s: no channel; records follow on the console\n",
		    getprogname(), getpid(), M2M_DEVICE);
	} else {
		printf("m2m-nodev: W M2M %s: %s; records follow on stdout\n", M2M_DEVICE, strerror(errno));
		fflush(stdout);
	}
}

static inline void
m2m_vlog(char sev, const char *cat, const char *fmt, va_list ap)
{
	char rec[M2M_RECORD_MAX + 16];
	int head, n;

	pthread_once(&m2m_once, m2m_open_once);
	head = snprintf(rec, sizeof(rec), "%c %s ", sev, cat);
	if (head < 0 || (size_t)head >= sizeof(rec)) {
		return;
	}
	n = vsnprintf(rec + head, sizeof(rec) - (size_t)head, fmt, ap);
	if (n < 0) {
		return;
	}
	n = head + n < (int)sizeof(rec) - 1 ? head + n : (int)sizeof(rec) - 1;
	while (n > head && (rec[n - 1] == '\n' || rec[n - 1] == ' ')) {
		n--;
	}
	rec[n] = '\0';
	/* one record, one line: a newline inside becomes " | " (as far as the record has room) */
	for (char *nl = strchr(rec, '\n'); nl != NULL; nl = strchr(nl, '\n')) {
		if ((size_t)n + 2 < sizeof(rec)) {
			memmove(nl + 3, nl + 1, (size_t)(rec + n - nl));
			memcpy(nl, " | ", 3);
			n += 2;
			nl += 3;
		} else {
			*nl = ' ';
		}
	}
	if (m2m_fd >= 0) {
		(void)write(m2m_fd, rec, (size_t)n);
	} else if (m2m_mali != IO_OBJECT_NULL) {
		(void)IOConnectCallStructMethod(m2m_mali, M2M_MALI_LOG, rec,
		    (size_t)n < M2M_MALI_LOG_MAX ? (size_t)n : M2M_MALI_LOG_MAX, NULL, NULL);
	} else if (m2m_con >= 0) {
		dprintf(m2m_con, "m2m[%s:%d]: %s\n", getprogname(), getpid(), rec);
	} else {
		printf("m2m-nodev: %s\n", rec);
		fflush(stdout);
	}
}

static inline void m2m_log(char sev, const char *cat, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static inline void
m2m_log(char sev, const char *cat, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	m2m_vlog(sev, cat, fmt, ap);
	va_end(ap);
}

static inline void
m2m_client(const char *role)
{
	struct m2m_client c;

	pthread_once(&m2m_once, m2m_open_once);
	memset(&c, 0, sizeof(c));
	strlcpy(c.role, role, sizeof(c.role));
	if (m2m_fd >= 0 && ioctl(m2m_fd, M2M_IOC_CLIENT, &c) != 0) {
		m2m_log('W', "M2M", "client registration (%s) refused: %s", role, strerror(errno));
	}
}

/*
 * Where a client died. The kernel's record ("F CRASH ... ended by signal N") says that it died; this one,
 * written from the signal handler before the process goes, says where: the faulting address, pc, lr and
 * the callers (frame pointers), each as image+offset(symbol) from dladdr, and for code in the shared cache
 * also the address as the cache was built (unslid), which `ipsw dyld a2s` names.
 */
extern const void *_dyld_get_shared_cache_range(size_t *length);
#define M2M_CACHE_BASE          0x180000000UL

static inline size_t
m2m_where(char *out, size_t size, uintptr_t address)
{
	size_t length = 0;
	uintptr_t cache = (uintptr_t)_dyld_get_shared_cache_range(&length);
	Dl_info d;
	int n;

	if (dladdr((const void *)address, &d) != 0 && d.dli_fname != NULL) {
		const char *image = strrchr(d.dli_fname, '/') != NULL ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname;

		n = snprintf(out, size, " %s+%#lx(%s)", image, address - (uintptr_t)d.dli_fbase, d.dli_sname != NULL ? d.dli_sname : "?");
	} else {
		n = snprintf(out, size, " %#lx", address);
	}
	if (n > 0 && (size_t)n < size && cache != 0 && address >= cache && address < cache + length) {
		n += snprintf(out + n, size - (size_t)n, "[cache %#lx]", address - cache + M2M_CACHE_BASE);
	}
	return n > 0 ? ((size_t)n < size ? (size_t)n : size - 1) : 0;
}

static inline void
m2m_crash_record(int signal_number, siginfo_t *info, void *context_in)
{
	ucontext_t *context = context_in;
	char where[M2M_RECORD_MAX - 96] = "";
	size_t used = 0;
	uintptr_t pc = 0, lr = 0, fp = 0;

	if (context != NULL && context->uc_mcontext != NULL) {
		pc = (uintptr_t)context->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)context->uc_mcontext->__ss.__lr;
		fp = (uintptr_t)context->uc_mcontext->__ss.__fp;
	}
	used += (size_t)snprintf(where + used, sizeof(where) - used, " pc");
	used += m2m_where(where + used, sizeof(where) - used, pc);
	used += (size_t)snprintf(where + used, sizeof(where) - used, "; lr");
	used += m2m_where(where + used, sizeof(where) - used, lr);
	used += (size_t)snprintf(where + used, sizeof(where) - used, "; callers");
	/* the frame chain: [fp] the previous frame, [fp + 8] its return address; it only ever goes up the stack */
	for (unsigned i = 0; i < 6 && fp != 0 && (fp & 7) == 0 && used + 8 < sizeof(where); i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		if (ret == 0) {
			break;
		}
		used += m2m_where(where + used, sizeof(where) - used, ret);
		fp = next > fp ? next : 0;
	}
	m2m_log('F', "CRASH", "signal %d at %p:%s", signal_number, info != NULL ? info->si_addr : NULL, where);
}

static void __attribute__((unused))
m2m_crash_handler(int signal_number, siginfo_t *info, void *context)
{
	m2m_crash_record(signal_number, info, context);
	signal(signal_number, SIG_DFL);
	raise(signal_number);
}

/* the handler, for each fatal signal the process has left at its default: a handler of its own is never replaced */
static inline void
m2m_watch_crashes(void)
{
	static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT, SIGFPE };
	struct sigaction action, old;

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = m2m_crash_handler;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (sigaction(signals[i], NULL, &old) == 0 && !(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_DFL) {
			sigaction(signals[i], &action, NULL);
		}
	}
}

#endif /* REDWOOD_M2M_H */
