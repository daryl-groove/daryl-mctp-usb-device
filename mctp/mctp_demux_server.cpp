#include "mctp_demux_server.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <iostream>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mctpcore {

namespace {
// Abstract socket name (without the leading '\0'); full path = "\0mctp-mux".
constexpr const char SockName[] = "mctp-mux";
// Maximum single client→server message (generous; PLDM PDRs can be large).
constexpr std::size_t MaxMsg = 4096;
} // namespace

DemuxServer::DemuxServer()
{
	server_fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (server_fd_ < 0) {
		std::cerr << std::format("demux: socket: {}\n",
		                         std::strerror(errno));
		return;
	}

	struct sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	addr.sun_path[0] = '\0'; // abstract namespace
	std::memcpy(addr.sun_path + 1, SockName, sizeof(SockName) - 1);
	const socklen_t addrlen = static_cast<socklen_t>(
		offsetof(sockaddr_un, sun_path) + 1 + sizeof(SockName) - 1);

	if (::bind(server_fd_, reinterpret_cast<const sockaddr *>(&addr),
	           addrlen) != 0) {
		std::cerr << std::format("demux: bind: {}\n",
		                         std::strerror(errno));
		::close(server_fd_);
		server_fd_ = -1;
		return;
	}

	if (::listen(server_fd_, 1) != 0) {
		std::cerr << std::format("demux: listen: {}\n",
		                         std::strerror(errno));
		::close(server_fd_);
		server_fd_ = -1;
	}
}

DemuxServer::~DemuxServer()
{
	close_client();
	if (server_fd_ >= 0)
		::close(server_fd_);
}

void DemuxServer::close_client()
{
	if (client_fd_ >= 0) {
		::close(client_fd_);
		client_fd_ = -1;
	}
}

void DemuxServer::accept_client()
{
	if (server_fd_ < 0)
		return;

	int fd = ::accept4(server_fd_, nullptr, nullptr, SOCK_CLOEXEC);
	if (fd < 0) {
		std::cerr << std::format("demux: accept: {}\n",
		                         std::strerror(errno));
		return;
	}

	close_client(); // displace previous client if any

	// First message from the client is the 1-byte msg-type registration.
	// pldmd sends it immediately after connect; a blocking read is safe.
	std::uint8_t type;
	if (::recv(fd, &type, 1, 0) != 1) {
		std::cerr << "demux: registration read failed; dropping client\n";
		::close(fd);
		return;
	}

	client_fd_ = fd;
	std::cout << std::format("demux: client connected, type=0x{:02x}\n",
	                         static_cast<unsigned>(type));
}

bool DemuxServer::forward_to_client(std::uint8_t src_eid,
                                    std::span<const std::uint8_t> mctp_msg)
{
	if (client_fd_ < 0)
		return false;

	// Build [src_eid][mctp_msg...] in one buffer for a single send().
	std::vector<std::uint8_t> buf;
	buf.reserve(1 + mctp_msg.size());
	buf.push_back(src_eid);
	buf.insert(buf.end(), mctp_msg.begin(), mctp_msg.end());

	ssize_t n = ::send(client_fd_, buf.data(), buf.size(), MSG_NOSIGNAL);
	if (n < 0 || static_cast<std::size_t>(n) != buf.size()) {
		std::cerr << "demux: forward failed; closing client\n";
		close_client();
		return false;
	}
	return true;
}

std::optional<std::pair<std::uint8_t, std::vector<std::uint8_t>>>
DemuxServer::recv_from_client()
{
	if (client_fd_ < 0)
		return std::nullopt;

	std::uint8_t buf[MaxMsg];
	ssize_t n = ::recv(client_fd_, buf, sizeof(buf), 0);
	if (n <= 0) {
		if (n < 0)
			std::cerr << std::format("demux: recv: {}\n",
			                         std::strerror(errno));
		else
			std::cout << "demux: client disconnected\n";
		close_client();
		return std::nullopt;
	}

	// Need at least [dest_eid][type] (2 bytes).
	if (static_cast<std::size_t>(n) < 2)
		return std::nullopt;

	const std::uint8_t dest_eid = buf[0];
	std::vector<std::uint8_t> msg(buf + 1, buf + n);
	return std::make_pair(dest_eid, std::move(msg));
}

} // namespace mctpcore
