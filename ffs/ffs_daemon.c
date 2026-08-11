#include "ffs_daemon.h"

#include "ffs_descriptors.h"
#include "mctp_endpoint.h"
#include "mctp_serial_frame.h"
#include "mctp_usb_frame.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/usb/functionfs.h>

volatile sig_atomic_t g_stop = 0;

/* Largest OUT transfer we accept: 4 (USB hdr) + 255 (max payload) + headroom. */
#define MAX_FRAME 1024u
/* Max USB frame we ever send back: same bound. */
#define MAX_RESP  1024u

/* Build path "<mount>/<name>" into buf[]. */
static void path_join(char *buf, size_t buf_size,
                      const char *mount, const char *name)
{
	snprintf(buf, buf_size, "%s/%s", mount, name);
}

static int write_all(int fd, const void *buf, size_t len, const char *what)
{
	ssize_t rc = write(fd, buf, len);
	if (rc < 0) {
		fprintf(stderr, "write %s failed: %s\n", what, strerror(errno));
		return -1;
	}
	if ((size_t)rc != len) {
		fprintf(stderr, "write %s short: %zd/%zu\n", what, rc, len);
		return -1;
	}
	return 0;
}

/* ── PTY rx state machine ───────────────────────────────────────────────────── */

typedef enum {
	PTY_RX_HUNT,      /* waiting for start sync 0x7E */
	PTY_RX_LEN_HI,   /* reading length high byte */
	PTY_RX_LEN_LO,   /* reading length low byte */
	PTY_RX_PAYLOAD,  /* reading payload bytes */
	PTY_RX_FCS_HI,   /* reading FCS high byte */
	PTY_RX_FCS_LO,   /* reading FCS low byte */
	PTY_RX_END,      /* expecting end sync 0x7E */
} pty_rx_state_t;

/* ── ep0 lifecycle ─────────────────────────────────────────────────────────── */

typedef struct {
	char         mount[256];
	ffs_mode_t   mode;
	int          ep_out;   /* ep1, host->device (we read); -1 = closed */
	int          ep_in;    /* ep2, device->host (we write); -1 = closed */
	int          mps_in;   /* ep2 wMaxPacketSize for ZLP; 0 = unknown   */
	int          pty_master_fd; /* PTY bridge mode only; -1 otherwise    */
	mctp_endpoint_state_t ep_state;
	/* PTY rx reassembly */
	pty_rx_state_t  pty_rx_state;
	uint8_t         pty_rx_buf[MCTP_SERIAL_MAX_FRAME];
	size_t          pty_rx_pos;
	size_t          pty_rx_payload_left;
} ffs_ctx_t;

static const char *event_name(uint8_t type)
{
	switch (type) {
	case FUNCTIONFS_BIND:    return "BIND";
	case FUNCTIONFS_UNBIND:  return "UNBIND";
	case FUNCTIONFS_ENABLE:  return "ENABLE";
	case FUNCTIONFS_DISABLE: return "DISABLE";
	case FUNCTIONFS_SETUP:   return "SETUP";
	case FUNCTIONFS_SUSPEND: return "SUSPEND";
	case FUNCTIONFS_RESUME:  return "RESUME";
	default:                  return "?";
	}
}

static void on_enable(ffs_ctx_t *ctx)
{
	if (ctx->ep_out >= 0)
		return; /* already enabled */

	char p1[256], p2[256];
	path_join(p1, sizeof(p1), ctx->mount, "ep1");
	path_join(p2, sizeof(p2), ctx->mount, "ep2");

	ctx->ep_out = open(p1, O_RDWR);
	ctx->ep_in  = open(p2, O_RDWR);
	if (ctx->ep_out < 0 || ctx->ep_in < 0) {
		fprintf(stderr, "open ep1/ep2 failed: %s\n", strerror(errno));
		if (ctx->ep_out >= 0) { close(ctx->ep_out); ctx->ep_out = -1; }
		if (ctx->ep_in  >= 0) { close(ctx->ep_in);  ctx->ep_in  = -1; }
		return;
	}

	struct usb_endpoint_descriptor desc;
	memset(&desc, 0, sizeof(desc));
	ctx->mps_in = (ioctl(ctx->ep_in, FUNCTIONFS_ENDPOINT_DESC, &desc) == 0)
	              ? (int)le16toh(desc.wMaxPacketSize) : 0;

	const char *mode_name =
		ctx->mode == FFS_MODE_ECHO        ? "echo" :
		ctx->mode == FFS_MODE_PTY_BRIDGE  ? "pty-bridge" : "mctp";
	printf("ENABLE: ep1/ep2 open, mode=%s, IN mps=%d\n", mode_name, ctx->mps_in);
}

static void on_disable(ffs_ctx_t *ctx)
{
	if (ctx->ep_out >= 0) { close(ctx->ep_out); ctx->ep_out = -1; }
	if (ctx->ep_in  >= 0) { close(ctx->ep_in);  ctx->ep_in  = -1; }
	ctx->mps_in = 0;
}

/* Write one IN frame; append ZLP when the frame exactly fills whole MPS packets. */
static void write_in_frame(ffs_ctx_t *ctx,
                           const uint8_t *frame, size_t frame_len)
{
	if (write_all(ctx->ep_in, frame, frame_len, "ep-IN") < 0)
		return;
	if (ctx->mps_in > 0 && frame_len > 0 &&
	    (frame_len % (size_t)ctx->mps_in) == 0) {
		write(ctx->ep_in, frame, 0); /* zero-length packet */
		printf("    (+ZLP: frame fills whole packets)\n");
	}
}

/* ── USB OUT handler ────────────────────────────────────────────────────────── */

static void handle_out(ffs_ctx_t *ctx)
{
	uint8_t buf[MAX_FRAME];
	ssize_t n = read(ctx->ep_out, buf, sizeof(buf));
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return;
		fprintf(stderr, "read ep-OUT failed: %s\n", strerror(errno));
		return;
	}
	if (n == 0)
		return;

	switch (ctx->mode) {
	case FFS_MODE_ECHO:
		write_in_frame(ctx, buf, (size_t)n);
		return;

	case FFS_MODE_MCTP: {
		uint8_t resp[MAX_RESP];
		const size_t resp_len = mctp_endpoint_process(
			&ctx->ep_state, buf, (size_t)n, resp, sizeof(resp));
		if (resp_len == 0) {
			printf("    -> no response\n");
			return;
		}
		write_in_frame(ctx, resp, resp_len);
		printf("    endpoint state: EID=%u\n", (unsigned)ctx->ep_state.eid);
		return;
	}

	case FFS_MODE_PTY_BRIDGE: {
		const mctp_usb_decode_result_t d = mctp_usb_decode(buf, (size_t)n);
		if (d.error != MCTP_USB_DECODE_OK) {
			fprintf(stderr, "USB->PTY: USB decode error %d\n", d.error);
			return;
		}
		uint8_t serial_frame[MAX_FRAME + MCTP_SERIAL_OVERHEAD];
		const size_t flen = mctp_serial_encode(
			d.payload, d.payload_len,
			serial_frame, sizeof(serial_frame));
		if (flen == 0) {
			fprintf(stderr, "USB->PTY: serial encode failed\n");
			return;
		}
		write_all(ctx->pty_master_fd, serial_frame, flen, "PTY master");
		return;
	}
	}
}

/* ── PTY master handler (kernel → USB IN) ───────────────────────────────────── */

static void pty_rx_reset(ffs_ctx_t *ctx)
{
	ctx->pty_rx_state        = PTY_RX_HUNT;
	ctx->pty_rx_pos          = 0;
	ctx->pty_rx_payload_left = 0;
}

static void pty_rx_dispatch(ffs_ctx_t *ctx)
{
	const uint8_t *mctp_pkt;
	size_t         mctp_len;

	if (mctp_serial_decode(ctx->pty_rx_buf, ctx->pty_rx_pos,
	                       &mctp_pkt, &mctp_len) < 0) {
		fprintf(stderr, "PTY->USB: serial decode failed (FCS error?)\n");
		return;
	}

	if (ctx->ep_in < 0)
		return; /* USB not enabled; discard */

	uint8_t usb_frame[MAX_RESP];
	const size_t usb_len = mctp_usb_encode(mctp_pkt, mctp_len,
	                                       usb_frame, sizeof(usb_frame));
	if (usb_len == 0) {
		fprintf(stderr, "PTY->USB: USB encode failed (packet too large?)\n");
		return;
	}

	write_in_frame(ctx, usb_frame, usb_len);
}

static void pty_rx_feed(ffs_ctx_t *ctx, uint8_t byte)
{
	switch (ctx->pty_rx_state) {
	case PTY_RX_HUNT:
		if (byte != MCTP_SERIAL_SYNC)
			break;
		ctx->pty_rx_buf[0] = byte;
		ctx->pty_rx_pos    = 1;
		ctx->pty_rx_state  = PTY_RX_LEN_HI;
		break;

	case PTY_RX_LEN_HI:
		ctx->pty_rx_buf[ctx->pty_rx_pos++] = byte;
		ctx->pty_rx_payload_left = (size_t)byte << 8;
		ctx->pty_rx_state = PTY_RX_LEN_LO;
		break;

	case PTY_RX_LEN_LO:
		ctx->pty_rx_buf[ctx->pty_rx_pos++] = byte;
		ctx->pty_rx_payload_left |= byte;
		if (ctx->pty_rx_payload_left == 0 ||
		    ctx->pty_rx_payload_left > MCTP_SERIAL_MAX_PAYLOAD) {
			pty_rx_reset(ctx);
			break;
		}
		ctx->pty_rx_state = PTY_RX_PAYLOAD;
		break;

	case PTY_RX_PAYLOAD:
		if (ctx->pty_rx_pos >= sizeof(ctx->pty_rx_buf)) {
			pty_rx_reset(ctx);
			break;
		}
		ctx->pty_rx_buf[ctx->pty_rx_pos++] = byte;
		if (--ctx->pty_rx_payload_left == 0)
			ctx->pty_rx_state = PTY_RX_FCS_HI;
		break;

	case PTY_RX_FCS_HI:
		ctx->pty_rx_buf[ctx->pty_rx_pos++] = byte;
		ctx->pty_rx_state = PTY_RX_FCS_LO;
		break;

	case PTY_RX_FCS_LO:
		ctx->pty_rx_buf[ctx->pty_rx_pos++] = byte;
		ctx->pty_rx_state = PTY_RX_END;
		break;

	case PTY_RX_END:
		if (byte != MCTP_SERIAL_SYNC) {
			pty_rx_reset(ctx);
			break;
		}
		ctx->pty_rx_buf[ctx->pty_rx_pos++] = byte;
		pty_rx_dispatch(ctx);
		pty_rx_reset(ctx);
		break;
	}
}

static void handle_pty_master(ffs_ctx_t *ctx)
{
	uint8_t chunk[256];
	ssize_t n = read(ctx->pty_master_fd, chunk, sizeof(chunk));
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return;
		fprintf(stderr, "read PTY master failed: %s\n", strerror(errno));
		return;
	}
	for (ssize_t i = 0; i < n; i++)
		pty_rx_feed(ctx, chunk[i]);
}

/* ── ep0 event handler ──────────────────────────────────────────────────────── */

typedef enum { EP0_CONTINUE, EP0_CLOSED, EP0_ERROR } ep0_status_t;

static ep0_status_t handle_ep0(int ep0, ffs_ctx_t *ctx)
{
	struct usb_functionfs_event events[8];
	ssize_t rc = read(ep0, events, sizeof(events));
	if (rc < 0) {
		if (errno == EINTR)
			return EP0_CONTINUE;
		fprintf(stderr, "read ep0 failed: %s\n", strerror(errno));
		return EP0_ERROR;
	}
	if (rc == 0) {
		fprintf(stderr, "ep0 closed\n");
		return EP0_CLOSED;
	}

	const size_t count = (size_t)rc / sizeof(events[0]);
	for (size_t i = 0; i < count; i++) {
		const uint8_t type = events[i].type;
		printf("event: %s\n", event_name(type));
		switch (type) {
		case FUNCTIONFS_ENABLE:
			on_enable(ctx);
			break;
		case FUNCTIONFS_DISABLE:
		case FUNCTIONFS_UNBIND:
			on_disable(ctx);
			break;
		default:
			break;
		}
	}
	return EP0_CONTINUE;
}

/* ── Public API ────────────────────────────────────────────────────────────── */

int ffs_serve(const char *mount, ffs_mode_t mode, int pty_master_fd,
              ffs_on_ready_fn on_ready, void *userdata)
{
	ffs_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	snprintf(ctx.mount, sizeof(ctx.mount), "%s", mount);
	ctx.mode          = mode;
	ctx.ep_out        = -1;
	ctx.ep_in         = -1;
	ctx.pty_master_fd = pty_master_fd;
	mctp_endpoint_state_init(&ctx.ep_state);
	pty_rx_reset(&ctx);

	char ep0_path[256];
	path_join(ep0_path, sizeof(ep0_path), mount, "ep0");
	int ep0 = open(ep0_path, O_RDWR);
	if (ep0 < 0) {
		fprintf(stderr, "open %s failed: %s\n", ep0_path, strerror(errno));
		return 1;
	}

	/* Descriptors first, then strings — kernel requires this order. */
	struct ffsdesc_descriptors desc = ffsdesc_make_descriptors();
	struct ffsdesc_strings     strs = ffsdesc_make_strings();
	if (write_all(ep0, &desc, sizeof(desc), "descriptors") < 0) goto err;
	if (write_all(ep0, &strs, sizeof(strs), "strings")     < 0) goto err;

	const char *mode_name =
		mode == FFS_MODE_ECHO        ? "echo" :
		mode == FFS_MODE_PTY_BRIDGE  ? "pty-bridge" : "mctp";
	printf("descriptors written (mode=%s); ready to bind a UDC\n", mode_name);

	if (on_ready && on_ready(userdata) != 0) {
		fprintf(stderr, "on_ready failed; aborting\n");
		goto err;
	}

	/* Poll loop: ep0 always; ep_out when USB enabled; PTY master in bridge mode. */
	for (;;) {
		if (g_stop) {
			printf("stop requested; exiting event loop\n");
			close(ep0);
			return 0;
		}

		struct pollfd fds[3];
		nfds_t nfds = 0;
		int ep_out_slot = -1, pty_slot = -1;

		fds[nfds].fd      = ep0;
		fds[nfds].events  = POLLIN;
		fds[nfds].revents = 0;
		nfds++;

		if (ctx.ep_out >= 0) {
			ep_out_slot = (int)nfds;
			fds[nfds].fd      = ctx.ep_out;
			fds[nfds].events  = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}

		if (mode == FFS_MODE_PTY_BRIDGE && ctx.pty_master_fd >= 0) {
			pty_slot = (int)nfds;
			fds[nfds].fd      = ctx.pty_master_fd;
			fds[nfds].events  = POLLIN;
			fds[nfds].revents = 0;
			nfds++;
		}

		int pr = poll(fds, nfds, -1);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
			goto err;
		}

		if (fds[0].revents & POLLIN) {
			switch (handle_ep0(ep0, &ctx)) {
			case EP0_CONTINUE: break;
			case EP0_CLOSED:   close(ep0); return 0;
			case EP0_ERROR:    goto err;
			}
		}

		if (ep_out_slot >= 0 && ctx.ep_out >= 0 &&
		    (fds[ep_out_slot].revents & POLLIN))
			handle_out(&ctx);

		if (pty_slot >= 0 && (fds[pty_slot].revents & POLLIN))
			handle_pty_master(&ctx);
	}

err:
	close(ep0);
	return 1;
}
