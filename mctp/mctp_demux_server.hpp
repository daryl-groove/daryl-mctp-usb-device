// Unix demux socket server — libpldm mctp-demux dialect (Step C2).
//
// mctpusbd listens here; pldmd connects as a client.
//
// Wire protocol (libpldm, NOT the NVIDIA mctp-demux-daemon):
//   Socket:    AF_UNIX SOCK_SEQPACKET, abstract path "\0mctp-mux"
//   Register:  on connect, client sends 1 byte = msg type it handles
//   RX:        server→client: [src_eid][msg_type][pldm_payload...]
//   TX:        client→server: [dest_eid][msg_type][pldm_payload...]
//
// One client at a time (sufficient for a device-side responder).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace mctpcore {

// PLDM message type (0x01) — the type DemuxServer currently routes.
inline constexpr std::uint8_t MsgTypePldm = 0x01;

class DemuxServer {
public:
	// Creates the AF_UNIX SOCK_SEQPACKET server socket and starts listening.
	// Logs to stderr on failure; server_fd() returns -1 if construction failed.
	DemuxServer();
	~DemuxServer();
	DemuxServer(const DemuxServer &) = delete;
	DemuxServer &operator=(const DemuxServer &) = delete;

	// fd to watch with POLLIN for incoming connections. -1 if init failed.
	int server_fd() const { return server_fd_; }

	// fd to watch with POLLIN for client→server messages. -1 if no client.
	int client_fd() const { return client_fd_; }

	// Call when server_fd() is readable: accept connection + read 1-byte
	// registration. Displaces any existing client.
	void accept_client();

	// Send [src_eid][mctp_msg...] to the connected client.
	// mctp_msg is the full MCTP message body (type byte + payload).
	// Returns false if no client or send failed (client is closed on failure).
	bool forward_to_client(std::uint8_t src_eid,
	                       std::span<const std::uint8_t> mctp_msg);

	// Call when client_fd() is readable.
	// Returns {dest_eid, mctp_msg} where mctp_msg = [type][payload...].
	// Returns nullopt on disconnect or error (client is closed; client_fd→-1).
	std::optional<std::pair<std::uint8_t, std::vector<std::uint8_t>>>
	recv_from_client();

private:
	int server_fd_ = -1;
	int client_fd_ = -1;

	void close_client();
};

} // namespace mctpcore
