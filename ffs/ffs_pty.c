#define _XOPEN_SOURCE 700 /* grantpt, unlockpt, ptsname (XSI extensions) */
#include "ffs_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* N_MCTP line discipline number (Linux 5.15+, value is stable). */
#ifndef N_MCTP
#define N_MCTP 25
#endif

int ffs_pty_open(int *master_fd, int *slave_fd)
{
	int mfd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (mfd < 0) {
		fprintf(stderr, "open /dev/ptmx failed: %s\n", strerror(errno));
		return -1;
	}

	if (grantpt(mfd) < 0 || unlockpt(mfd) < 0) {
		fprintf(stderr, "grantpt/unlockpt failed: %s\n", strerror(errno));
		close(mfd);
		return -1;
	}

	const char *slave_path = ptsname(mfd);
	if (!slave_path) {
		fprintf(stderr, "ptsname failed: %s\n", strerror(errno));
		close(mfd);
		return -1;
	}

	int sfd = open(slave_path, O_RDWR | O_NOCTTY);
	if (sfd < 0) {
		fprintf(stderr, "open %s failed: %s\n", slave_path, strerror(errno));
		close(mfd);
		return -1;
	}

	int ldisc = N_MCTP;
	if (ioctl(sfd, TIOCSETD, &ldisc) < 0) {
		fprintf(stderr, "TIOCSETD N_MCTP on %s failed: %s\n",
		        slave_path, strerror(errno));
		close(sfd);
		close(mfd);
		return -1;
	}

	printf("PTY %s: N_MCTP attached (mctpserial interface created)\n", slave_path);

	*master_fd = mfd;
	*slave_fd  = sfd;
	return 0;
}
