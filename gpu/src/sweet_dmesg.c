/*
 * sweet-dmesg: print the kernel's message buffer, as macOS' dmesg does (the iOS image has none). The console
 * window in ramoops holds 16 KB; this is the whole buffer, so a sweet-run test list ends with it and the daemons'
 * console lines (m2m records from PosterBoard, say) land in run.log.
 *
 *   sweet-dmesg           everything
 *   sweet-dmesg TEXT ...  only the lines that contain one of the TEXTs
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

/* libproc's, in libSystem on iOS too; the iOS SDK has no libproc.h */
extern int proc_kmsgbuf(void *buffer, uint32_t buffersize);

int
main(int argc, char **argv)
{
	int size = 0;
	size_t len = sizeof(size);
	char *buf, *line, *next;
	int n;

	if (sysctlbyname("kern.msgbuf", &size, &len, NULL, 0) != 0 || size <= 0) {
		size = 1 << 20;
	}
	buf = calloc(1, (size_t)size + 1);
	if (buf == NULL) {
		return 1;
	}
	n = proc_kmsgbuf(buf, (uint32_t)size);
	if (n <= 0) {
		perror("sweet-dmesg: proc_kmsgbuf");
		return 1;
	}
	buf[n < size ? n : size] = '\0';
	for (line = buf; *line != '\0'; line = next) {
		int show = argc < 2;

		next = strchr(line, '\n');
		if (next != NULL) {
			*next++ = '\0';
		} else {
			next = line + strlen(line);
		}
		for (int i = 1; i < argc && !show; i++) {
			show = strstr(line, argv[i]) != NULL;
		}
		if (show && *line != '\0') {
			puts(line);
		}
	}
	return 0;
}
