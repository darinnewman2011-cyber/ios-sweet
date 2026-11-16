/*
 * sweet-clean: frees space on the A8's iOS root when it is full (the root has no rm).
 * Deletes the files (not the folders) under iOS's unified-log store and the crash reports,
 * and prints free space before and after plus the size of a few big folders.
 * Usage: sweet-clean [-n]   (-n = report only)
 * Copyright (c) 2026 Darin. MIT licence (see LICENSE).
 */
#include <ftw.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int dry;
static long long files, bytes, sum;

static int del(const char *p, const struct stat *st, int type, struct FTW *f)
{
	(void)f;
	if (type == FTW_F || type == FTW_SL) {
		long long b = (long long)st->st_blocks * 512;
		if (dry || unlink(p) == 0) {
			files++;
			bytes += b;
		}
	}
	return 0;
}

static int add(const char *p, const struct stat *st, int type, struct FTW *f)
{
	(void)p; (void)f;
	if (type == FTW_F)
		sum += (long long)st->st_blocks * 512;
	return 0;
}

static void freespace(const char *when)
{
	struct statfs s;
	if (statfs("/", &s) == 0)
		printf("sweet-clean: %s: %lld MB free of %lld MB\n", when,
		    (long long)s.f_bavail * s.f_bsize >> 20, (long long)s.f_blocks * s.f_bsize >> 20);
}

int main(int argc, char **argv)
{
	static const char *junk[] = {
		"/private/var/db/diagnostics/Persist", "/private/var/db/diagnostics/Special",
		"/private/var/db/diagnostics/Signpost", "/private/var/db/diagnostics/HighVolume",
		"/private/var/mobile/Library/Logs/CrashReporter", "/private/var/root/Library/Logs/CrashReporter",
	};
	static const char *big[] = {
		"/private/var/db", "/private/var/root", "/private/var/containers", "/private/var/mobile/Library",
		"/private/var/mobile/Media", "/private/var/tmp", "/private/var/folders", "/sweet",
	};
	dry = argc > 1 && strcmp(argv[1], "-n") == 0;
	freespace("before");
	for (unsigned i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
		sum = 0;
		nftw(big[i], add, 32, FTW_PHYS | FTW_MOUNT);
		printf("sweet-clean: %-32s %lld MB\n", big[i], sum >> 20);
	}
	for (unsigned i = 0; i < sizeof(junk) / sizeof(junk[0]); i++) {
		files = bytes = 0;
		nftw(junk[i], del, 32, FTW_PHYS | FTW_MOUNT | FTW_DEPTH);
		printf("sweet-clean: %s %s: %lld files, %lld MB\n", dry ? "would delete" : "deleted", junk[i], files,
		    bytes >> 20);
	}
	sync();
	freespace("after");
	return 0;
}
