#include "mctp_core_binding.hpp"

#include "mctp_control.hpp"

extern "C" {
#include <libmctp.h>
}

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

#include <endian.h>
#include <unistd.h>

namespace mctpcore {

namespace {
// DSP0283 §8 USB framing header (4 bytes, identical wire layout to
// mctp_usb_frame.cpp so all existing tests remain valid against this path).
constexpr std::uint16_t UsbDmtfId = 0xB41A;
constexpr std::size_t UsbHdrSize = 4;

struct __attribute__((packed)) UsbHdr {
    std::uint16_t dmtf_id;   // 0xB41A, little-endian
    std::uint8_t rsvd;
    std::uint8_t byte_count; // total frame length including this 4-byte header
};
} // namespace

// C binding struct: the first member MUST be struct mctp_binding so that a
// mctp_binding * returned by libmctp can be safely cast back to FfsBindingC *
// via a reinterpret_cast (standard layout, same address).
//
// The back-pointer to Impl is void * because FfsBinding::Impl is a private
// nested struct; the cast to Impl * is done only inside the static member
// functions of Impl, where the private type is accessible.
struct FfsBindingC {
    struct mctp_binding binding; // must be first — see tx_cb cast
    void *impl_ptr;              // FfsBinding::Impl *, cast inside static fns
};

struct FfsBinding::Impl {
    FfsBindingC c_bind{};
    struct mctp *mctp = nullptr;
    mctpctrl::EndpointState *state = nullptr;
    int ep_in_fd = -1;
    int mps_in = 0;

    // libmctp calls this to transmit one (possibly fragmented) MCTP packet.
    // Returns 0 on success (pktbuf released by core), negative to drop.
    static int tx_cb(struct mctp_binding *b, struct mctp_pktbuf *pkt)
    {
        auto *fb = reinterpret_cast<FfsBindingC *>(b);
        Impl *impl = static_cast<Impl *>(fb->impl_ptr);

        // pkt->mctp_hdr_off is the byte offset of the MCTP header in pkt->data.
        // pkt->end - pkt->mctp_hdr_off is the full MCTP packet length.
        const auto *pkt_bytes =
            reinterpret_cast<const std::uint8_t *>(mctp_pktbuf_hdr(pkt));
        const std::size_t pkt_len = pkt->end - pkt->mctp_hdr_off;

        const std::size_t frame_len = UsbHdrSize + pkt_len;
        std::vector<std::uint8_t> frame(frame_len);

        UsbHdr hdr;
        hdr.dmtf_id = htole16(UsbDmtfId);
        hdr.rsvd = 0;
        hdr.byte_count = static_cast<std::uint8_t>(frame_len);
        std::memcpy(frame.data(), &hdr, UsbHdrSize);
        std::memcpy(frame.data() + UsbHdrSize, pkt_bytes, pkt_len);

        ssize_t rc = ::write(impl->ep_in_fd, frame.data(), frame.size());
        if (rc < 0 || static_cast<std::size_t>(rc) != frame.size()) {
            std::cerr << "mctp_core_binding: tx write failed\n";
            return -EIO;
        }

        // ZLP: a bulk-IN transfer ends for the host on a short packet.  When
        // the frame exactly fills whole MPS packets the host read never
        // completes; append a zero-length packet to terminate it.
        if (impl->mps_in > 0 &&
            frame.size() % static_cast<std::size_t>(impl->mps_in) == 0) {
            ::write(impl->ep_in_fd, frame.data(), 0);
        }

        return 0;
    }

    // libmctp delivers a fully reassembled MCTP message here.
    //
    // SDK openbmc libmctp 0.11 parameter order (differs from NVIDIA fork!):
    //   (src_eid, tag_owner, msg_tag, data /*user ctx*/, msg, len)
    static void rx_all_cb(uint8_t src_eid, bool /*tag_owner*/, uint8_t msg_tag,
                          void *data, void *msg, size_t len)
    {
        auto *impl = static_cast<Impl *>(data);
        if (len == 0)
            return;

        const std::span<const std::uint8_t> body{
            static_cast<const std::uint8_t *>(msg), len};

        const std::uint8_t msg_type = body[0] & mctpctrl::MsgTypeMask;
        std::vector<std::uint8_t> reply;

        switch (msg_type) {
        case mctpctrl::MsgTypeControl: {
            const std::uint8_t old_eid = impl->state->eid;
            reply = mctpctrl::handle(*impl->state, body);

            // Set Endpoint ID changed our EID: re-register so libmctp accepts
            // packets to the new EID (it filters on bus->eid).
            if (impl->state->eid != old_eid) {
                mctp_unregister_bus(impl->mctp, &impl->c_bind.binding);
                mctp_register_bus(impl->mctp, &impl->c_bind.binding,
                                  impl->state->eid);
                mctp_set_rx_all(impl->mctp, rx_all_cb, impl);
                mctp_binding_set_tx_enabled(&impl->c_bind.binding, true);
            }
            break;
        }
        default:
            return; // unknown type: drop (PLDM forwarding is Step C)
        }

        if (reply.empty())
            return;

        // Respond to the host: dest = src of request, TO = false (we are not
        // the tag owner — the host initiated with TO=1).
        mctp_message_tx(impl->mctp, src_eid, false, msg_tag, reply.data(),
                        reply.size());
    }

    Impl(int ep_fd, mctpctrl::EndpointState &st, int mps)
        : state(&st), ep_in_fd(ep_fd), mps_in(mps)
    {
        std::memset(&c_bind, 0, sizeof(c_bind));
        c_bind.binding.name = "ffs";
        c_bind.binding.version = 1;
        // pkt_size: one MCTP BTU packet (4-byte header + 64-byte payload).
        // pkt_header/pkt_trailer: zero (no binding-private framing bytes).
        c_bind.binding.pkt_size = MCTP_PACKET_SIZE(MCTP_BTU);
        c_bind.binding.tx = tx_cb;
        c_bind.impl_ptr = this;

        mctp = mctp_init();

        // Start with EID 0 (null/unassigned).  libmctp accepts packets where
        // dest == bus->eid || dest == MCTP_EID_NULL || dest == MCTP_EID_BROADCAST,
        // so Set EID (addressed to EID 0) is delivered before assignment.
        mctp_register_bus(mctp, &c_bind.binding, MCTP_EID_NULL);
        mctp_set_rx_all(mctp, rx_all_cb, this);
        mctp_binding_set_tx_enabled(&c_bind.binding, true);
    }

    ~Impl()
    {
        if (mctp) {
            mctp_unregister_bus(mctp, &c_bind.binding);
            mctp_destroy(mctp);
        }
    }
};

FfsBinding::FfsBinding(int ep_in_fd, mctpctrl::EndpointState &state,
                       int mps_in)
    : impl_(std::make_unique<Impl>(ep_in_fd, state, mps_in))
{}

FfsBinding::~FfsBinding() = default;

void FfsBinding::recv_frame(std::span<const std::uint8_t> usb_frame)
{
    if (usb_frame.size() < UsbHdrSize)
        return;

    const auto *hdr = reinterpret_cast<const UsbHdr *>(usb_frame.data());
    if (le16toh(hdr->dmtf_id) != UsbDmtfId)
        return;
    if (hdr->byte_count != static_cast<std::uint8_t>(usb_frame.size()))
        return;

    const auto mctp_pkt = usb_frame.subspan(UsbHdrSize);
    if (mctp_pkt.empty())
        return;

    struct mctp_pktbuf *pkt =
        mctp_pktbuf_alloc(&impl_->c_bind.binding, 0);
    if (!pkt)
        return;

    if (mctp_pktbuf_push(pkt,
                         const_cast<std::uint8_t *>(mctp_pkt.data()),
                         mctp_pkt.size()) != 0) {
        mctp_pktbuf_free(pkt);
        return;
    }

    // mctp_bus_rx takes ownership of pkt and frees it after processing.
    mctp_bus_rx(&impl_->c_bind.binding, pkt);
}

} // namespace mctpcore
