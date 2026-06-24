#include "mctp_control.h"

#include <string.h>

/* Minimum bytes needed to identify a control request: type + ctrl_hdr + cmd. */
#define HDR_LEN 3u

/* Get Endpoint ID response: simple endpoint, no medium-specific info. */
#define ENDPOINT_TYPE_SIMPLE  0x00u
#define MEDIUM_SPECIFIC_NONE  0x00u

/* Set Endpoint ID response: assignment accepted, no EID pool. */
#define SET_EID_STATUS_ACCEPTED 0x00u
#define EID_POOL_SIZE_NONE      0x00u

/* Set Endpoint ID request data: [operation, eid]. */
#define SET_EID_DATA_LEN   2u
#define SET_EID_VALUE_IDX  1u

/* Get Message Type Support: one supported type (MCTP control). */
#define SUPPORTED_TYPE_COUNT 0x01u

const uint8_t mctp_ctrl_default_uuid[MCTP_CTRL_UUID_LEN] = {
	0xd6, 0x1a, 0x4c, 0x70, 0x2b, 0x55, 0x4e, 0x9a,
	0x8f, 0x13, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
};

/* Write the standard 4-byte response preamble; return bytes written. */
static size_t begin_response(const uint8_t *request, uint8_t cc,
                             uint8_t *out, size_t out_size)
{
	if (out_size < 4u)
		return 0;
	out[0] = MCTP_CTRL_MSG_TYPE;
	out[1] = request[1] & (uint8_t)~MCTP_CTRL_RQ_BIT;  /* clear Rq, keep instance id */
	out[2] = request[2];                                 /* echo command */
	out[3] = cc;
	return 4u;
}

const char *mctp_ctrl_command_name(uint8_t code)
{
	switch (code) {
	case MCTP_CTRL_CMD_SET_EID:      return "Set Endpoint ID";
	case MCTP_CTRL_CMD_GET_EID:      return "Get Endpoint ID";
	case MCTP_CTRL_CMD_GET_UUID:     return "Get Endpoint UUID";
	case MCTP_CTRL_CMD_GET_MSG_TYPE: return "Get Message Type Support";
	default:                          return "unknown/unsupported";
	}
}

size_t mctp_ctrl_handle(mctp_endpoint_state_t *state,
                        const uint8_t *request, size_t request_len,
                        uint8_t *out, size_t out_size)
{
	if (request_len < HDR_LEN)
		return 0;
	if ((request[0] & MCTP_CTRL_MSG_TYPE_MASK) != MCTP_CTRL_MSG_TYPE)
		return 0;
	if (!(request[1] & MCTP_CTRL_RQ_BIT))
		return 0;  /* a response, not a request */

	const uint8_t cmd  = request[2];
	const uint8_t *data     = request + HDR_LEN;
	const size_t   data_len = request_len - HDR_LEN;

	size_t n;

	switch (cmd) {
	case MCTP_CTRL_CMD_GET_EID:
		n = begin_response(request, MCTP_CTRL_CC_SUCCESS, out, out_size);
		if (!n || out_size - n < 3u) return 0;
		out[n++] = state->eid;
		out[n++] = ENDPOINT_TYPE_SIMPLE;
		out[n++] = MEDIUM_SPECIFIC_NONE;
		return n;

	case MCTP_CTRL_CMD_SET_EID:
		if (data_len < SET_EID_DATA_LEN)
			return begin_response(request, MCTP_CTRL_CC_ERR_INVALID_LEN,
			                      out, out_size);
		state->eid = data[SET_EID_VALUE_IDX];
		n = begin_response(request, MCTP_CTRL_CC_SUCCESS, out, out_size);
		if (!n || out_size - n < 3u) return 0;
		out[n++] = SET_EID_STATUS_ACCEPTED;
		out[n++] = state->eid;
		out[n++] = EID_POOL_SIZE_NONE;
		return n;

	case MCTP_CTRL_CMD_GET_UUID:
		n = begin_response(request, MCTP_CTRL_CC_SUCCESS, out, out_size);
		if (!n || out_size - n < MCTP_CTRL_UUID_LEN) return 0;
		memcpy(out + n, state->uuid, MCTP_CTRL_UUID_LEN);
		return n + MCTP_CTRL_UUID_LEN;

	case MCTP_CTRL_CMD_GET_MSG_TYPE:
		n = begin_response(request, MCTP_CTRL_CC_SUCCESS, out, out_size);
		if (!n || out_size - n < 2u) return 0;
		out[n++] = SUPPORTED_TYPE_COUNT;
		out[n++] = MCTP_CTRL_MSG_TYPE;
		return n;

	default:
		return begin_response(request, MCTP_CTRL_CC_ERR_UNSUPPORTED,
		                      out, out_size);
	}
}
