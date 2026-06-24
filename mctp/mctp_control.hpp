// MCTP control-message responder (DSP0236), device/endpoint side.
//
// Pure request->response logic over the MCTP *message body* (starting at the
// message-type byte): no transport, no USB, no I/O. This is the responder the
// strategy calls out as the bulk of Milestone 3b — libmctp's core has no built-in
// control responder, so a simple endpoint must answer these itself. Unit-tested
// off-target.
//
// Supported commands (a simple endpoint, per the strategy): Set/Get Endpoint ID,
// Get Endpoint UUID, Get Message Type Support. Anything else gets a well-formed
// "unsupported command" response.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace mctpctrl {

// MCTP message type for control messages; the IC (integrity check) bit is the
// high bit and is masked off when classifying.
inline constexpr std::uint8_t MsgTypeControl = 0x00;
inline constexpr std::uint8_t MsgTypeMask = 0x7f;

// Control-message header bits (byte after the message type).
inline constexpr std::uint8_t RqBit = 0x80; // set in requests, clear in responses

enum class Command : std::uint8_t {
	SetEndpointId = 0x01,
	GetEndpointId = 0x02,
	GetEndpointUuid = 0x03,
	GetMessageTypeSupport = 0x05,
};

enum class CompletionCode : std::uint8_t {
	Success = 0x00,
	ErrorInvalidLength = 0x03,
	ErrorUnsupportedCmd = 0x05,
};

// EID a freshly powered endpoint reports before a bus owner assigns one.
inline constexpr std::uint8_t UnassignedEid = 0x00;

// Placeholder dev identity; a real device would burn in a per-unit UUID.
inline constexpr std::array<std::uint8_t, 16> DefaultUuid = {
	0xd6, 0x1a, 0x4c, 0x70, 0x2b, 0x55, 0x4e, 0x9a,
	0x8f, 0x13, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
};

// Endpoint state the responder reads and (for Set Endpoint ID) mutates.
struct EndpointState {
	std::uint8_t eid = UnassignedEid;
	std::array<std::uint8_t, 16> uuid = DefaultUuid;
};

// Handle one control message body. Returns the response message body, or an
// empty vector when the input is not a control request we should answer (wrong
// message type, response rather than request, or too short to parse a header).
std::vector<std::uint8_t> handle(EndpointState &state,
				 std::span<const std::uint8_t> request);

// Human-readable name for a control command code, for logging/diagnostics.
const char *command_name(std::uint8_t code);

} // namespace mctpctrl
