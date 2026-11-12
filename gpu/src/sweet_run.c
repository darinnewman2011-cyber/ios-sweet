/*
 * sweet-run: run the commands in /sweet/run.cmd once per boot (launchd job com.sweet.run) and keep their output in
 * /sweet/run.log as well as on the console. Both files have a fixed size and are only ever overwritten in place, so
 * from TWRP (no HFS+ there) tools/a8run.py can write the commands and read the log straight through their blocks in
 * root.img; the 16 KB console window in ramoops is too small for test output.
 *
 * run.cmd: text up to the first NUL byte, one command per line, split at spaces (no shell, no quoting):
 *   # comment
 *   delay N          wait N seconds (before the first command: let the boot settle)
 *   timeout N        kill each later command after N seconds (default 120)
 *   env NAME=VALUE   set an environment variable for later commands
 *   /path/prog args  run it, stdout and stderr into the log
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CMD_PATH "/sweet/run.cmd"
#define LOG_PATH "/sweet/run.log"
#define CMD_MAX (64 * 1024)
#define LOG_SIZE (1024 * 1024)

extern char **environ;

static int logfd = -1, con = -1;
static off_t logpos;

/*
 * The history: /sweet/runs.log holds the logs of the last RUNS_SLOTS boots, one slot each, so a later boot does not
 * destroy the log that was wanted (run.log alone is rewritten at every boot). A slot starts with "SWEETBOOT N";
 * this boot takes the slot without that line, or else the one with the lowest N, and numbers itself max + 1.
 * Fixed size, only ever overwritten in place, as run.log (tools/a8run.py runslog reads it from TWRP).
 */
#define RUNS_PATH "/sweet/runs.log"
#define RUNS_SLOTS 8
#define RUNS_SLOT (1024 * 1024)
static int runsfd = -1;
static off_t runsbase, runspos;

static void
out(const char *buf, size_t n)
{
	if (con >= 0) {
		write(con, buf, n);
	}
	if (logfd >= 0 && logpos < LOG_SIZE - 1) {
		if (logpos + (off_t)n > LOG_SIZE - 1) {
			n = (size_t)(LOG_SIZE - 1 - logpos);
		}
		pwrite(logfd, buf, n, logpos);
		logpos += n;
	}
	if (runsfd >= 0 && runspos < RUNS_SLOT - 1) {
		size_t m = runspos + (off_t)n > RUNS_SLOT - 1 ? (size_t)(RUNS_SLOT - 1 - runspos) : n;

		pwrite(runsfd, buf, m, runsbase + runspos);
		runspos += m;
	}

	/* to the disk every 2 s, not only after each command: a long one (sweet-dmesg -f) runs until the boot ends,
	 * and a forced restart would take everything still in the cache with it */
	static struct timespec synced;
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	if (t.tv_sec - synced.tv_sec >= 2) {
		synced = t;
		if (logfd >= 0) {
			fsync(logfd);
		}
		if (runsfd >= 0) {
			fsync(runsfd);
		}
	}
}

static void
runs_open(const char *zero, size_t zero_size)
{
	unsigned long best = 0, newest = 0;
	int slot = -1;
	char head[32];

	if ((runsfd = open(RUNS_PATH, O_RDWR)) < 0) {
		return;
	}
	for (int i = 0; i < RUNS_SLOTS; i++) {
		unsigned long n = 0;

		memset(head, 0, sizeof(head));
		pread(runsfd, head, sizeof(head) - 1, (off_t)i * RUNS_SLOT);
		if (sscanf(head, "SWEETBOOT %lu", &n) != 1 || n == 0) {
			if (slot < 0 || best != 0) {
				slot = i;       /* a free slot: the first one wins over any used one */
				best = 0;
			}
			continue;
		}
		if (n > newest) {
			newest = n;
		}
		if (slot < 0 || (best != 0 && n < best)) {
			slot = i;
			best = n;
		}
	}
	runsbase = (off_t)slot * RUNS_SLOT;
	for (off_t o = 0; o < RUNS_SLOT; o += (off_t)zero_size) {
		pwrite(runsfd, zero, zero_size, runsbase + o);
	}
	runspos = snprintf(head, sizeof(head), "SWEETBOOT %lu\n", newest + 1);
	pwrite(runsfd, head, (size_t)runspos, runsbase);
}

static void
outf(const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n > 0) {
		out(b, n < (int)sizeof(b) ? (size_t)n : sizeof(b) - 1);
	}
}

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void
run(char **argv, int timeout)
{
	posix_spawn_file_actions_t fa;
	int p[2], status = 0;
	pid_t pid;
	double t0 = now();
	bool killed = false;

	if (pipe(p) != 0) {
		outf("sweet-run: pipe: %s\n", strerror(errno));
		return;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	posix_spawn_file_actions_adddup2(&fa, p[1], 2);
	posix_spawn_file_actions_addclose(&fa, p[0]);
	errno = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	if (errno != 0) {
		outf("sweet-run: cannot start %s: %s\n", argv[0], strerror(errno));
		close(p[0]);
		return;
	}
	for (;;) {
		struct pollfd pf = { .fd = p[0], .events = POLLIN };
		char b[4096];
		ssize_t n;

		if (!killed && now() - t0 > timeout) {
			outf("\nsweet-run: %s still running after %d s, killed\n", argv[0], timeout);
			kill(pid, SIGKILL);
			killed = true;
		}
		if (poll(&pf, 1, 1000) <= 0) {
			continue;
		}
		if ((n = read(p[0], b, sizeof(b))) <= 0) {
			break;
		}
		out(b, (size_t)n);
	}
	close(p[0]);
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status)) {
		outf("sweet-run: %s: signal %d after %.1f s\n", argv[0], WTERMSIG(status), now() - t0);
	} else {
		outf("sweet-run: %s: exit %d after %.1f s\n", argv[0], WEXITSTATUS(status), now() - t0);
	}
	fsync(logfd);
}

int
main(void)
{
	static char cmd[CMD_MAX + 1], zero[64 * 1024];
	int timeout = 120, fd;
	ssize_t n;

	con = open("/dev/console", O_WRONLY | O_NOCTTY);
	logfd = open(LOG_PATH, O_WRONLY);
	if (logfd >= 0) {
		for (off_t o = 0; o < LOG_SIZE; o += sizeof(zero)) {
			pwrite(logfd, zero, sizeof(zero), o);
		}
	}
	runs_open(zero, sizeof(zero));
	if ((fd = open(CMD_PATH, O_RDONLY)) < 0 || (n = read(fd, cmd, CMD_MAX)) < 0) {
		outf("sweet-run: no %s: %s\n", CMD_PATH, strerror(errno));
		return 1;
	}
	close(fd);
	cmd[n] = 0;
	outf("sweet-run: started, pid %d, %zu bytes of commands\n", getpid(), strlen(cmd));
	for (char *line = cmd, *next; line != NULL && *line != 0; line = next) {
		char *argv[32];
		int argc = 0;

		if ((next = strchr(line, '\n')) != NULL) {
			*next++ = 0;
		}
		for (char *t = strtok(line, " \t\r"); t != NULL && argc < 31; t = strtok(NULL, " \t\r")) {
			argv[argc++] = t;
		}
		argv[argc] = NULL;
		if (argc == 0 || argv[0][0] == '#') {
			continue;
		}
		if (strcmp(argv[0], "delay") == 0 && argc == 2) {
			outf("sweet-run: waiting %s s\n", argv[1]);
			sleep((unsigned)atoi(argv[1]));
		} else if (strcmp(argv[0], "timeout") == 0 && argc == 2) {
			timeout = atoi(argv[1]);
		} else if (strcmp(argv[0], "env") == 0 && argc == 2 && strchr(argv[1], '=') != NULL) {
			outf("sweet-run: env %s\n", argv[1]);
			putenv(strdup(argv[1]));
		} else {
			outf("sweet-run: $");
			for (int i = 0; i < argc; i++) {
				outf(" %s", argv[i]);
			}
			outf("\n");
			run(argv, timeout);
		}
	}
	outf("sweet-run: done\n");
	fsync(logfd);
	return 0;
}
