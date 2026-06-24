#pragma once

#include <stddef.h>
#include <stdint.h>

/* MCTP control-message responder (DSP0236). */

#define MCTP_CTRL_MSG_TYPE      0x00u
#define MCTP_CTRL_MSG_TYPE_MASK 0x7fu
#define MCTP_CTRL_RQ_BIT        0x80u

#define MCTP_CTRL_CMD_SET_EID          0x01u
#define MCTP_CTRL_CMD_GET_EID          0x02u
#define MCTP_CTRL_CMD_GET_UUID         0x03u
#define MCTP_CTRL_CMD_GET_MSG_TYPE     0x05u

#define MCTP_CTRL_CC_SUCCESS            0x00u
#define MCTP_CTRL_CC_ERR_INVALID_LEN    0x03u
#define MCTP_CTRL_CC_ERR_UNSUPPORTED    0x05u

#define MCTP_CTRL_EID_UNASSIGNED 0x00u
#define MCTP_CTRL_UUID_LEN       16u

/* Placeholder UUID; a real device would burn in a per-unit value. */
extern const uint8_t mctp_ctrl_default_uuid[MCTP_CTRL_UUID_LEN];

typedef struct {
	uint8_t eid;
	uint8_t uuid[MCTP_CTRL_UUID_LEN];
} mctp_endpoint_state_t;

/* Zero-initialise then fill uuid with the default; call before first use. */
static inline void mctp_endpoint_state_init(mctp_endpoint_state_t *s)
{
	uint8_t i;
	s->eid = MCTP_CTRL_EID_UNASSIGNED;
	for (i = 0; i < MCTP_CTRL_UUID_LEN; i++)
		s->uuid[i] = mctp_ctrl_default_uuid[i];
}

/* Human-readable command name for logging. */
const char *mctp_ctrl_command_name(uint8_t code);

/*
 * Handle one control message body (starting at the message-type byte).
 * Writes the response body into out[].
 * Returns bytes written, or 0 if this message should not be answered.
 */
size_t mctp_ctrl_handle(mctp_endpoint_state_t *state,
                        const uint8_t *request, size_t request_len,
                        uint8_t *out, size_t out_size);
