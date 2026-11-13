/* Copyright (c) 2026 Darin. MIT License (see LICENSE); keep this notice. Written by Claude (Anthropic) for the ios-sweet Galaxy A8 port. */
/*
 * sweet-settime SECONDS: set the clock to a Unix time (root), and say what it was and is.
 *
 * The A8 has no RTC this kernel reads, so every boot starts at 1970 (the lock screen shows Dec 31 1969 in CST).
 * Suspected cost: Apple's signing certificates are not valid yet, so signature checks of extensions fail and
 * LaunchServices registers none (no posters, no widgets; G9 runs 10-12). sweet-run runs this as its first
 * command, at about 46 s, before lsd starts (about 55 s).
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

int
main(int argc, char **argv)
{
	struct timeval before, want = { 0 }, after;

	if (argc != 2 || (want.tv_sec = strtol(argv[1], NULL, 10)) <= 0) {
		fprintf(stderr, "usage: sweet-settime SECONDS\n");
		return 2;
	}
	gettimeofday(&before, NULL);
	if (settimeofday(&want, NULL) != 0) {
		perror("sweet-settime: settimeofday");
		return 1;
	}
	gettimeofday(&after, NULL);
	printf("sweet-settime: clock was %ld, asked %ld, now %ld\n", (long)before.tv_sec, (long)want.tv_sec,
	    (long)after.tv_sec);
	return 0;
}
