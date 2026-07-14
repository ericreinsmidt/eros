/* Print EV_KEY events from one or more input devices, for discovering
 * button codes. Usage: evread /dev/input/event0 /dev/input/event3 ... */
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
	int n = argc - 1;
	if (n <= 0) { fprintf(stderr, "usage: %s <event-dev>...\n", argv[0]); return 2; }
	struct pollfd pfd[16];
	if (n > 16) n = 16;
	for (int i = 0; i < n; i++) {
		pfd[i].fd = open(argv[i + 1], O_RDONLY);
		pfd[i].events = POLLIN;
	}
	for (;;) {
		if (poll(pfd, n, -1) <= 0) continue;
		for (int i = 0; i < n; i++) {
			if (!(pfd[i].revents & POLLIN)) continue;
			struct input_event ev;
			while (read(pfd[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
				if (ev.type == EV_KEY)
					printf("%s type=KEY code=%d value=%d\n",
					       argv[i + 1], ev.code, ev.value);
				else if (ev.type == EV_ABS)
					printf("%s type=ABS code=%d value=%d\n",
					       argv[i + 1], ev.code, ev.value);
				fflush(stdout);
			}
		}
	}
	return 0;
}
