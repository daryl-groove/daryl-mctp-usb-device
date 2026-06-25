// Tests for mctpcore::DemuxServer — pure Unix socket IPC, no libmctp dependency.
// Runs natively (no UDC/FunctionFS required).

#include <gtest/gtest.h>

#include "mctp_demux_server.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Connect a SOCK_SEQPACKET client to the abstract "\0mctp-mux" socket.
// Returns the connected fd, or -1 on error.
static int make_client()
{
	int fd = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0)
		return -1;

	struct sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	addr.sun_path[0] = '\0';
	std::memcpy(addr.sun_path + 1, "mctp-mux", 8);
	const socklen_t addrlen = static_cast<socklen_t>(
		offsetof(sockaddr_un, sun_path) + 1 + 8);

	if (::connect(fd, reinterpret_cast<const sockaddr *>(&addr),
	              addrlen) != 0) {
		::close(fd);
		return -1;
	}
	return fd;
}

TEST(DemuxServer, ConstructionBindsSocket)
{
	mctpcore::DemuxServer demux;
	EXPECT_GE(demux.server_fd(), 0);
	EXPECT_EQ(demux.client_fd(), -1);
}

TEST(DemuxServer, AcceptRegistration)
{
	mctpcore::DemuxServer demux;
	ASSERT_GE(demux.server_fd(), 0);

	int cfd = make_client();
	ASSERT_GE(cfd, 0);

	// Send registration byte (PLDM type 0x01).
	std::uint8_t reg = 0x01;
	ASSERT_EQ(::send(cfd, &reg, 1, 0), 1);

	// Server accepts and reads registration.
	demux.accept_client();
	EXPECT_GE(demux.client_fd(), 0);

	::close(cfd);
}

TEST(DemuxServer, ForwardToClient)
{
	mctpcore::DemuxServer demux;
	ASSERT_GE(demux.server_fd(), 0);

	int cfd = make_client();
	ASSERT_GE(cfd, 0);

	std::uint8_t reg = 0x01;
	ASSERT_EQ(::send(cfd, &reg, 1, 0), 1);
	demux.accept_client();
	ASSERT_GE(demux.client_fd(), 0);

	// Server forwards src_eid=0x09 + msg=[0x01, 0xDE, 0xAD].
	const std::vector<std::uint8_t> msg = { 0x01, 0xDE, 0xAD };
	ASSERT_TRUE(demux.forward_to_client(0x09, msg));

	// Client receives [0x09][0x01][0xDE][0xAD].
	std::uint8_t buf[64];
	ssize_t n = ::recv(cfd, buf, sizeof(buf), 0);
	ASSERT_EQ(n, 4);
	EXPECT_EQ(buf[0], 0x09); // src_eid
	EXPECT_EQ(buf[1], 0x01);
	EXPECT_EQ(buf[2], 0xDE);
	EXPECT_EQ(buf[3], 0xAD);

	::close(cfd);
}

TEST(DemuxServer, RecvFromClient)
{
	mctpcore::DemuxServer demux;
	ASSERT_GE(demux.server_fd(), 0);

	int cfd = make_client();
	ASSERT_GE(cfd, 0);

	std::uint8_t reg = 0x01;
	ASSERT_EQ(::send(cfd, &reg, 1, 0), 1);
	demux.accept_client();
	ASSERT_GE(demux.client_fd(), 0);

	// Client sends [dest_eid=0x09][0x01][0xBE][0xEF].
	const std::uint8_t resp[] = { 0x09, 0x01, 0xBE, 0xEF };
	ASSERT_EQ(::send(cfd, resp, sizeof(resp), 0),
	          static_cast<ssize_t>(sizeof(resp)));

	auto result = demux.recv_from_client();
	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(result->first, 0x09); // dest_eid
	const std::vector<std::uint8_t> expected = { 0x01, 0xBE, 0xEF };
	EXPECT_EQ(result->second, expected);

	::close(cfd);
}

TEST(DemuxServer, ClientDisconnect)
{
	mctpcore::DemuxServer demux;
	ASSERT_GE(demux.server_fd(), 0);

	int cfd = make_client();
	ASSERT_GE(cfd, 0);

	std::uint8_t reg = 0x01;
	ASSERT_EQ(::send(cfd, &reg, 1, 0), 1);
	demux.accept_client();
	ASSERT_GE(demux.client_fd(), 0);

	// Client disconnects.
	::close(cfd);

	// recv_from_client detects EOF → returns nullopt, closes client.
	auto result = demux.recv_from_client();
	EXPECT_FALSE(result.has_value());
	EXPECT_EQ(demux.client_fd(), -1);
}

TEST(DemuxServer, ForwardNoClient)
{
	mctpcore::DemuxServer demux;
	ASSERT_GE(demux.server_fd(), 0);

	// No client connected — forward should fail gracefully.
	const std::vector<std::uint8_t> msg = { 0x01, 0xAA };
	EXPECT_FALSE(demux.forward_to_client(0x01, msg));
}
