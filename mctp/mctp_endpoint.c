#include "mctp_endpoint.h"

#include "mctp_usb_frame.h"
#include "mctp_packet.h"

/* Scratch buffers: largest single control response is 4 (preamble) + 16 (UUID). */
#define CTRL_BODY_BUF  32u
#define PKT_BUF        (MCTP_HDR_SIZE + CTRL_BODY_BUF)
#define FRAME_BUF      (MCTP_USB_HDR_SIZE + PKT_BUF)

size_t mctp_endpoint_process(mctp_endpoint_state_t *state,
                             const uint8_t *buf, size_t len,
                             uint8_t *out, size_t out_size)
{
	/* Decode USB frame. */
	const mctp_usb_decode_result_t frame = mctp_usb_decode(buf, len);
	if (frame.error != MCTP_USB_DECODE_OK)
		return 0;

	/* Parse MCTP transport header. */
	const mctp_parse_result_t pkt = mctp_packet_parse(frame.payload,
	                                                   frame.payload_len);
	if (pkt.error != MCTP_PARSE_OK)
		return 0;

	/* Dispatch on message type (body[0] & 0x7F). */
	if (pkt.body_len == 0)
		return 0;
	const uint8_t msg_type = pkt.body[0] & MCTP_CTRL_MSG_TYPE_MASK;

	uint8_t ctrl_body[CTRL_BODY_BUF];
	size_t  ctrl_len = 0;

	switch (msg_type) {
	case MCTP_CTRL_MSG_TYPE:
		ctrl_len = mctp_ctrl_handle(state, pkt.body, pkt.body_len,
		                            ctrl_body, sizeof(ctrl_body));
		break;
	default:
		return 0;
	}

	if (ctrl_len == 0)
		return 0;

	/* Build MCTP response packet. */
	uint8_t pkt_buf[PKT_BUF];
	const size_t pkt_len = mctp_packet_build_response(
		&pkt.header, state->eid,
		ctrl_body, ctrl_len,
		pkt_buf, sizeof(pkt_buf));
	if (pkt_len == 0)
		return 0;

	/* Wrap in USB frame. */
	return mctp_usb_encode(pkt_buf, pkt_len, out, out_size);
}
