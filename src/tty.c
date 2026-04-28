#include "tty.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;
static int have_saved;
static int saved_flags;
static int have_flags;

int tty_raw(void)
{
	if (!isatty(STDIN_FILENO)) return -1;

	if (tcgetattr(STDIN_FILENO, &saved) != 0) return -1;
	have_saved = 1;

	/*
	 * Keep it minimal: disable canonical mode and echo, set VMIN=0
	 * VTIME=0 so read() is non-blocking when combined with O_NONBLOCK.
	 * Leave ISIG enabled so Ctrl-C still delivers SIGINT.
	 */
	struct termios raw = saved;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN]  = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
		have_saved = 0;
		return -1;
	}

	saved_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
	if (saved_flags >= 0) {
		have_flags = 1;
		fcntl(STDIN_FILENO, F_SETFL, saved_flags | O_NONBLOCK);
	}

	/* Ensure we restore on abnormal exit paths (e.g. die()). */
	atexit(tty_restore);
	return 0;
}

void tty_restore(void)
{
	if (have_flags) {
		fcntl(STDIN_FILENO, F_SETFL, saved_flags);
		have_flags = 0;
	}
	if (have_saved) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
		have_saved = 0;
	}
}

int tty_peek(void)
{
	unsigned char c;
	ssize_t r = read(STDIN_FILENO, &c, 1);
	if (r == 1) return c;
	if (r == 0) return -1;
	if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
	return -2;
}
