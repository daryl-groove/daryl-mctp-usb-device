#include "ffs_daemon.h"
#include "ffs_pty.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <usbg/usbg.h>

/*
 * mctpusbd — MCTP-over-USB gadget daemon (Option A1: PTY + mctp-serial bridge).
 * Creates the USB gadget via libusbgx, mounts FunctionFS, opens a PTY, attaches
 * N_MCTP line discipline, and bridges USB bulk transfers ↔ PTY master fd.
 *
 *   --echo          : raw loopback mode (for hardware testing)
 *   --udc <name>    : bind to a named UDC (default: auto-pick first)
 *   <mountpoint>    : FunctionFS mount path (default: /dev/ffs-mctp)
 */

#define CONFIGFS_PATH "/sys/kernel/config"
#define GADGET_NAME   "mctp"
#define FFS_INSTANCE  "mctp"    /* -> functions/ffs.mctp; also mount source */
#define VID           0x1d6bu
#define PID           0x0104u

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void install_signal_handlers(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; /* no SA_RESTART: poll() wakes with EINTR */
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static int usbg_check(int ret, const char *what)
{
	if (ret == USBG_SUCCESS)
		return 0;
	fprintf(stderr, "%s: %s (%s)\n", what,
	        usbg_strerror((usbg_error)ret),
	        usbg_error_name((usbg_error)ret));
	return -1;
}

typedef struct {
	usbg_gadget *gadget;
	const char  *udc_name; /* NULL = auto */
	usbg_state  *usbg;
} on_ready_ctx_t;

static int on_ready_enable_udc(void *userdata)
{
	on_ready_ctx_t *ctx = (on_ready_ctx_t *)userdata;

	usbg_udc *udc = NULL;
	if (ctx->udc_name) {
		udc = usbg_get_udc(ctx->usbg, ctx->udc_name);
		if (!udc) {
			fprintf(stderr, "UDC '%s' not found\n", ctx->udc_name);
			return -1;
		}
	}

	if (usbg_check(usbg_enable_gadget(ctx->gadget, udc), "enable_gadget") < 0)
		return -1;

	printf("UDC bound (%s); enumerating\n",
	       ctx->udc_name ? ctx->udc_name : "auto");
	return 0;
}

typedef struct {
	ffs_mode_t  mode;
	const char *udc_name;
	const char *mount;
} options_t;

static int parse_args(int argc, char **argv, options_t *opt)
{
	opt->mode     = FFS_MODE_PTY_BRIDGE;
	opt->udc_name = NULL;
	opt->mount    = "/dev/ffs-mctp";

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--echo") == 0) {
			opt->mode = FFS_MODE_ECHO;
		} else if (strcmp(argv[i], "--udc") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "--udc needs a name\n");
				return -1;
			}
			opt->udc_name = argv[i];
		} else if (argv[i][0] != '-') {
			opt->mount = argv[i];
		} else {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	options_t opt;
	if (parse_args(argc, argv, &opt) < 0) {
		fprintf(stderr,
		        "usage: %s [--echo] [--udc <name>] [functionfs-mountpoint]\n",
		        argv[0]);
		return 2;
	}

	install_signal_handlers();

	usbg_state    *s = NULL;
	usbg_gadget   *g = NULL;
	usbg_function *f = NULL;
	usbg_config   *c = NULL;
	int mounted     = 0;
	int pty_master  = -1;
	int pty_slave   = -1;
	int rc          = 1;

	if (usbg_check(usbg_init(CONFIGFS_PATH, &s), "usbg_init") < 0)
		return 1;

	struct usbg_gadget_attrs g_attrs;
	memset(&g_attrs, 0, sizeof(g_attrs));
	g_attrs.bcdUSB          = 0x0200;
	g_attrs.bDeviceClass    = 0x00;
	g_attrs.bMaxPacketSize0 = 64;
	g_attrs.idVendor        = VID;
	g_attrs.idProduct       = PID;
	g_attrs.bcdDevice       = 0x0100;

	struct usbg_gadget_strs g_strs;
	memset(&g_strs, 0, sizeof(g_strs));
	g_strs.manufacturer = (char *)"daryl";
	g_strs.product      = (char *)"MCTP FFS device";
	g_strs.serial       = (char *)"0123456789";

	struct usbg_config_strs c_strs;
	memset(&c_strs, 0, sizeof(c_strs));
	c_strs.configuration = (char *)"config 1";

	if (usbg_check(usbg_create_gadget(s, GADGET_NAME, &g_attrs, &g_strs, &g),
	               "create_gadget") < 0)
		goto cleanup;
	if (usbg_check(usbg_create_function(g, USBG_F_FFS, FFS_INSTANCE, NULL, &f),
	               "create_function(ffs)") < 0)
		goto cleanup;
	if (usbg_check(usbg_create_config(g, 1, NULL, NULL, &c_strs, &c),
	               "create_config") < 0)
		goto cleanup;
	if (usbg_check(usbg_add_config_function(c, "ffs.mctp", f),
	               "add_config_function") < 0)
		goto cleanup;

	mkdir(opt.mount, 0755); /* ignore EEXIST */
	if (mount(FFS_INSTANCE, opt.mount, "functionfs", 0, NULL) != 0) {
		fprintf(stderr, "mount functionfs at %s failed: %s\n",
		        opt.mount, strerror(errno));
		goto cleanup;
	}
	mounted = 1;

	printf("gadget '%s' created; functionfs mounted at %s\n",
	       GADGET_NAME, opt.mount);

	if (opt.mode == FFS_MODE_PTY_BRIDGE) {
		if (ffs_pty_open(&pty_master, &pty_slave) < 0)
			goto cleanup;
	}

	on_ready_ctx_t ready_ctx = { g, opt.udc_name, s };
	rc = ffs_serve(opt.mount, opt.mode, pty_master,
	               on_ready_enable_udc, &ready_ctx);

cleanup:
	if (pty_slave  >= 0) close(pty_slave);
	if (pty_master >= 0) close(pty_master);
	if (g) usbg_disable_gadget(g);
	if (mounted && umount(opt.mount) != 0)
		fprintf(stderr, "umount %s failed: %s\n", opt.mount, strerror(errno));
	if (g) usbg_rm_gadget(g, USBG_RM_RECURSE);
	usbg_cleanup(s);
	return rc;
}
