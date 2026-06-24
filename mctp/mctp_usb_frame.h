#pragma once

#include <stddef.h>
#include <stdint.h>

/* DSP0283 4-byte USB header encode/decode. */

#define MCTP_USB_DMTF_ID  0xB41Au
#define MCTP_USB_HDR_SIZE 4u

typedef enum {
	MCTP_USB_DECODE_OK,
	MCTP_USB_DECODE_TOO_SHORT,
	MCTP_USB_DECODE_BAD_DMTF_ID,
	MCTP_USB_DECODE_LENGTH_MISMATCH,
} mctp_usb_decode_error_t;

typedef struct {
	mctp_usb_decode_error_t error;
	const uint8_t          *payload;     /* points into caller's buffer */
	size_t                  payload_len;
} mctp_usb_decode_result_t;

/*
 * Encode: write USB header + mctp_packet into out[].
 * out must be at least MCTP_USB_HDR_SIZE + packet_len bytes.
 * Returns total bytes written, or 0 on error.
 */
size_t mctp_usb_encode(const uint8_t *packet, size_t packet_len,
                       uint8_t *out, size_t out_size);

/* Decode: validate header and return pointer into frame[]. */
mctp_usb_decode_result_t mctp_usb_decode(const uint8_t *frame, size_t frame_len);
