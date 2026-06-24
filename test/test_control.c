#include "mctp_control.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Tests for the MCTP control responder (DSP0236).
 *
 * Request body layout: [msgType=0x00][Rq|InstanceId][command][data...]
 * Response layout:     [msgType=0x00][InstanceId   ][command][cc][data...]
 */

/* Rq set, instance id 3 → echoed back as 0x03 (Rq cleared). */
#define REQ_HDR  (MCTP_CTRL_RQ_BIT | 0x03u)
#define RESP_HDR (0x03u)

static mctp_endpoint_state_t make_state(void)
{
	mctp_endpoint_state_t s;
	mctp_endpoint_state_init(&s);
	return s;
}

static void test_get_eid(void)
{
	mctp_endpoint_state_t s = make_state();
	uint8_t out[32];
	const uint8_t req[] = { 0x00, REQ_HDR, 0x02 };
	const size_t n = mctp_ctrl_handle(&s, req, sizeof(req), out, sizeof(out));
	const uint8_t want[] = { 0x00, RESP_HDR, 0x02, 0x00, 0x00, 0x00, 0x00 };
	assert(n == sizeof(want));
	assert(memcmp(out, want, n) == 0);
	printf("  test_get_eid: OK\n");
}

static void test_set_then_get(void)
{
	mctp_endpoint_state_t s = make_state();
	uint8_t out[32];
	const uint8_t set_req[] = { 0x00, REQ_HDR, 0x01, 0x00, 0x09 };
	const size_t sn = mctp_ctrl_handle(&s, set_req, sizeof(set_req),
	                                   out, sizeof(out));
	const uint8_t want_set[] = { 0x00, RESP_HDR, 0x01, 0x00, 0x00, 0x09, 0x00 };
	assert(sn == sizeof(want_set));
	assert(memcmp(out, want_set, sn) == 0);
	assert(s.eid == 0x09);

	const uint8_t get_req[] = { 0x00, REQ_HDR, 0x02 };
	const size_t gn = mctp_ctrl_handle(&s, get_req, sizeof(get_req),
	                                   out, sizeof(out));
	assert(gn == 7u);
	assert(out[4] == 0x09); /* reported EID */
	printf("  test_set_then_get: OK\n");
}

static void test_set_too_short(void)
{
	mctp_endpoint_state_t s = make_state();
	uint8_t out[32];
	const uint8_t req[] = { 0x00, REQ_HDR, 0x01 }; /* missing data */
	const size_t n = mctp_ctrl_handle(&s, req, sizeof(req), out, sizeof(out));
	const uint8_t want[] = { 0x00, RESP_HDR, 0x01, MCTP_CTRL_CC_ERR_INVALID_LEN };
	assert(n == sizeof(want));
	assert(memcmp(out, want, n) == 0);
	printf("  test_set_too_short: OK\n");
}

static void test_get_uuid(void)
{
	mctp_endpoint_state_t s = make_state();
	uint8_t out[32];
	const uint8_t req[] = { 0x00, REQ_HDR, 0x03 };
	const size_t n = mctp_ctrl_handle(&s, req, sizeof(req), out, sizeof(out));
	assert(n == 4u + MCTP_CTRL_UUID_LEN);
	assert(out[0] == 0x00);
	assert(out[1] == RESP_HDR);
	assert(out[2] == 0x03);
	assert(out[3] == 0x00); /* success */
	assert(memcmp(out + 4, mctp_ctrl_default_uuid, MCTP_CTRL_UUID_LEN) == 0);
	printf("  test_get_uuid: OK\n");
}

static void test_get_msg_type(void)
{
	mctp_endpoint_state_t s = make_state();
	uint8_t out[32];
	const uint8_t req[] = { 0x00, REQ_HDR, 0x05 };
	const size_t n = mctp_ctrl_handle(&s, req, sizeof(req), out, sizeof(out));
	const uint8_t want[] = { 0x00, RESP_HDR, 0x05, 0x00, 0x01, 0x00 };
	assert(n == sizeof(want));
	assert(memcmp(out, want, n) == 0);
	printf("  test_get_msg_type: OK\n");
}

static void test_unsupported_command(void)
{
	mctp_endpoint_state_t s = make_state();
	uint8_t out[32];
	const uint8_t req[] = { 0x00, REQ_HDR, 0x04 }; /* Get MCTP Version */
	const size_t n = mctp_ctrl_handle(&s, req, sizeof(req), out, sizeof(out));
	const uint8_t want[] = { 0x00, RESP_HDR, 0x04, MCTP_CTRL_CC_ERR_UNSUPPORTED };
	assert(n == sizeof(want));
	assert(memcmp(out, want, n) == 0);
	printf("  test_unsupported_command: OK\n");
}

static void test_instance_id_echoed(void)
{
	mctp_endpoint_state_t s = make_state();
	uint8_t out[32];
	const uint8_t hdr = MCTP_CTRL_RQ_BIT | 0x1fu; /* max instance id */
	const uint8_t req[] = { 0x00, hdr, 0x02 };
	const size_t n = mctp_ctrl_handle(&s, req, sizeof(req), out, sizeof(out));
	assert(n >= 2u);
	assert(out[1] == 0x1fu); /* Rq cleared, instance id preserved */
	printf("  test_instance_id_echoed: OK\n");
}

static void test_dropped_inputs(void)
{
	mctp_endpoint_state_t s = make_state();
	uint8_t out[32];

	/* Not a control message (PLDM type 0x01). */
	const uint8_t pldm[] = { 0x01, REQ_HDR, 0x02 };
	assert(mctp_ctrl_handle(&s, pldm, sizeof(pldm), out, sizeof(out)) == 0);

	/* A response (Rq clear), not a request. */
	const uint8_t resp[] = { 0x00, 0x03, 0x02 };
	assert(mctp_ctrl_handle(&s, resp, sizeof(resp), out, sizeof(out)) == 0);

	/* Too short to hold a header. */
	const uint8_t short_req[] = { 0x00, REQ_HDR };
	assert(mctp_ctrl_handle(&s, short_req, sizeof(short_req),
	                        out, sizeof(out)) == 0);

	printf("  test_dropped_inputs: OK\n");
}

int main(void)
{
	printf("test_control:\n");
	test_get_eid();
	test_set_then_get();
	test_set_too_short();
	test_get_uuid();
	test_get_msg_type();
	test_unsupported_command();
	test_instance_id_echoed();
	test_dropped_inputs();
	printf("test_control: all OK\n");
	return 0;
}
