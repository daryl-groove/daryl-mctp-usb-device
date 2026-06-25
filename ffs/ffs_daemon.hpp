// ffs_daemon — the FunctionFS MCTP event loop, shared by both daemon binaries.
//
// ffs_serve() owns everything from the FunctionFS mount boundary inward: open
// ep0, hand the kernel the descriptor + strings block, then service ep0
// lifecycle events and the bulk data loop. It is deliberately agnostic about
// *how* the gadget was created and bound to a UDC:
//
//   - ffsd     : gadget created + UDC bound externally by scripts/gadget.sh.
//                Calls ffs_serve() with no on_ready hook.
//   - mctpusbd : gadget created via libusbg in-process; on_ready enables the
//                UDC once the descriptors are in place.
//
// The MCTP core (mctp_* + ffs_descriptors.hpp) is reused unchanged.

#pragma once

#include <csignal>
#include <cstdint>
#include <functional>
#include <poll.h>
#include <span>
#include <string>
#include <vector>

namespace ffsd {

// What the data loop does with each received frame.
enum class Mode { Mctp, Echo };

// Set by a SIGINT/SIGTERM handler (installed only by callers that want a
// graceful teardown, e.g. mctpusbd) to make ffs_serve() return from its poll
// loop. ffsd installs no handler, so it never sets this and behaves as before.
extern volatile std::sig_atomic_t g_stop;

// Optional frame processor injected by the caller for the Mctp data path.
// When set, ffs_serve calls it instead of the built-in standalone engine
// (mctpep::process) for each received USB frame.
//
// Arguments: (frame, ep_in_fd, mps_in)
//   frame     — raw USB bytes read from ep-OUT (host→device)
//   ep_in_fd  — open fd for ep-IN (device→host); processor writes its own reply
//   mps_in    — ep-IN wMaxPacketSize (for ZLP decisions; 0 = skip ZLP)
//
// The processor is responsible for writing any reply to ep_in_fd.
// It is NOT called for Echo mode (handled before this hook).
using FrameProcessor =
    std::function<void(std::span<const std::uint8_t>, int, int)>;

// Extension point for extra fds in the ffs_serve poll loop (Step C).
//
// collect  — called each iteration before poll(); append any extra pollfd
//            entries to the vector (fds[0]=ep0, fds[1]=ep1 are already there).
// dispatch — called each iteration after poll(); inspect revents and handle
//            events for the fds collect() added. The full fds span is passed so
//            the lambda can locate its entries by fd value.
//
// Both fields default to empty (no-op), so existing callers are unaffected.
struct PollExt {
	std::function<void(std::vector<pollfd> &)> collect;
	std::function<void(std::span<const pollfd>)> dispatch;
};

// Open <mount>/ep0, write the MCTP descriptors + strings, then invoke on_ready
// (the "descriptors written, now safe to enable the UDC" seam; return false to
// abort). After that, service ep0 lifecycle events and the bulk data loop until
// ep0 closes, an error occurs, or g_stop is set. Returns 0 on clean exit, 1 on
// error.
//
// on_frame: when set (Mctp mode only), overrides the built-in standalone MCTP
// engine.  ffsd passes nothing (keeps the standalone trace path); mctpusbd
// passes a FrameProcessor that routes through the libmctp-core binding.
//
// poll_ext: optional collect/dispatch hooks for extra fds (Step C DemuxServer).
int ffs_serve(const std::string &mount, Mode mode,
	      const std::function<bool()> &on_ready = {},
	      FrameProcessor on_frame = {},
	      PollExt poll_ext = {});

} // namespace ffsd
