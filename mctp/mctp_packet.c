#include "mctp_packet.h"

#include <string.h>

mctp_parse_result_t mctp_packet_parse(const uint8_t *packet, size_t len)
{
	mctp_parse_result_t r = { MCTP_PARSE_TOO_SHORT, {0}, NULL, 0 };

	if (len < MCTP_HDR_SIZE)
		return r;

	mctp_hdr_t hdr;
	memcpy(&hdr, packet, sizeof(hdr));

	const int som = (hdr.flags_seq_tag & MCTP_FLAG_SOM) != 0;
	const int eom = (hdr.flags_seq_tag & MCTP_FLAG_EOM) != 0;
	if (!som || !eom) {
		r.error = MCTP_PARSE_NOT_SINGLE_PACKET;
		return r;
	}

	r.error    = MCTP_PARSE_OK;
	r.header   = hdr;
	r.body     = packet + MCTP_HDR_SIZE;
	r.body_len = len - MCTP_HDR_SIZE;
	return r;
}

size_t mctp_packet_build_response(const mctp_hdr_t *req_hdr, uint8_t our_eid,
                                  const uint8_t *body, size_t body_len,
                                  uint8_t *out, size_t out_size)
{
	const size_t total = MCTP_HDR_SIZE + body_len;
	if (total > out_size)
		return 0;

	mctp_hdr_t hdr;
	hdr.version      = req_hdr->version;
	hdr.dest         = req_hdr->src;
	hdr.src          = our_eid;
	/* Single packet, Tag Owner cleared, request tag preserved, sequence 0. */
	hdr.flags_seq_tag = MCTP_FLAG_SOM | MCTP_FLAG_EOM |
	                    (req_hdr->flags_seq_tag & MCTP_TAG_MASK);

	memcpy(out, &hdr, sizeof(hdr));
	memcpy(out + sizeof(hdr), body, body_len);
	return total;
}
