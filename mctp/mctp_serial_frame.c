#include "mctp_serial_frame.h"

#include <string.h>

/* CRC-16/CCITT-FALSE: poly=0x1021, init=0xFFFF, no bit reflection. */
static uint16_t crc16_update(uint16_t crc, const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)buf[i] << 8;
		for (int j = 0; j < 8; j++)
			crc = (crc & 0x8000u)
			      ? (uint16_t)((crc << 1) ^ 0x1021u)
			      : (uint16_t)(crc << 1);
	}
	return crc;
}

size_t mctp_serial_encode(const uint8_t *packet, size_t packet_len,
                          uint8_t *out, size_t out_size)
{
	if (packet_len > 0xFFFFu)
		return 0;
	const size_t total = packet_len + MCTP_SERIAL_OVERHEAD;
	if (total > out_size)
		return 0;

	out[0] = MCTP_SERIAL_SYNC;
	out[1] = (uint8_t)(packet_len >> 8);
	out[2] = (uint8_t)(packet_len & 0xFFu);
	memcpy(out + 3, packet, packet_len);

	/* FCS covers {sync, len_hi, len_lo, payload}. */
	const uint16_t fcs = crc16_update(0xFFFFu, out, 3 + packet_len);
	out[3 + packet_len]     = (uint8_t)(fcs >> 8);
	out[3 + packet_len + 1] = (uint8_t)(fcs & 0xFFu);
	out[3 + packet_len + 2] = MCTP_SERIAL_SYNC;

	return total;
}

int mctp_serial_decode(const uint8_t *buf, size_t len,
                       const uint8_t **payload, size_t *payload_len)
{
	if (len < MCTP_SERIAL_OVERHEAD)
		return -1;
	if (buf[0] != MCTP_SERIAL_SYNC || buf[len - 1] != MCTP_SERIAL_SYNC)
		return -1;

	const size_t plen = ((size_t)buf[1] << 8) | buf[2];
	if (plen + MCTP_SERIAL_OVERHEAD != len)
		return -1;

	const uint16_t rx_fcs  = ((uint16_t)buf[3 + plen] << 8)
	                         | buf[3 + plen + 1];
	const uint16_t exp_fcs = crc16_update(0xFFFFu, buf, 3 + plen);
	if (rx_fcs != exp_fcs)
		return -1;

	*payload     = buf + 3;
	*payload_len = plen;
	return 0;
}
