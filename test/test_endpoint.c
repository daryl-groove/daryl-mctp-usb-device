#include "mctp_endpoint.h"
#include "mctp_control.h"
#include "mctp_packet.h"
#include "mctp_usb_frame.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * End-to-end test: full MCTP-over-USB request frame in, response frame out.
 * Exercises frame + packet + control together with no UDC.
 */

#define BUS_OWNER_EID  0x08u
#define REQ_CTRL_HDR   (MCTP_CTRL_RQ_BIT | 0x03u)
#define REQ_FLAGS      (MCTP_FLAG_SOM | MCTP_FLAG_EOM | MCTP_FLAG_TO | 0x01u)

/* Build a full MCTP-over-USB request frame from a control message body. */
static size_t make_request_frame(uint8_t dest,
                                 const uint8_t *ctrl, size_t ctrl_len,
                                 uint8_t *out, size_t out_size)
{
	/* MCTP packet: hdr + ctrl body */
	uint8_t pkt[256];
	pkt[0] = MCTP_VERSION_1;
	pkt[1] = dest;
	pkt[2] = BUS_OWNER_EID;
	pkt[3] = REQ_FLAGS;
	memcpy(pkt + 4, ctrl, ctrl_len);
	const size_t pkt_len = 4u + ctrl_len;
	return mctp_usb_encode(pkt, pkt_len, out, out_size);
}

/* Decode a response frame down to its MCTP packet. */
static mctp_parse_result_t unwrap(const uint8_t *frame, size_t frame_len)
{
	const mctp_usb_decode_result_t f = mctp_usb_decode(frame, frame_len);
	if (f.error != MCTP_USB_DECODE_OK) {
		mctp_parse_result_t bad = { MCTP_PARSE_TOO_SHORT, {0}, NULL, 0 };
		return bad;
	}
	return mctp_packet_parse(f.payload, f.payload_len);
}

static void test_set_then_get_eid_end_to_end(void)
{
	mctp_endpoint_state_t state;
	mctp_endpoint_state_init(&state);

	uint8_t req_frame[256], resp_frame[256];

	/* Bus owner assigns EID 9 (we still answer at the null EID). */
	const uint8_t set_ctrl[] = { 0x00, REQ_CTRL_HDR, 0x01, 0x00, 0x09 };
	const size_t req_len = make_request_frame(0x00, set_ctrl, sizeof(set_ctrl),
	                                          req_frame, sizeof(req_frame));
	assert(req_len > 0);

	const size_t resp_len = mctp_endpoint_process(&state,
	                                               req_frame, req_len,
	                                               resp_frame, sizeof(resp_frame));
	assert(resp_len > 0);
	assert(state.eid == 0x09);

	const mctp_parse_result_t sr = unwrap(resp_frame, resp_len);
	assert(sr.error == MCTP_PARSE_OK);
	assert(sr.header.dest == BUS_OWNER_EID);
	assert(sr.header.src  == 0x09);
	assert(sr.body_len == 7u);
	assert(sr.body[3] == 0x00); /* completion success */
	assert(sr.body[5] == 0x09); /* EID assigned */

	/* Now Get EID addressed to EID 9. */
	const uint8_t get_ctrl[] = { 0x00, REQ_CTRL_HDR, 0x02 };
	const size_t req2_len = make_request_frame(0x09, get_ctrl, sizeof(get_ctrl),
	                                            req_frame, sizeof(req_frame));
	assert(req2_len > 0);

	const size_t resp2_len = mctp_endpoint_process(&state,
	                                                req_frame, req2_len,
	                                                resp_frame, sizeof(resp_frame));
	const mctp_parse_result_t gr = unwrap(resp_frame, resp2_len);
	assert(gr.error == MCTP_PARSE_OK);
	assert(gr.header.src == 0x09);
	assert(gr.body_len == 7u);
	assert(gr.body[3] == 0x00); /* success */
	assert(gr.body[4] == 0x09); /* reported EID */
	printf("  test_set_then_get_eid_end_to_end: OK\n");
}

static void test_non_mctp_frame_ignored(void)
{
	mctp_endpoint_state_t state;
	mctp_endpoint_state_init(&state);
	/* Valid-looking bytes but wrong DMTF ID → decode fails → no response. */
	const uint8_t junk[] = { 0x00, 0x00, 0x00, 0x04, 0x01, 0x02, 0x03 };
	uint8_t resp[256];
	assert(mctp_endpoint_process(&state, junk, sizeof(junk),
	                             resp, sizeof(resp)) == 0);
	printf("  test_non_mctp_frame_ignored: OK\n");
}

int main(void)
{
	printf("test_endpoint:\n");
	test_set_then_get_eid_end_to_end();
	test_non_mctp_frame_ignored();
	printf("test_endpoint: all OK\n");
	return 0;
}
