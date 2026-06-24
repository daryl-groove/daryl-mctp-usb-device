#include "mctp_usb_frame.hpp"

#include <cstring>

namespace mctpusb {

std::vector<std::uint8_t> encode(std::span<const std::uint8_t> mctpPacket)
{
	const std::size_t total = sizeof(UsbHeader) + mctpPacket.size();

	UsbHeader header{};
	header.dmtfId = DmtfId;
	header.reserved = 0;
	header.length = static_cast<std::uint8_t>(total); // see precondition

	std::vector<std::uint8_t> frame(total);
	std::memcpy(frame.data(), &header, sizeof(header));
	std::memcpy(frame.data() + sizeof(header), mctpPacket.data(),
		    mctpPacket.size());
	return frame;
}

DecodeResult decode(std::span<const std::uint8_t> frame)
{
	if (frame.size() < sizeof(UsbHeader))
		return { DecodeError::TooShort, {} };

	UsbHeader header{};
	std::memcpy(&header, frame.data(), sizeof(header));

	if (header.dmtfId != DmtfId)
		return { DecodeError::BadDmtfId, {} };

	// Mirrors libmctp usb.c: byte_count must match the bytes received.
	if (header.length != frame.size())
		return { DecodeError::LengthMismatch, {} };

	return { DecodeError::Ok, frame.subspan(sizeof(UsbHeader)) };
}

} // namespace mctpusb
