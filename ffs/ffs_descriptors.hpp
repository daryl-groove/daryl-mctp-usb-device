// FunctionFS descriptor + strings blob builders (Milestone 2).
//
// Pure, side-effect-free: they produce the exact byte layout the kernel expects
// on ep0, with no I/O. Kept separate from the daemon so the risky part — the
// wire layout — is unit-testable on a host with no UDC.
//
// The interface is now a real MCTP-over-USB function (DSP0283): interface class
// 0x14 plus two bulk endpoints (OUT then IN). FunctionFS names the endpoint
// files by their order here, so OUT becomes ep1 and IN becomes ep2.

#pragma once

#include <cstdint>
#include <cstring>

#include <endian.h>

#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

// Namespace is 'ffsdesc' not 'ffs' on purpose: libc declares ffs() (find first
// bit set), and a namespace named ffs would collide with it.
namespace ffsdesc {

// String index 1 is reported by the interface descriptor as iInterface; index 0
// means "no string", so the first usable string is 1.
inline constexpr std::uint8_t InterfaceStringId = 1;
inline constexpr char InterfaceString[] = "MCTP over USB (DSP0283)";
inline constexpr std::uint16_t LangIdEnUs = 0x0409;

// MCTP-over-USB interface identity (DSP0283). libmctp's host binding matches on
// bInterfaceClass == 0x14 only; subclass/protocol just need to be well-formed.
// Subclass 0 / protocol 1 == a managed device endpoint speaking MCTP 1.x, the
// same values Zephyr's mctp_usb class uses.
inline constexpr std::uint8_t McTpClassId = 0x14;
inline constexpr std::uint8_t McTpSubClass = 0x00;
inline constexpr std::uint8_t McTpProtocol = 0x01;

// Two bulk endpoints. Order here decides the FunctionFS ep file names: OUT first
// (ep1, host->device), IN second (ep2, device->host).
inline constexpr std::uint8_t EpOutAddr = USB_DIR_OUT | 0x01; // 0x01
inline constexpr std::uint8_t EpInAddr = USB_DIR_IN | 0x01;   // 0x81
inline constexpr std::uint16_t FsMaxPacket = 64;
inline constexpr std::uint16_t HsMaxPacket = 512;

// Full descriptor block in the FunctionFS V2 wire layout. The head struct in
// functionfs.h only covers magic/length/flags; the per-speed counts and the
// descriptors themselves must follow manually, so the whole thing is spelled
// out here as one packed struct to keep the byte layout obvious. Each speed
// lists the interface then its two endpoints (same order, so FS/HS map to the
// same ep files).
struct __attribute__((packed)) Descriptors {
	__le32 magic;
	__le32 length;
	__le32 flags;
	__le32 fsCount;
	__le32 hsCount;
	usb_interface_descriptor fsIntf;
	usb_endpoint_descriptor_no_audio fsOut;
	usb_endpoint_descriptor_no_audio fsIn;
	usb_interface_descriptor hsIntf;
	usb_endpoint_descriptor_no_audio hsOut;
	usb_endpoint_descriptor_no_audio hsIn;
};

struct __attribute__((packed)) Strings {
	__le32 magic;
	__le32 length;
	__le32 strCount;
	__le32 langCount;
	__le16 lang;
	char str0[sizeof(InterfaceString)];
};

inline usb_interface_descriptor make_interface()
{
	usb_interface_descriptor intf{};
	intf.bLength = sizeof(usb_interface_descriptor);
	intf.bDescriptorType = USB_DT_INTERFACE;
	intf.bInterfaceNumber = 0;
	intf.bAlternateSetting = 0;
	intf.bNumEndpoints = 2;
	intf.bInterfaceClass = McTpClassId;
	intf.bInterfaceSubClass = McTpSubClass;
	intf.bInterfaceProtocol = McTpProtocol;
	intf.iInterface = InterfaceStringId;
	return intf;
}

inline usb_endpoint_descriptor_no_audio make_endpoint(std::uint8_t address,
						      std::uint16_t maxPacket)
{
	usb_endpoint_descriptor_no_audio ep{};
	ep.bLength = USB_DT_ENDPOINT_SIZE; // 7: the no-audio form
	ep.bDescriptorType = USB_DT_ENDPOINT;
	ep.bEndpointAddress = address;
	ep.bmAttributes = USB_ENDPOINT_XFER_BULK;
	ep.wMaxPacketSize = htole16(maxPacket);
	ep.bInterval = 0; // ignored for bulk
	return ep;
}

inline Descriptors make_descriptors()
{
	Descriptors d{};
	d.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
	d.length = htole32(sizeof(d));
	d.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
	d.fsCount = htole32(3); // interface + OUT + IN
	d.hsCount = htole32(3);
	d.fsIntf = make_interface();
	d.fsOut = make_endpoint(EpOutAddr, FsMaxPacket);
	d.fsIn = make_endpoint(EpInAddr, FsMaxPacket);
	d.hsIntf = make_interface();
	d.hsOut = make_endpoint(EpOutAddr, HsMaxPacket);
	d.hsIn = make_endpoint(EpInAddr, HsMaxPacket);
	return d;
}

inline Strings make_strings()
{
	Strings s{};
	s.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
	s.length = htole32(sizeof(s));
	s.strCount = htole32(1);
	s.langCount = htole32(1);
	s.lang = htole16(LangIdEnUs);
	std::memcpy(s.str0, InterfaceString, sizeof(InterfaceString));
	return s;
}

} // namespace ffsdesc
