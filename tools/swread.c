/* swread: report the physical audio switch (SW_TABLET_MODE on event3).
 *
 * Exit 0 when the switch selects Bluetooth (up), 1 when it selects the speaker
 * (down) OR the switch can't be read. launch.sh uses this to decide whether
 * in-game (minarch) audio should route to a connected BT sink -- speaker is the
 * safe default, so an unreadable switch never forces audio onto Bluetooth.
 *
 * Polarity matches musicd (apps/music/player.c): bit 0 = switch up = BT. */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

int main(void)
{
	int fd = open("/dev/input/event3", O_RDONLY);
	if (fd < 0) return 1;
	unsigned long bits[(SW_MAX + 1 + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
	memset(bits, 0, sizeof bits);
	int rc = ioctl(fd, EVIOCGSW(sizeof bits), bits);
	close(fd);
	if (rc < 0) return 1;
	int on = (bits[SW_TABLET_MODE / (8 * sizeof(long))]
	          >> (SW_TABLET_MODE % (8 * sizeof(long)))) & 1;
	return on ? 1 : 0;   /* bit 0 (switch up) = BT -> exit 0 */
}
