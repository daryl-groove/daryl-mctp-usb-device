#include "mctp_packet.hpp"

#include <cstring>

namespace mctppkt {

ParseResult parse(std::span<const std::uint8_t> packet)
{
	if (packet.size() < sizeof(Header))
		return { ParseError::TooShort, {}, {} };

	Header header{};
	std::memcpy(&header, packet.data(), sizeof(header));

	const bool singlePacket = (header.flagsSeqTag & FlagSom) &&
				  (header.flagsSeqTag & FlagEom);
	if (!singlePacket)
		return { ParseError::NotSinglePacket, {}, {} };

	return { ParseError::Ok, header, packet.subspan(sizeof(Header)) };
}

std::vector<std::uint8_t> build_response(const Header &requestHeader,
					 std::uint8_t ourEid,
					 std::span<const std::uint8_t> body)
{
	Header header{};
	header.version = requestHeader.version;
	header.dest = requestHeader.src;
	header.src = ourEid;
	// Single packet, Tag Owner cleared (we are the responder), original
	// message tag preserved, sequence 0.
	const std::uint8_t tag = requestHeader.flagsSeqTag & TagMask;
	header.flagsSeqTag = FlagSom | FlagEom | tag;

	std::vector<std::uint8_t> packet(sizeof(Header) + body.size());
	std::memcpy(packet.data(), &header, sizeof(header));
	std::memcpy(packet.data() + sizeof(header), body.data(), body.size());
	return packet;
}

} // namespace mctppkt
