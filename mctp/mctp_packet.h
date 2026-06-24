#pragma once

#include <stddef.h>
#include <stdint.h>

/* MCTP transport header (DSP0236) parse + response builder. */

#define MCTP_VERSION_1   0x01u
#define MCTP_FLAG_SOM    0x80u
#define MCTP_FLAG_EOM    0x40u
#define MCTP_FLAG_TO     0x08u
#define MCTP_TAG_MASK    0x07u
#define MCTP_HDR_SIZE    4u

typedef struct __attribute__((packed)) {
	uint8_t version;
	uint8_t dest;
	uint8_t src;
	uint8_t flags_seq_tag;
} mctp_hdr_t;

typedef enum {
	MCTP_PARSE_OK,
	MCTP_PARSE_TOO_SHORT,
	MCTP_PARSE_NOT_SINGLE_PACKET,
} mctp_parse_error_t;

typedef struct {
	mctp_parse_error_t  error;
	mctp_hdr_t          header;      /* valid when error == MCTP_PARSE_OK */
	const uint8_t      *body;        /* points into caller's buffer */
	size_t              body_len;
} mctp_parse_result_t;

/* Parse inbound MCTP packet from packet[0..len). */
mctp_parse_result_t mctp_packet_parse(const uint8_t *packet, size_t len);

/*
 * Build response packet into out[].
 * Swaps dest/src, clears Tag Owner, keeps request tag, sets SOM+EOM.
 * Returns bytes written, or 0 on error.
 */
size_t mctp_packet_build_response(const mctp_hdr_t *req_hdr, uint8_t our_eid,
                                  const uint8_t *body, size_t body_len,
                                  uint8_t *out, size_t out_size);
