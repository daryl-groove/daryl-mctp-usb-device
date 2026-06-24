#include "ffs_daemon.h"

#include <stdio.h>
#include <string.h>

/*
 * ffsd — script-driven FunctionFS MCTP daemon.
 * The gadget is created and the UDC bound externally by scripts/gadget.sh;
 * ffsd only owns the FunctionFS side: write descriptors to ep0 and run the
 * MCTP event loop.
 *
 *   --echo   : raw loopback (read OUT -> write IN)
 *   default  : MCTP responder (Set/Get EID, Get UUID, Get Msg Type)
 */

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [--echo] <functionfs-mountpoint>\n", prog);
}

int main(int argc, char **argv)
{
	ffs_mode_t  mode  = FFS_MODE_MCTP;
	const char *mount = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--echo") == 0) {
			mode = FFS_MODE_ECHO;
		} else if (!mount) {
			mount = argv[i];
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	if (!mount) {
		usage(argv[0]);
		return 2;
	}

	/* No on_ready: gadget.sh binds the UDC after descriptors are written. */
	return ffs_serve(mount, mode, NULL, NULL);
}
