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
#include <functional>
#include <string>

namespace ffsd {

// What the data loop does with each received frame.
enum class Mode { Mctp, Echo };

// Set by a SIGINT/SIGTERM handler (installed only by callers that want a
// graceful teardown, e.g. mctpusbd) to make ffs_serve() return from its poll
// loop. ffsd installs no handler, so it never sets this and behaves as before.
extern volatile std::sig_atomic_t g_stop;

// Open <mount>/ep0, write the MCTP descriptors + strings, then invoke on_ready
// (the "descriptors written, now safe to enable the UDC" seam; return false to
// abort). After that, service ep0 lifecycle events and the bulk data loop until
// ep0 closes, an error occurs, or g_stop is set. Returns 0 on clean exit, 1 on
// error.
int ffs_serve(const std::string &mount, Mode mode,
	      const std::function<bool()> &on_ready = {});

} // namespace ffsd
