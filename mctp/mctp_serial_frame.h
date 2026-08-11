#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * DSP0253 MCTP Serial Transport framing encode/decode.
 *
 * Frame format on wire:
 *   [0x7E][len_hi][len_lo][mctp_packet...][fcs_hi][fcs_lo][0x7E]
 *
 * FCS = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over
 *       {0x7E, len_hi, len_lo, mctp_packet...}.
 * FCS stored big-endian (fcs_hi first).
 */

#define MCTP_SERIAL_SYNC         0x7Eu
#define MCTP_SERIAL_OVERHEAD     6u    /* sync(1)+len(2)+fcs(2)+sync(1) */

/* Max MCTP packet per DSP0236: 4-byte transport hdr + 64-byte payload. */
#define MCTP_SERIAL_MAX_PAYLOAD  68u
#define MCTP_SERIAL_MAX_FRAME    (MCTP_SERIAL_MAX_PAYLOAD + MCTP_SERIAL_OVERHEAD)

/*
 * Encode one MCTP packet into a DSP0253 serial frame in out[].
 * out must be at least packet_len + MCTP_SERIAL_OVERHEAD bytes.
 * Returns total bytes written, or 0 on error.
 */
size_t mctp_serial_encode(const uint8_t *packet, size_t packet_len,
                          uint8_t *out, size_t out_size);

/*
 * Decode one DSP0253 frame from buf[len].
 * On success: sets *payload to point into buf[] and *payload_len to its length.
 * Returns 0 on success, -1 on error (bad framing or FCS mismatch).
 */
int mctp_serial_decode(const uint8_t *buf, size_t len,
                       const uint8_t **payload, size_t *payload_len);
