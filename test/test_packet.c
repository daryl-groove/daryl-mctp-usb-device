#include "mctp_packet.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Tests for the MCTP transport/packet layer (DSP0236). */

/* Request: dest 9, src 0 (bus owner), single packet, Tag Owner set, tag 1. */
#define REQ_FLAGS (MCTP_FLAG_SOM | MCTP_FLAG_EOM | MCTP_FLAG_TO | 0x01u)

static void test_parse_single_packet(void)
{
	const uint8_t pkt[] = { MCTP_VERSION_1, 0x09, 0x00, REQ_FLAGS, 0xde, 0xad };
	const mctp_parse_result_t r = mctp_packet_parse(pkt, sizeof(pkt));

	assert(r.error == MCTP_PARSE_OK);
	assert(r.header.dest == 0x09);
	assert(r.header.src  == 0x00);
	assert(r.body_len == 2u);
	assert(r.body[0] == 0xde);
	assert(r.body[1] == 0xad);
	printf("  test_parse_single_packet: OK\n");
}

static void test_parse_too_short(void)
{
	const uint8_t pkt[] = { 0x01, 0x09, 0x00 };
	assert(mctp_packet_parse(pkt, sizeof(pkt)).error == MCTP_PARSE_TOO_SHORT);
	printf("  test_parse_too_short: OK\n");
}

static void test_parse_multi_packet(void)
{
	/* EOM cleared → fragment */
	const uint8_t flags = MCTP_FLAG_SOM | MCTP_FLAG_TO;
	const uint8_t pkt[] = { 0x01, 0x09, 0x00, flags, 0x00 };
	assert(mctp_packet_parse(pkt, sizeof(pkt)).error ==
	       MCTP_PARSE_NOT_SINGLE_PACKET);
	printf("  test_parse_multi_packet: OK\n");
}

static void test_build_response(void)
{
	const mctp_hdr_t req = { MCTP_VERSION_1, 0x09, 0x00, REQ_FLAGS };
	const uint8_t body[] = { 0xaa, 0xbb };
	uint8_t out[64];
	const size_t n = mctp_packet_build_response(&req, 0x09,
	                                             body, sizeof(body),
	                                             out, sizeof(out));
	const uint8_t want_flags = MCTP_FLAG_SOM | MCTP_FLAG_EOM | 0x01u;
	assert(n == 4u + sizeof(body));
	assert(out[0] == MCTP_VERSION_1);
	assert(out[1] == 0x00); /* dest := request src */
	assert(out[2] == 0x09); /* src := our EID */
	assert(out[3] == want_flags);
	assert(out[4] == 0xaa);
	assert(out[5] == 0xbb);
	printf("  test_build_response: OK\n");
}

static void test_roundtrip(void)
{
	const mctp_hdr_t req = { MCTP_VERSION_1, 0x09, 0x00, REQ_FLAGS };
	const uint8_t body[] = { 0x01 };
	uint8_t out[64];
	const size_t n = mctp_packet_build_response(&req, 0x09,
	                                             body, sizeof(body),
	                                             out, sizeof(out));
	assert(n > 0);
	const mctp_parse_result_t r = mctp_packet_parse(out, n);
	assert(r.error == MCTP_PARSE_OK);
	assert(r.header.dest == 0x00);
	assert(r.header.src  == 0x09);
	printf("  test_roundtrip: OK\n");
}

int main(void)
{
	printf("test_packet:\n");
	test_parse_single_packet();
	test_parse_too_short();
	test_parse_multi_packet();
	test_build_response();
	test_roundtrip();
	printf("test_packet: all OK\n");
	return 0;
}
