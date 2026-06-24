#include "mctp_usb_frame.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Round-trip and validation tests for the MCTP-USB frame codec (DSP0283). */

static void test_encode_layout(void)
{
	const uint8_t pkt[] = { 0x01, 0x02, 0x03 };
	uint8_t frame[64];
	const size_t n = mctp_usb_encode(pkt, sizeof(pkt), frame, sizeof(frame));

	assert(n == MCTP_USB_HDR_SIZE + sizeof(pkt));
	/* DMTF ID 0xB41A little-endian. */
	assert(frame[0] == 0x1a);
	assert(frame[1] == 0xb4);
	assert(frame[2] == 0x00); /* reserved */
	assert(frame[3] == (uint8_t)n);
	assert(frame[4] == 0x01);
	assert(frame[5] == 0x02);
	assert(frame[6] == 0x03);
	printf("  test_encode_layout: OK\n");
}

static void test_roundtrip(void)
{
	const uint8_t pkt[] = { 0xaa, 0xbb, 0xcc, 0xdd };
	uint8_t frame[64];
	const size_t n = mctp_usb_encode(pkt, sizeof(pkt), frame, sizeof(frame));
	assert(n > 0);

	const mctp_usb_decode_result_t r = mctp_usb_decode(frame, n);
	assert(r.error == MCTP_USB_DECODE_OK);
	assert(r.payload_len == sizeof(pkt));
	assert(memcmp(r.payload, pkt, sizeof(pkt)) == 0);
	printf("  test_roundtrip: OK\n");
}

static void test_decode_too_short(void)
{
	const uint8_t tiny[] = { 0x1a, 0xb4 };
	const mctp_usb_decode_result_t r = mctp_usb_decode(tiny, sizeof(tiny));
	assert(r.error == MCTP_USB_DECODE_TOO_SHORT);
	printf("  test_decode_too_short: OK\n");
}

static void test_decode_bad_dmtf_id(void)
{
	const uint8_t pkt[] = { 0x01 };
	uint8_t frame[64];
	const size_t n = mctp_usb_encode(pkt, sizeof(pkt), frame, sizeof(frame));
	frame[0] = 0x00; /* corrupt DMTF ID */
	const mctp_usb_decode_result_t r = mctp_usb_decode(frame, n);
	assert(r.error == MCTP_USB_DECODE_BAD_DMTF_ID);
	printf("  test_decode_bad_dmtf_id: OK\n");
}

static void test_decode_length_mismatch(void)
{
	const uint8_t pkt[] = { 0x01, 0x02 };
	uint8_t frame[64];
	const size_t n = mctp_usb_encode(pkt, sizeof(pkt), frame, sizeof(frame));
	frame[3] = 0xff; /* length field corrupted */
	const mctp_usb_decode_result_t r = mctp_usb_decode(frame, n);
	assert(r.error == MCTP_USB_DECODE_LENGTH_MISMATCH);
	printf("  test_decode_length_mismatch: OK\n");
}

int main(void)
{
	printf("test_frame:\n");
	test_encode_layout();
	test_roundtrip();
	test_decode_too_short();
	test_decode_bad_dmtf_id();
	test_decode_length_mismatch();
	printf("test_frame: all OK\n");
	return 0;
}
