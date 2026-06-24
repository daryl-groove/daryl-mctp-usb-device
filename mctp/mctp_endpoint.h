#pragma once

#include "mctp_control.h"

#include <stddef.h>
#include <stdint.h>

/* Orchestrator: USB frame -> MCTP packet -> control handler -> response frame. */

/*
 * Process one inbound USB frame (buf[0..len)).
 * Writes the complete USB response frame into out[].
 * Returns bytes written, or 0 if no response should be sent.
 */
size_t mctp_endpoint_process(mctp_endpoint_state_t *state,
                             const uint8_t *buf, size_t len,
                             uint8_t *out, size_t out_size);
