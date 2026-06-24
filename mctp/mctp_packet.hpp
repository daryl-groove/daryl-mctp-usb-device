// MCTP transport/packet header (DSP0236), device side.
//
// Parses an inbound MCTP packet down to its message body and builds the response
// packet (swap dest/src, clear Tag Owner, keep the request's message tag). Only
// single-packet messages are handled — MCTP control messages fit in one packet,
// so reassembly is deliberately out of scope. Pure, unit-tested off-target.
//
// Header layout mirrors libmctp's struct mctp_hdr / MCTP_HDR_* flags.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace mctppkt {

inline constexpr std::uint8_t Version1 = 0x01;

// flags_seq_tag bit fields.
inline constexpr std::uint8_t FlagSom = 0x80;
inline constexpr std::uint8_t FlagEom = 0x40;
inline constexpr std::uint8_t FlagTagOwner = 0x08;
inline constexpr std::uint8_t TagMask = 0x07;

struct __attribute__((packed)) Header {
	std::uint8_t version;
	std::uint8_t dest;
	std::uint8_t src;
	std::uint8_t flagsSeqTag;
};

enum class ParseError {
	Ok,
	TooShort,	 // fewer bytes than the header
	NotSinglePacket, // SOM and EOM not both set (reassembly unsupported)
};

struct ParseResult {
	ParseError error;
	Header header;			   // valid only when error == Ok
	std::span<const std::uint8_t> body; // message body, valid when Ok
};

ParseResult parse(std::span<const std::uint8_t> packet);

// Build the response packet for a parsed request: dest/src swapped, Tag Owner
// cleared, the request's message tag preserved, a single packet (SOM+EOM,
// sequence 0). ourEid is used as the source EID.
std::vector<std::uint8_t> build_response(const Header &requestHeader,
					 std::uint8_t ourEid,
					 std::span<const std::uint8_t> body);

} // namespace mctppkt
