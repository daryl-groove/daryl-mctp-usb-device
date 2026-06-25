// libmctp-core FunctionFS binding (Step B).
//
// Wraps the libmctp-core library as a pure transport engine (no AF_MCTP,
// no kernel dependency).  Two FunctionFS bulk endpoints are wired to core:
//
//   recv_frame(): feed one USB frame read from ep-OUT (host→device).
//     libmctp strips the DSP0283 USB header, parses the MCTP packet header,
//     reassembles multi-packet messages, and delivers whole messages to the
//     internal rx callback.
//
//   tx callback (internal): called by mctp_message_tx() for each outbound
//     packet.  Prepends the 4-byte DSP0283 USB framing header and writes to
//     the ep-IN fd (device→host).
//
// The MCTP control responder (mctpctrl::handle, type 0x00) is unchanged from
// the standalone path.  After a successful Set Endpoint ID the binding
// re-registers the bus with the new EID so libmctp accepts subsequent packets
// addressed to that EID.  Unknown message types are dropped (PLDM forwarding
// is Step C).

#pragma once

#include "mctp_control.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace mctpcore {

class FfsBinding {
public:
    // ep_in_fd : open fd for ep2 (bulk IN, device→host); replies are written here.
    // state    : MCTP endpoint state (EID, UUID); ownership stays with the caller.
    // mps_in   : ep2 wMaxPacketSize; used for ZLP detection (0 = skip ZLP).
    FfsBinding(int ep_in_fd, mctpctrl::EndpointState &state, int mps_in = 0);
    ~FfsBinding();
    FfsBinding(const FfsBinding &) = delete;
    FfsBinding &operator=(const FfsBinding &) = delete;

    // Feed one raw USB frame (as read from ep-OUT) into libmctp-core.
    // Reassembly, message-type dispatch, and reply transmission are handled
    // internally; the caller does not need to write to ep-IN.
    void recv_frame(std::span<const std::uint8_t> usb_frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mctpcore
