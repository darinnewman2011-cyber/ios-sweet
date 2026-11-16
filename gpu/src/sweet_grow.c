/*
 * sweet-grow: grows the mounted HFS+ root to fill its device (the A8's root.img after it was made
 * longer in TWRP; the kernel's root device takes its size from the file at boot).
 * Asks HFS to resize the live volume (HFSIOC_RESIZE_VOLUME, as diskutil resizeVolume does).
 * Usage: sweet-grow [-n] [MB]   (-n = report only; MB = new size, default: the whole device)
 * Copyright (c) 2026 Darin. MIT licence (see LICENSE).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>

#define DKIOCGETBLOCKSIZE       _IOR('d', 24, uint32_t)
#define DKIOCGETBLOCKCOUNT      _IOR('d', 25, uint64_t)
#define HFSIOC_RESIZE_VOLUME    _IOW('h', 16, uint64_t)
int fsctl(const char *, unsigned long, void *, unsigned int);

static void show(const char *when)
{
	struct statfs s;
	if (statfs("/", &s) == 0)
		printf("sweet-grow: %s: volume %lld MB, %lld MB free (block %u) on %s\n", when,
		    (long long)s.f_blocks * s.f_bsize >> 20, (long long)s.f_bavail * s.f_bsize >> 20, s.f_bsize,
		    s.f_mntfromname);
}

int main(int argc, char **argv)
{
	int dry = argc > 1 && strcmp(argv[1], "-n") == 0;
	struct statfs s;
	uint32_t bs = 0;
	uint64_t count = 0, want;

	show("before");
	if (statfs("/", &s) != 0) {
		perror("sweet-grow: statfs");
		return 1;
	}
	/* the block device is busy while mounted (EBUSY, run 53): ask the raw one, /dev/rNAME */
	char raw[sizeof(s.f_mntfromname) + 2];
	const char *slash = strrchr(s.f_mntfromname, '/');
	snprintf(raw, sizeof(raw), "/dev/r%s", slash != NULL ? slash + 1 : s.f_mntfromname);
	int fd = open(raw, O_RDONLY);
	if (fd < 0 || ioctl(fd, DKIOCGETBLOCKSIZE, &bs) != 0 || ioctl(fd, DKIOCGETBLOCKCOUNT, &count) != 0) {
		perror("sweet-grow: device size");
		return 1;
	}
	close(fd);
	want = (uint64_t)bs * count;
	if (argc > 1 + dry)
		want = strtoull(argv[1 + dry], NULL, 0) << 20;
	want &= ~(uint64_t)(s.f_bsize - 1);
	printf("sweet-grow: device %llu MB (%u x %llu); volume to %llu MB\n", (unsigned long long)((uint64_t)bs * count >> 20),
	    bs, (unsigned long long)count, (unsigned long long)(want >> 20));
	if (want <= (uint64_t)s.f_blocks * s.f_bsize) {
		printf("sweet-grow: nothing to grow\n");
		return 0;
	}
	if (dry)
		return 0;
	if (fsctl("/", HFSIOC_RESIZE_VOLUME, &want, 0) != 0) {
		perror("sweet-grow: HFSIOC_RESIZE_VOLUME");
		return 1;
	}
	sync();
	show("after");
	return 0;
}
