#include "mctp_endpoint.hpp"

#include "mctp_packet.hpp"
#include "mctp_usb_frame.hpp"

namespace mctpep {

std::vector<std::uint8_t> process(mctpctrl::EndpointState &state,
				  std::span<const std::uint8_t> usbFrame)
{
	const mctpusb::DecodeResult frame = mctpusb::decode(usbFrame);
	if (frame.error != mctpusb::DecodeError::Ok)
		return {};

	const mctppkt::ParseResult packet = mctppkt::parse(frame.payload);
	if (packet.error != mctppkt::ParseError::Ok)
		return {};

	const std::vector<std::uint8_t> reply =
		mctpctrl::handle(state, packet.body);
	if (reply.empty())
		return {};

	// Source the response with our current EID: a Set Endpoint ID request
	// will have just changed it.
	const std::vector<std::uint8_t> responsePacket =
		mctppkt::build_response(packet.header, state.eid, reply);
	return mctpusb::encode(responsePacket);
}

} // namespace mctpep
