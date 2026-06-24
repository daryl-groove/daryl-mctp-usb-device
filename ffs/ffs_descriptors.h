#pragma once

#include <stdint.h>
#include <string.h>
#include <endian.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

/*
 * FunctionFS V2 descriptor + strings blob for MCTP-over-USB (DSP0283).
 * Pure data builders — no I/O; safe to unit-test off-target.
 *
 * Namespace prefix 'ffsdesc' avoids colliding with libc ffs().
 */

/* String index 1 is reported as iInterface; 0 means "no string". */
#define FFSDESC_INTF_STRING_ID  1
#define FFSDESC_LANG_ID_EN_US   0x0409u

static const char ffsdesc_intf_string[] = "MCTP over USB (DSP0283)";

/* MCTP-over-USB interface identity (DSP0283). */
#define FFSDESC_CLASS     0x14u
#define FFSDESC_SUBCLASS  0x00u
#define FFSDESC_PROTOCOL  0x01u

/* Endpoint addresses: OUT first (ep1), IN second (ep2). */
#define FFSDESC_EP_OUT_ADDR  (USB_DIR_OUT | 0x01u)  /* 0x01 */
#define FFSDESC_EP_IN_ADDR   (USB_DIR_IN  | 0x01u)  /* 0x81 */
#define FFSDESC_FS_MPS       64u
#define FFSDESC_HS_MPS       512u

/* Full FunctionFS V2 descriptor block (FS + HS, each: interface + OUT + IN). */
struct __attribute__((packed)) ffsdesc_descriptors {
	__le32 magic;
	__le32 length;
	__le32 flags;
	__le32 fs_count;
	__le32 hs_count;
	struct usb_interface_descriptor         fs_intf;
	struct usb_endpoint_descriptor_no_audio fs_out;
	struct usb_endpoint_descriptor_no_audio fs_in;
	struct usb_interface_descriptor         hs_intf;
	struct usb_endpoint_descriptor_no_audio hs_out;
	struct usb_endpoint_descriptor_no_audio hs_in;
};

struct __attribute__((packed)) ffsdesc_strings {
	__le32 magic;
	__le32 length;
	__le32 str_count;
	__le32 lang_count;
	__le16 lang;
	char   str0[sizeof(ffsdesc_intf_string)];
};

static inline struct usb_interface_descriptor ffsdesc_make_intf(void)
{
	struct usb_interface_descriptor d;
	memset(&d, 0, sizeof(d));
	d.bLength            = sizeof(d);
	d.bDescriptorType    = USB_DT_INTERFACE;
	d.bInterfaceNumber   = 0;
	d.bAlternateSetting  = 0;
	d.bNumEndpoints      = 2;
	d.bInterfaceClass    = FFSDESC_CLASS;
	d.bInterfaceSubClass = FFSDESC_SUBCLASS;
	d.bInterfaceProtocol = FFSDESC_PROTOCOL;
	d.iInterface         = FFSDESC_INTF_STRING_ID;
	return d;
}

static inline struct usb_endpoint_descriptor_no_audio
ffsdesc_make_ep(uint8_t addr, uint16_t mps)
{
	struct usb_endpoint_descriptor_no_audio d;
	memset(&d, 0, sizeof(d));
	d.bLength          = USB_DT_ENDPOINT_SIZE;
	d.bDescriptorType  = USB_DT_ENDPOINT;
	d.bEndpointAddress = addr;
	d.bmAttributes     = USB_ENDPOINT_XFER_BULK;
	d.wMaxPacketSize   = htole16(mps);
	d.bInterval        = 0;
	return d;
}

static inline struct ffsdesc_descriptors ffsdesc_make_descriptors(void)
{
	struct ffsdesc_descriptors d;
	memset(&d, 0, sizeof(d));
	d.magic    = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
	d.length   = htole32(sizeof(d));
	d.flags    = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
	d.fs_count = htole32(3);
	d.hs_count = htole32(3);
	d.fs_intf  = ffsdesc_make_intf();
	d.fs_out   = ffsdesc_make_ep(FFSDESC_EP_OUT_ADDR, FFSDESC_FS_MPS);
	d.fs_in    = ffsdesc_make_ep(FFSDESC_EP_IN_ADDR,  FFSDESC_FS_MPS);
	d.hs_intf  = ffsdesc_make_intf();
	d.hs_out   = ffsdesc_make_ep(FFSDESC_EP_OUT_ADDR, FFSDESC_HS_MPS);
	d.hs_in    = ffsdesc_make_ep(FFSDESC_EP_IN_ADDR,  FFSDESC_HS_MPS);
	return d;
}

static inline struct ffsdesc_strings ffsdesc_make_strings(void)
{
	struct ffsdesc_strings s;
	memset(&s, 0, sizeof(s));
	s.magic      = htole32(FUNCTIONFS_STRINGS_MAGIC);
	s.length     = htole32(sizeof(s));
	s.str_count  = htole32(1);
	s.lang_count = htole32(1);
	s.lang       = htole16(FFSDESC_LANG_ID_EN_US);
	memcpy(s.str0, ffsdesc_intf_string, sizeof(ffsdesc_intf_string));
	return s;
}
