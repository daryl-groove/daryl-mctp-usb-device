// Tests for the MCTP transport/packet layer (DSP0236). Pure logic, runs anywhere.

#include <gtest/gtest.h>

#include "mctp_packet.hpp"

#include <cstdint>
#include <vector>

using Bytes = std::vector<std::uint8_t>;

// A request addressed to us (dest 9) from the bus owner (src 0): single packet,
// Tag Owner set, message tag 1.
constexpr std::uint8_t ReqFlags =
	mctppkt::FlagSom | mctppkt::FlagEom | mctppkt::FlagTagOwner | 0x01;

TEST(Packet, ParseSinglePacket)
{
	const Bytes pkt = { mctppkt::Version1, 0x09, 0x00, ReqFlags, 0xde, 0xad };
	const mctppkt::ParseResult r = mctppkt::parse(pkt);

	EXPECT_EQ(r.error, mctppkt::ParseError::Ok);
	EXPECT_EQ(r.header.dest, std::uint8_t{ 0x09 });
	EXPECT_EQ(r.header.src, std::uint8_t{ 0x00 });
	ASSERT_EQ(r.body.size(), 2u);
	EXPECT_EQ(r.body[0], std::uint8_t{ 0xde });
	EXPECT_EQ(r.body[1], std::uint8_t{ 0xad });
}

TEST(Packet, ParseRejectsTooShort)
{
	EXPECT_EQ(mctppkt::parse(Bytes{ 0x01, 0x09, 0x00 }).error,
		  mctppkt::ParseError::TooShort);
}

TEST(Packet, ParseRejectsMultiPacket)
{
	// EOM cleared -> a fragment of a multi-packet message.
	const std::uint8_t fragFlags = mctppkt::FlagSom | mctppkt::FlagTagOwner;
	EXPECT_EQ(
		mctppkt::parse(Bytes{ 0x01, 0x09, 0x00, fragFlags, 0x00 }).error,
		mctppkt::ParseError::NotSinglePacket);
}

TEST(Packet, BuildResponse)
{
	const mctppkt::Header reqHdr = { mctppkt::Version1, 0x09, 0x00, ReqFlags };
	const Bytes body = { 0xaa, 0xbb };
	const Bytes resp = mctppkt::build_response(reqHdr, 0x09, body);

	// dest/src swapped, Tag Owner cleared, tag 1 kept, single packet.
	const std::uint8_t wantFlags = mctppkt::FlagSom | mctppkt::FlagEom | 0x01;
	ASSERT_EQ(resp.size(), 4u + body.size());
	EXPECT_EQ(resp[0], mctppkt::Version1);
	EXPECT_EQ(resp[1], std::uint8_t{ 0x00 }); // dest := request src
	EXPECT_EQ(resp[2], std::uint8_t{ 0x09 }); // src := our eid
	EXPECT_EQ(resp[3], wantFlags);
	EXPECT_EQ(resp[4], std::uint8_t{ 0xaa });
	EXPECT_EQ(resp[5], std::uint8_t{ 0xbb });
}

TEST(Packet, Roundtrip)
{
	const mctppkt::Header reqHdr = { mctppkt::Version1, 0x09, 0x00, ReqFlags };
	const Bytes resp = mctppkt::build_response(reqHdr, 0x09, Bytes{ 0x01 });
	const mctppkt::ParseResult r = mctppkt::parse(resp);

	EXPECT_EQ(r.error, mctppkt::ParseError::Ok);
	EXPECT_EQ(r.header.dest, std::uint8_t{ 0x00 });
	EXPECT_EQ(r.header.src, std::uint8_t{ 0x09 });
}
