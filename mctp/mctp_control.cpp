#include "mctp_control.hpp"

namespace mctpctrl {
namespace {

// Message type + control header + command, before any command data.
constexpr std::size_t HdrLen = 3;

// Get Endpoint ID response fields (DSP0236): a simple endpoint with a
// dynamically assigned EID and no medium-specific information.
constexpr std::uint8_t EndpointTypeSimple = 0x00;
constexpr std::uint8_t MediumSpecificNone = 0x00;

// Set Endpoint ID response fields: assignment accepted, device has no EID pool.
constexpr std::uint8_t SetEidStatusAccepted = 0x00;
constexpr std::uint8_t EidPoolSizeNone = 0x00;

// Set Endpoint ID request data: [operation, eid].
constexpr std::size_t SetEidDataLen = 2;
constexpr std::size_t SetEidValueIndex = 1;

// Get Message Type Support response: one supported type, MCTP control.
constexpr std::uint8_t SupportedTypeCount = 0x01;

// Start a response with the standard four-byte preamble: message type, the
// request's header with the Rq bit cleared (D bit and instance id echoed), the
// echoed command code, and the completion code.
std::vector<std::uint8_t> begin_response(std::span<const std::uint8_t> request,
					 CompletionCode code)
{
	std::vector<std::uint8_t> response;
	response.push_back(MsgTypeControl);
	response.push_back(static_cast<std::uint8_t>(request[1] & ~RqBit));
	response.push_back(request[2]);
	response.push_back(static_cast<std::uint8_t>(code));
	return response;
}

} // namespace

const char *command_name(std::uint8_t code)
{
	switch (static_cast<Command>(code)) {
	case Command::SetEndpointId:
		return "Set Endpoint ID";
	case Command::GetEndpointId:
		return "Get Endpoint ID";
	case Command::GetEndpointUuid:
		return "Get Endpoint UUID";
	case Command::GetMessageTypeSupport:
		return "Get Message Type Support";
	}
	return "unknown/unsupported";
}

std::vector<std::uint8_t> handle(EndpointState &state,
				 std::span<const std::uint8_t> request)
{
	if (request.size() < HdrLen)
		return {};
	if ((request[0] & MsgTypeMask) != MsgTypeControl)
		return {};
	if (!(request[1] & RqBit))
		return {}; // a response, not a request — not ours to answer

	const auto command = static_cast<Command>(request[2]);
	const std::span<const std::uint8_t> data = request.subspan(HdrLen);

	switch (command) {
	case Command::GetEndpointId: {
		auto response = begin_response(request, CompletionCode::Success);
		response.push_back(state.eid);
		response.push_back(EndpointTypeSimple);
		response.push_back(MediumSpecificNone);
		return response;
	}
	case Command::SetEndpointId: {
		if (data.size() < SetEidDataLen)
			return begin_response(
				request, CompletionCode::ErrorInvalidLength);
		// Accept the assignment by taking the requested EID. The
		// operation field (Set/Force/Reset/SetDiscovered) is not
		// distinguished at this milestone.
		state.eid = data[SetEidValueIndex];
		auto response = begin_response(request, CompletionCode::Success);
		response.push_back(SetEidStatusAccepted);
		response.push_back(state.eid);
		response.push_back(EidPoolSizeNone);
		return response;
	}
	case Command::GetEndpointUuid: {
		auto response = begin_response(request, CompletionCode::Success);
		response.insert(response.end(), state.uuid.begin(),
				state.uuid.end());
		return response;
	}
	case Command::GetMessageTypeSupport: {
		auto response = begin_response(request, CompletionCode::Success);
		response.push_back(SupportedTypeCount);
		response.push_back(MsgTypeControl);
		return response;
	}
	}

	return begin_response(request, CompletionCode::ErrorUnsupportedCmd);
}

} // namespace mctpctrl
