// End-to-end test of the device path: a full MCTP-over-USB request frame in,
// a full response frame out, decoded and checked. Exercises frame + packet +
// control together, with no UDC and no USB stack.

#include <gtest/gtest.h>

#include "mctp_control.hpp"
#include "mctp_endpoint.hpp"
#include "mctp_packet.hpp"
#include "mctp_usb_frame.hpp"

#include <cstdint>
#include <vector>

using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr std::uint8_t BusOwnerEid = 0x08;
constexpr std::uint8_t ReqCtrlHdr = mctpctrl::RqBit | 0x03; // Rq, instance id 3

// Wrap an MCTP control message body in a single-packet MCTP request addressed
// to `dest`, then in an MCTP-over-USB frame.
Bytes request_frame(std::uint8_t dest, const Bytes &ctrl)
{
	const std::uint8_t flags =
		mctppkt::FlagSom | mctppkt::FlagEom | mctppkt::FlagTagOwner | 0x01;
	Bytes packet = { mctppkt::Version1, dest, BusOwnerEid, flags };
	packet.insert(packet.end(), ctrl.begin(), ctrl.end());
	return mctpusb::encode(packet);
}

// Decode a response frame back down to its MCTP packet.
mctppkt::ParseResult unwrap(const Bytes &frame)
{
	const mctpusb::DecodeResult f = mctpusb::decode(frame);
	if (f.error != mctpusb::DecodeError::Ok)
		return { mctppkt::ParseError::TooShort, {}, {} };
	return mctppkt::parse(f.payload);
}

} // namespace

TEST(Endpoint, SetThenGetEidEndToEnd)
{
	mctpctrl::EndpointState state; // EID unassigned (0)

	// Bus owner assigns EID 9 while we still answer at the null EID.
	const Bytes setFrame =
		request_frame(0x00, Bytes{ 0x00, ReqCtrlHdr, 0x01, 0x00, 0x09 });
	const Bytes setResp = mctpep::process(state, setFrame);
	EXPECT_FALSE(setResp.empty());
	EXPECT_EQ(state.eid, std::uint8_t{ 0x09 });

	const mctppkt::ParseResult sr = unwrap(setResp);
	EXPECT_EQ(sr.error, mctppkt::ParseError::Ok);
	EXPECT_EQ(sr.header.dest, BusOwnerEid);
	EXPECT_EQ(sr.header.src, std::uint8_t{ 0x09 });
	// completion code success, status accepted, EID setting 9, pool 0.
	ASSERT_EQ(sr.body.size(), 7u);
	EXPECT_EQ(sr.body[3], std::uint8_t{ 0x00 }); // completion success
	EXPECT_EQ(sr.body[5], std::uint8_t{ 0x09 }); // EID assigned

	// Now Get EID, addressed to our assigned EID 9.
	const Bytes getFrame =
		request_frame(0x09, Bytes{ 0x00, ReqCtrlHdr, 0x02 });
	const Bytes getResp = mctpep::process(state, getFrame);
	const mctppkt::ParseResult gr = unwrap(getResp);
	EXPECT_EQ(gr.error, mctppkt::ParseError::Ok);
	EXPECT_EQ(gr.header.src, std::uint8_t{ 0x09 });
	ASSERT_EQ(gr.body.size(), 7u);
	EXPECT_EQ(gr.body[3], std::uint8_t{ 0x00 }); // completion success
	EXPECT_EQ(gr.body[4], std::uint8_t{ 0x09 }); // reported EID == 9
}

TEST(Endpoint, NonMctpFrameIgnored)
{
	mctpctrl::EndpointState state;
	// Valid-looking bytes but wrong DMTF id -> decode fails -> no response.
	const Bytes junk = { 0x00, 0x00, 0x00, 0x04, 0x01, 0x02, 0x03 };
	EXPECT_TRUE(mctpep::process(state, junk).empty());
}
