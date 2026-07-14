/* Inject evdev key events into an input device node (dev/testing tool).
 * Usage: evinject /dev/input/event3 316:1 314:1 sleep:200 316:0 314:0
 * code:value writes EV_KEY code=value followed by EV_SYN; sleep:ms waits. */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static int emit(int fd, int type, int code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof ev);
	gettimeofday(&ev.time, NULL);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	return write(fd, &ev, sizeof ev) == (ssize_t)sizeof ev ? 0 : -1;
}

int main(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <event-dev> code:value|sleep:ms ...\n", argv[0]);
		return 2;
	}
	int fd = open(argv[1], O_WRONLY);
	if (fd < 0) { perror(argv[1]); return 1; }
	for (int i = 2; i < argc; i++) {
		int a, b;
		if (sscanf(argv[i], "sleep:%d", &a) == 1) {
			usleep(a * 1000);
		} else if (sscanf(argv[i], "abs:%d:%d", &a, &b) == 2) {
			if (emit(fd, EV_ABS, a, b) != 0 || emit(fd, EV_SYN, 0, 0) != 0) {
				perror("write");
				return 1;
			}
		} else if (sscanf(argv[i], "%d:%d", &a, &b) == 2) {
			if (emit(fd, EV_KEY, a, b) != 0 || emit(fd, EV_SYN, 0, 0) != 0) {
				perror("write");
				return 1;
			}
		} else {
			fprintf(stderr, "bad arg: %s\n", argv[i]);
			return 2;
		}
	}
	close(fd);
	return 0;
}
