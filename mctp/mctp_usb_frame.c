#include "mctp_usb_frame.h"

#include <string.h>

/* Wire layout of the 4-byte DSP0283 USB header. */
typedef struct __attribute__((packed)) {
	uint16_t dmtf_id;  /* 0xB41A LE → wire bytes 0x1A 0xB4 */
	uint8_t  reserved;
	uint8_t  length;   /* total frame length including this header */
} usb_hdr_t;

size_t mctp_usb_encode(const uint8_t *packet, size_t packet_len,
                       uint8_t *out, size_t out_size)
{
	const size_t total = MCTP_USB_HDR_SIZE + packet_len;
	if (total > out_size || total > 0xffu)
		return 0;

	usb_hdr_t hdr;
	hdr.dmtf_id  = MCTP_USB_DMTF_ID;
	hdr.reserved = 0;
	hdr.length   = (uint8_t)total;

	memcpy(out, &hdr, sizeof(hdr));
	memcpy(out + sizeof(hdr), packet, packet_len);
	return total;
}

mctp_usb_decode_result_t mctp_usb_decode(const uint8_t *frame, size_t frame_len)
{
	mctp_usb_decode_result_t r = { MCTP_USB_DECODE_TOO_SHORT, NULL, 0 };

	if (frame_len < MCTP_USB_HDR_SIZE)
		return r;

	usb_hdr_t hdr;
	memcpy(&hdr, frame, sizeof(hdr));

	if (hdr.dmtf_id != MCTP_USB_DMTF_ID) {
		r.error = MCTP_USB_DECODE_BAD_DMTF_ID;
		return r;
	}
	if (hdr.length != (uint8_t)frame_len) {
		r.error = MCTP_USB_DECODE_LENGTH_MISMATCH;
		return r;
	}

	r.error       = MCTP_USB_DECODE_OK;
	r.payload     = frame + MCTP_USB_HDR_SIZE;
	r.payload_len = frame_len - MCTP_USB_HDR_SIZE;
	return r;
}
