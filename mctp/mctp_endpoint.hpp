// MCTP-over-USB endpoint orchestrator, device side.
//
// Ties the layers together: a received USB frame in, the USB frame to send back
// out (if any). This is the seam the FunctionFS daemon drives — one call per
// bulk-OUT transfer, the result going to bulk-IN. Pure (state in/out, no I/O),
// so the whole request->response path is testable off-target.

#pragma once

#include "mctp_control.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace mctpep {

// Process one received MCTP-over-USB frame. Returns the response frame to send,
// or empty when there is nothing to send: not an MCTP-USB frame, not a
// single-packet message, not a control request we answer, etc.
std::vector<std::uint8_t> process(mctpctrl::EndpointState &state,
				  std::span<const std::uint8_t> usbFrame);

} // namespace mctpep
