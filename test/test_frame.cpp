// Round-trip and validation tests for the MCTP-USB frame codec (DSP0283).
// Pure logic, runs on any host — no UDC, no USB stack.

#include <gtest/gtest.h>

#include "mctp_usb_frame.hpp"

#include <cstdint>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t HeaderLen = sizeof(mctpusb::UsbHeader);

TEST(UsbFrame, EncodeLayout)
{
	const Bytes pkt = { 0x01, 0x02, 0x03 };
	const Bytes frame = mctpusb::encode(pkt);

	ASSERT_EQ(frame.size(), HeaderLen + pkt.size());
	// Wire bytes of DMTF ID 0xB41A on a little-endian host.
	EXPECT_EQ(frame[0], std::uint8_t{ 0x1a });
	EXPECT_EQ(frame[1], std::uint8_t{ 0xb4 });
	EXPECT_EQ(frame[2], std::uint8_t{ 0x00 });
	EXPECT_EQ(frame[3], static_cast<std::uint8_t>(HeaderLen + pkt.size()));
	EXPECT_EQ(frame[4], std::uint8_t{ 0x01 });
	EXPECT_EQ(frame[5], std::uint8_t{ 0x02 });
	EXPECT_EQ(frame[6], std::uint8_t{ 0x03 });
}

TEST(UsbFrame, Roundtrip)
{
	const Bytes pkt = { 0xaa, 0xbb, 0xcc, 0xdd };
	const Bytes frame = mctpusb::encode(pkt);
	const mctpusb::DecodeResult r = mctpusb::decode(frame);

	EXPECT_EQ(r.error, mctpusb::DecodeError::Ok);
	// payload is a span (view into frame); compare element-wise for diagnostics.
	ASSERT_EQ(r.payload.size(), pkt.size());
	for (std::size_t i = 0; i < pkt.size(); ++i)
		EXPECT_EQ(r.payload[i], pkt[i]) << "at byte " << i;
}

TEST(UsbFrame, DecodeRejectsTooShort)
{
	const Bytes tiny = { 0x1a, 0xb4 };
	EXPECT_EQ(mctpusb::decode(tiny).error, mctpusb::DecodeError::TooShort);
}

TEST(UsbFrame, DecodeRejectsBadDmtfId)
{
	Bytes bad = mctpusb::encode(Bytes{ 0x01 });
	bad[0] = 0x00;
	EXPECT_EQ(mctpusb::decode(bad).error, mctpusb::DecodeError::BadDmtfId);
}

TEST(UsbFrame, DecodeRejectsLengthMismatch)
{
	Bytes mismatch = mctpusb::encode(Bytes{ 0x01, 0x02 });
	mismatch[3] = 0xff;
	EXPECT_EQ(mctpusb::decode(mismatch).error, mctpusb::DecodeError::LengthMismatch);
}
