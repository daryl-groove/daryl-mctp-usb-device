// Tests for the MCTP control responder (DSP0236). Pure logic, runs anywhere.
//
// Message body layout used throughout:
//   request : [msgType=0x00][Rq|InstanceId][command][request data...]
//   response: [msgType=0x00][InstanceId   ][command][completion][response data...]

#include <gtest/gtest.h>

#include "mctp_control.hpp"

#include <cstdint>
#include <vector>

using Bytes = std::vector<std::uint8_t>;

// Request header byte: Rq set, instance id 3 (an arbitrary fixed id we expect
// echoed back with Rq cleared, i.e. 0x03).
constexpr std::uint8_t ReqHdr = mctpctrl::RqBit | 0x03;
constexpr std::uint8_t RespHdr = 0x03;

class ControlTest : public ::testing::Test {
protected:
	mctpctrl::EndpointState state;
};

TEST_F(ControlTest, GetEndpointId)
{
	const Bytes resp = mctpctrl::handle(state, Bytes{ 0x00, ReqHdr, 0x02 });
	// success, eid unassigned, simple endpoint, no medium-specific info.
	EXPECT_EQ(resp, (Bytes{ 0x00, RespHdr, 0x02, 0x00, 0x00, 0x00, 0x00 }));
}

TEST_F(ControlTest, SetThenGet)
{
	const Bytes setResp =
		mctpctrl::handle(state, Bytes{ 0x00, ReqHdr, 0x01, 0x00, 0x09 });
	EXPECT_EQ(setResp, (Bytes{ 0x00, RespHdr, 0x01, 0x00, 0x00, 0x09, 0x00 }));
	EXPECT_EQ(state.eid, std::uint8_t{ 0x09 });

	const Bytes getResp =
		mctpctrl::handle(state, Bytes{ 0x00, ReqHdr, 0x02 });
	ASSERT_EQ(getResp.size(), 7u);
	EXPECT_EQ(getResp[4], std::uint8_t{ 0x09 });
}

TEST_F(ControlTest, SetTooShort)
{
	const Bytes resp = mctpctrl::handle(state, Bytes{ 0x00, ReqHdr, 0x01 });
	EXPECT_EQ(resp, (Bytes{ 0x00, RespHdr, 0x01, 0x03 }));
}

TEST_F(ControlTest, GetUuid)
{
	const Bytes resp = mctpctrl::handle(state, Bytes{ 0x00, ReqHdr, 0x03 });
	ASSERT_EQ(resp.size(), 4u + 16u);
	EXPECT_EQ(resp[0], std::uint8_t{ 0x00 });
	EXPECT_EQ(resp[1], RespHdr);
	EXPECT_EQ(resp[2], std::uint8_t{ 0x03 });
	EXPECT_EQ(resp[3], std::uint8_t{ 0x00 }); // completion success
	const Bytes uuid(resp.begin() + 4, resp.end());
	EXPECT_EQ(uuid, (Bytes(mctpctrl::DefaultUuid.begin(),
			       mctpctrl::DefaultUuid.end())));
}

TEST_F(ControlTest, GetMessageTypeSupport)
{
	const Bytes resp = mctpctrl::handle(state, Bytes{ 0x00, ReqHdr, 0x05 });
	// success, one supported type: MCTP control (0x00).
	EXPECT_EQ(resp, (Bytes{ 0x00, RespHdr, 0x05, 0x00, 0x01, 0x00 }));
}

TEST_F(ControlTest, UnsupportedCommand)
{
	// 0x04 Get MCTP Version is deliberately not implemented.
	const Bytes resp = mctpctrl::handle(state, Bytes{ 0x00, ReqHdr, 0x04 });
	EXPECT_EQ(resp, (Bytes{ 0x00, RespHdr, 0x04, 0x05 }));
}

TEST_F(ControlTest, InstanceIdEchoed)
{
	const std::uint8_t hdr = mctpctrl::RqBit | 0x1f; // max instance id
	const Bytes resp = mctpctrl::handle(state, Bytes{ 0x00, hdr, 0x02 });
	ASSERT_GE(resp.size(), 2u);
	EXPECT_EQ(resp[1], std::uint8_t{ 0x1f }); // Rq cleared, instance id preserved
}

TEST_F(ControlTest, DroppedInputs)
{
	// Not a control message (PLDM type 0x01).
	EXPECT_TRUE(mctpctrl::handle(state, Bytes{ 0x01, ReqHdr, 0x02 }).empty());
	// A response (Rq clear), not a request.
	EXPECT_TRUE(mctpctrl::handle(state, Bytes{ 0x00, 0x03, 0x02 }).empty());
	// Too short to hold a header.
	EXPECT_TRUE(mctpctrl::handle(state, Bytes{ 0x00, ReqHdr }).empty());
}
