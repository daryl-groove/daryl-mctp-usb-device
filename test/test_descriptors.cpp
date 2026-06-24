// Wire-layout tests for the FunctionFS descriptor/strings blobs.
//
// These run on any host (no UDC needed) and guard the part that fails silently
// at write(ep0): packing, total length, and little-endian field order. LE is
// checked by reading bytes back manually, so the assertions hold regardless of
// the host's own endianness.

#include <gtest/gtest.h>

#include "ffs_descriptors.hpp"

#include <cstdint>
#include <cstring>

namespace {

const std::uint8_t *bytes_of(const void *p)
{
	return static_cast<const std::uint8_t *>(p);
}

std::uint32_t read_le32(const std::uint8_t *p)
{
	return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 |
	       std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}

std::uint16_t read_le16(const std::uint8_t *p)
{
	return std::uint16_t(p[0] | p[1] << 8);
}

// Field offsets within usb_interface_descriptor we care about.
constexpr std::size_t IntfLen = 9;
constexpr std::size_t EpLen = 7; // no-audio endpoint descriptor
constexpr std::size_t OffNumEndpoints = 4;
constexpr std::size_t OffClass = 5;
constexpr std::size_t OffSubClass = 6;
constexpr std::size_t OffProtocol = 7;
constexpr std::size_t OffIInterface = 8;

// Helper: assert interface descriptor fields at `p`. Tag appears in failure output.
void check_interface(const std::uint8_t *p, const char *tag)
{
	EXPECT_EQ(p[0], std::uint8_t(IntfLen)) << tag << " bLength";
	EXPECT_EQ(p[1], std::uint8_t(USB_DT_INTERFACE)) << tag << " bDescriptorType";
	EXPECT_EQ(p[OffNumEndpoints], std::uint8_t(2)) << tag << " bNumEndpoints";
	EXPECT_EQ(p[OffClass], std::uint8_t(ffsdesc::McTpClassId))
		<< tag << " bInterfaceClass";
	EXPECT_EQ(p[OffSubClass], std::uint8_t(ffsdesc::McTpSubClass)) << tag;
	EXPECT_EQ(p[OffProtocol], std::uint8_t(ffsdesc::McTpProtocol)) << tag;
	EXPECT_EQ(p[OffIInterface], std::uint8_t(ffsdesc::InterfaceStringId))
		<< tag << " iInterface";
}

// Helper: assert endpoint descriptor fields at `p`. Tag appears in failure output.
void check_endpoint(const std::uint8_t *p, std::uint8_t address,
		    std::uint16_t maxPacket, const char *tag)
{
	EXPECT_EQ(p[0], std::uint8_t(EpLen)) << tag << " bLength";
	EXPECT_EQ(p[1], std::uint8_t(USB_DT_ENDPOINT)) << tag << " bDescriptorType";
	EXPECT_EQ(p[2], address) << tag << " bEndpointAddress";
	EXPECT_EQ(p[3], std::uint8_t(USB_ENDPOINT_XFER_BULK)) << tag << " bmAttributes";
	EXPECT_EQ(read_le16(p + 4), maxPacket) << tag << " wMaxPacketSize";
}

} // namespace

TEST(Descriptors, Layout)
{
	// Packed, no padding: 5 LE32 head fields + per speed (interface + 2 EPs).
	EXPECT_EQ(sizeof(ffsdesc::Descriptors),
		  5 * 4 + 2 * IntfLen + 4 * EpLen);

	const ffsdesc::Descriptors d = ffsdesc::make_descriptors();
	const std::uint8_t *p = bytes_of(&d);

	EXPECT_EQ(read_le32(p + 0), std::uint32_t(FUNCTIONFS_DESCRIPTORS_MAGIC_V2));
	EXPECT_EQ(read_le32(p + 4), std::uint32_t(sizeof(ffsdesc::Descriptors)));
	EXPECT_EQ(read_le32(p + 8),
		  std::uint32_t(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC));
	EXPECT_EQ(read_le32(p + 12), std::uint32_t(3)); // fsCount
	EXPECT_EQ(read_le32(p + 16), std::uint32_t(3)); // hsCount

	// FS block: interface, OUT (ep1), IN (ep2).
	const std::uint8_t *fs = p + 20;
	check_interface(fs, "fsIntf");
	check_endpoint(fs + IntfLen, ffsdesc::EpOutAddr, ffsdesc::FsMaxPacket, "fsOut");
	check_endpoint(fs + IntfLen + EpLen, ffsdesc::EpInAddr, ffsdesc::FsMaxPacket,
		       "fsIn");

	// HS block: same endpoints at the high-speed max packet size.
	const std::uint8_t *hs = fs + IntfLen + 2 * EpLen;
	check_interface(hs, "hsIntf");
	check_endpoint(hs + IntfLen, ffsdesc::EpOutAddr, ffsdesc::HsMaxPacket, "hsOut");
	check_endpoint(hs + IntfLen + EpLen, ffsdesc::EpInAddr, ffsdesc::HsMaxPacket,
		       "hsIn");
}

TEST(Descriptors, Strings)
{
	EXPECT_EQ(sizeof(ffsdesc::Strings),
		  4 * 4 + 2 + sizeof(ffsdesc::InterfaceString));

	const ffsdesc::Strings s = ffsdesc::make_strings();
	const std::uint8_t *p = bytes_of(&s);

	EXPECT_EQ(read_le32(p + 0), std::uint32_t(FUNCTIONFS_STRINGS_MAGIC));
	EXPECT_EQ(read_le32(p + 4), std::uint32_t(sizeof(ffsdesc::Strings)));
	EXPECT_EQ(read_le32(p + 8), std::uint32_t(1));  // strCount
	EXPECT_EQ(read_le32(p + 12), std::uint32_t(1)); // langCount
	EXPECT_EQ(read_le16(p + 16), std::uint16_t(ffsdesc::LangIdEnUs));
	EXPECT_STREQ(s.str0, ffsdesc::InterfaceString);
}
