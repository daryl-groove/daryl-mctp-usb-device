// ffs_daemon — FunctionFS MCTP event loop (see ffs_daemon.hpp).
//
// Bring-up order matters: descriptors+strings MUST be written to ep0 before the
// gadget is bound to a UDC, otherwise the bind fails. on_ready() is therefore
// the seam where a caller that owns the gadget (mctpusbd) enables the UDC.
// The ep1/ep2 files only exist after the host ENABLEs the configuration.
//
// Being an example, this logs verbosely and unconditionally: every ep0 event,
// plus a hex + decoded trace of each bulk transfer in and out.

#include "ffs_daemon.hpp"

#include "ffs_descriptors.hpp"
#include "mctp_control.hpp"
#include "mctp_endpoint.hpp"
#include "mctp_packet.hpp"
#include "mctp_usb_frame.hpp"

#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <vector>

#include <endian.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/usb/functionfs.h>

namespace ffsd {

volatile std::sig_atomic_t g_stop = 0;

namespace {

// Largest single OUT transfer we accept. One MCTP-over-USB frame is at most
// 4 (USB header) + 255 (1-byte length field) bytes; round up for headroom.
constexpr std::size_t MaxFrame = 1024;

// Owning fd wrapper: closes on destruction, movable so endpoints can be opened
// on ENABLE and closed on DISABLE.
class Fd {
public:
	Fd() = default;
	explicit Fd(int fd) : fd_(fd) {}
	~Fd() { close(); }
	Fd(Fd &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
	Fd &operator=(Fd &&o) noexcept
	{
		if (this != &o) {
			close();
			fd_ = o.fd_;
			o.fd_ = -1;
		}
		return *this;
	}
	Fd(const Fd &) = delete;
	Fd &operator=(const Fd &) = delete;

	int get() const { return fd_; }
	bool valid() const { return fd_ >= 0; }
	void close()
	{
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}

private:
	int fd_ = -1;
};

// Daemon state carried across events and data transfers.
struct Context {
	std::string mount;
	Mode mode = Mode::Mctp;
	Fd epOut;		      // ep1, host -> device (we read)
	Fd epIn;		      // ep2, device -> host (we write)
	int mpsIn = 0;		      // ep2 wMaxPacketSize, for ZLP decisions
	mctpctrl::EndpointState state; // MCTP endpoint state (EID etc.)
	FrameProcessor frame_proc;     // optional override for MCTP data path
};

enum class Ep0Status { Continue, Closed, Error };

const char *event_name(std::uint8_t type)
{
	switch (type) {
	case FUNCTIONFS_BIND:
		return "BIND";
	case FUNCTIONFS_UNBIND:
		return "UNBIND";
	case FUNCTIONFS_ENABLE:
		return "ENABLE";
	case FUNCTIONFS_DISABLE:
		return "DISABLE";
	case FUNCTIONFS_SETUP:
		return "SETUP";
	case FUNCTIONFS_SUSPEND:
		return "SUSPEND";
	case FUNCTIONFS_RESUME:
		return "RESUME";
	default:
		return "?";
	}
}

bool write_all(int fd, const void *buf, std::size_t len, const char *what)
{
	auto rc = ::write(fd, buf, len);
	if (rc < 0) {
		std::cerr << std::format("write {} failed: {}\n", what,
					 std::strerror(errno));
		return false;
	}
	if (static_cast<std::size_t>(rc) != len) {
		std::cerr << std::format("write {} short: {}/{}\n", what, rc,
					 len);
		return false;
	}
	return true;
}

void hex_dump(const char *label, std::span<const std::uint8_t> data)
{
	std::string hex;
	for (std::uint8_t b : data)
		hex += std::format("{:02x} ", static_cast<unsigned>(b));
	std::cout << std::format("{} {}B: {}\n", label, data.size(), hex);
}

// Hex dump plus a best-effort decode of one MCTP-over-USB frame, for the demo
// trace. Reuses the real codec (decode -> parse -> control header) rather than
// poking at byte offsets, so the trace can never disagree with what the device
// actually does.
void trace(const char *label, std::span<const std::uint8_t> frame)
{
	hex_dump(label, frame);

	const mctpusb::DecodeResult f = mctpusb::decode(frame);
	if (f.error != mctpusb::DecodeError::Ok) {
		std::cout << "    (not an MCTP-over-USB frame)\n";
		return;
	}
	const mctppkt::ParseResult p = mctppkt::parse(f.payload);
	if (p.error != mctppkt::ParseError::Ok) {
		std::cout << "    (not a single-packet MCTP message)\n";
		return;
	}

	const mctppkt::Header &h = p.header;
	std::cout << std::format(
		"    MCTP ver={} dst={} src={} tag={} TO={}\n",
		static_cast<unsigned>(h.version),
		static_cast<unsigned>(h.dest), static_cast<unsigned>(h.src),
		static_cast<unsigned>(h.flagsSeqTag & mctppkt::TagMask),
		(h.flagsSeqTag & mctppkt::FlagTagOwner) ? 1 : 0);

	if (p.body.size() >= 3 &&
	    (p.body[0] & mctpctrl::MsgTypeMask) == mctpctrl::MsgTypeControl) {
		const bool rq = p.body[1] & mctpctrl::RqBit;
		std::cout << std::format(
			"    control {} cmd=0x{:02x} ({})\n",
			rq ? "request" : "response",
			static_cast<unsigned>(p.body[2]),
			mctpctrl::command_name(p.body[2]));
		if (!rq && p.body.size() >= 4)
			std::cout << std::format(
				"    completion=0x{:02x}\n",
				static_cast<unsigned>(p.body[3]));
	} else if (!p.body.empty()) {
		std::cout << std::format(
			"    message type 0x{:02x} (not control)\n",
			static_cast<unsigned>(p.body[0] & mctpctrl::MsgTypeMask));
	}
}

// Send one response frame on the IN endpoint. A bulk IN transfer ends for the
// host only on a short (or zero-length) packet; when the frame exactly fills
// whole max-size packets we must append a zero-length packet or the host's read
// never completes. (Mirrors the ZLP handling in Zephyr's mctp_usb binding.)
void write_in_frame(Context &ctx, std::span<const std::uint8_t> frame)
{
	if (!write_all(ctx.epIn.get(), frame.data(), frame.size(), "ep-IN"))
		return;
	if (ctx.mpsIn > 0 && !frame.empty() && (frame.size() % ctx.mpsIn) == 0) {
		::write(ctx.epIn.get(), frame.data(), 0); // zero-length packet
		std::cout << "    (+ZLP: frame fills whole packets)\n";
	}
}

void handle_out(Context &ctx)
{
	std::uint8_t buf[MaxFrame];
	ssize_t n = ::read(ctx.epOut.get(), buf, sizeof(buf));
	if (n < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return;
		std::cerr << std::format("read ep-OUT failed: {}\n",
					 std::strerror(errno));
		return;
	}
	if (n == 0)
		return; // zero-length OUT (e.g. host ZLP): nothing to do

	const std::span<const std::uint8_t> in(buf, static_cast<std::size_t>(n));

	if (ctx.mode == Mode::Echo) {
		hex_dump("ECHO RX", in);
		write_in_frame(ctx, in);
		hex_dump("ECHO TX", in);
		return;
	}

	// Optional core-binding path (Step B): the caller supplies its own
	// frame processor (e.g. mctpcore::FfsBinding) and writes the reply
	// directly to ep_in_fd.  The standalone trace path below is kept for
	// ffsd (debug tool) which passes no processor.
	if (ctx.frame_proc) {
		ctx.frame_proc(in, ctx.epIn.get(), ctx.mpsIn);
		return;
	}

	trace("RX", in);
	const std::vector<std::uint8_t> reply = mctpep::process(ctx.state, in);
	if (reply.empty()) {
		std::cout << "    -> no response\n";
		return;
	}
	trace("TX", reply);
	write_in_frame(ctx, reply);
	std::cout << std::format("    endpoint state: EID={}\n",
				 static_cast<unsigned>(ctx.state.eid));
}

void on_enable(Context &ctx)
{
	if (ctx.epOut.valid())
		return; // already enabled; ignore repeat

	ctx.epOut = Fd(::open((ctx.mount + "/ep1").c_str(), O_RDWR));
	ctx.epIn = Fd(::open((ctx.mount + "/ep2").c_str(), O_RDWR));
	if (!ctx.epOut.valid() || !ctx.epIn.valid()) {
		std::cerr << std::format("open ep1/ep2 failed: {}\n",
					 std::strerror(errno));
		ctx.epOut.close();
		ctx.epIn.close();
		return;
	}

	// Active-speed endpoint descriptor gives the real wMaxPacketSize, which
	// decides when a ZLP is needed. If unavailable, skip ZLP (mpsIn = 0).
	usb_endpoint_descriptor desc{};
	if (::ioctl(ctx.epIn.get(), FUNCTIONFS_ENDPOINT_DESC, &desc) == 0)
		ctx.mpsIn = le16toh(desc.wMaxPacketSize);
	else
		ctx.mpsIn = 0;

	std::cout << std::format("ENABLE: ep1/ep2 open, mode={}, IN mps={}\n",
				 ctx.mode == Mode::Echo ? "echo" : "mctp",
				 ctx.mpsIn);
}

void on_disable(Context &ctx)
{
	ctx.epOut.close();
	ctx.epIn.close();
	ctx.mpsIn = 0;
}

Ep0Status handle_ep0(int ep0, Context &ctx)
{
	usb_functionfs_event events[8];
	ssize_t rc = ::read(ep0, events, sizeof(events));
	if (rc < 0) {
		if (errno == EINTR)
			return Ep0Status::Continue;
		std::cerr << std::format("read ep0 failed: {}\n",
					 std::strerror(errno));
		return Ep0Status::Error;
	}
	if (rc == 0) {
		std::cerr << "ep0 closed\n";
		return Ep0Status::Closed;
	}

	const std::size_t count =
		static_cast<std::size_t>(rc) / sizeof(events[0]);
	for (std::size_t i = 0; i < count; ++i) {
		const std::uint8_t type = events[i].type;
		std::cout << std::format("event: {}\n", event_name(type));
		switch (type) {
		case FUNCTIONFS_ENABLE:
			on_enable(ctx);
			break;
		case FUNCTIONFS_DISABLE:
		case FUNCTIONFS_UNBIND:
			on_disable(ctx);
			break;
		case FUNCTIONFS_SETUP:
			// Bulk-only MCTP has no class control requests, and the
			// composite framework answers standard ones, so none is
			// expected here. Log only; if a host ever issues one its
			// control transfer will visibly time out.
			break;
		default:
			break;
		}
	}
	return Ep0Status::Continue;
}

} // namespace

int ffs_serve(const std::string &mount, Mode mode,
	      const std::function<bool()> &on_ready, FrameProcessor on_frame)
{
	Context ctx;
	ctx.mount = mount;
	ctx.mode = mode;
	ctx.frame_proc = std::move(on_frame);

	Fd ep0(::open((mount + "/ep0").c_str(), O_RDWR));
	if (!ep0.valid()) {
		std::cerr << std::format("open {}/ep0 failed: {}\n", mount,
					 std::strerror(errno));
		return 1;
	}

	// Descriptors first, then strings — this order makes the function ready
	// to be bound to a UDC.
	const ffsdesc::Descriptors descriptors = ffsdesc::make_descriptors();
	const ffsdesc::Strings strings = ffsdesc::make_strings();
	if (!write_all(ep0.get(), &descriptors, sizeof(descriptors),
		       "descriptors"))
		return 1;
	if (!write_all(ep0.get(), &strings, sizeof(strings), "strings"))
		return 1;

	std::cout << std::format(
		"descriptors written (mode={}); ready to bind a UDC\n",
		mode == Mode::Echo ? "echo" : "mctp");

	// Seam: the caller that owns the gadget enables the UDC here, now that
	// the descriptors are in place. Returning false aborts the daemon.
	if (on_ready && !on_ready()) {
		std::cerr << "on_ready failed; aborting\n";
		return 1;
	}

	// Event loop: ep0 always polled for lifecycle events; ep1 (OUT) added
	// once enabled. ep2 (IN) is only written to, so it is not polled.
	for (;;) {
		if (g_stop) {
			std::cout << "stop requested; exiting event loop\n";
			return 0;
		}

		struct pollfd fds[2];
		nfds_t nfds = 0;
		fds[nfds++] = { ep0.get(), POLLIN, 0 };
		const bool haveOut = ctx.epOut.valid();
		if (haveOut)
			fds[nfds++] = { ctx.epOut.get(), POLLIN, 0 };

		int pr = ::poll(fds, nfds, -1);
		if (pr < 0) {
			if (errno == EINTR)
				continue; // re-check g_stop at loop top
			std::cerr << std::format("poll failed: {}\n",
						 std::strerror(errno));
			return 1;
		}

		if (fds[0].revents & POLLIN) {
			switch (handle_ep0(ep0.get(), ctx)) {
			case Ep0Status::Continue:
				break;
			case Ep0Status::Closed:
				return 0;
			case Ep0Status::Error:
				return 1;
			}
		}

		// ep1 may have been closed by a DISABLE handled just above, so
		// re-check validity and that it was the polled fd.
		if (haveOut && nfds >= 2 && ctx.epOut.valid() &&
		    (fds[1].revents & POLLIN))
			handle_out(ctx);
	}
}

} // namespace ffsd
