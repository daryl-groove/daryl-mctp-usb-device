// MCTP-over-USB framing (DSP0283), device side.
//
// Each USB transfer carries a 4-byte header followed by one MCTP packet. This
// mirrors NVIDIA libmctp's usb.c (struct mctp_usb_header_*) byte-for-byte so a
// libmctp host on the same machine interoperates. The codec is role-neutral and
// pure (no I/O), so it is unit-tested off-target.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace mctpusb {

// libmctp stores this as a native uint16_t == 0xB41A; on a little-endian host
// that is the wire sequence 0x1A 0xB4 (DMTF ID 0x1AB4). Same-arch host/device
// is the supported case (single guest, or one BMC), matching libmctp's own
// assumption.
inline constexpr std::uint16_t DmtfId = 0xB41A;

struct __attribute__((packed)) UsbHeader {
	std::uint16_t dmtfId;
	std::uint8_t reserved;
	std::uint8_t length; // total frame length, including this header
};

enum class DecodeError {
	Ok,
	TooShort,	// fewer bytes than the header
	BadDmtfId,	// not an MCTP-over-USB frame
	LengthMismatch, // header length disagrees with received size
};

struct DecodeResult {
	DecodeError error;
	std::span<const std::uint8_t> payload; // valid only when error == Ok
};

// Prepend the MCTP-USB header to one MCTP packet.
// Precondition: header + packet fits in the single-byte length field (<= 255).
std::vector<std::uint8_t> encode(std::span<const std::uint8_t> mctpPacket);

// Validate a received frame and return the MCTP packet payload.
DecodeResult decode(std::span<const std::uint8_t> frame);

} // namespace mctpusb
