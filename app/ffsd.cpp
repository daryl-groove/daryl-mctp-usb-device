// ffsd — script-driven FunctionFS MCTP-over-USB daemon (Milestones 2 / 3a / 3b).
//
// The gadget is created and bound to a UDC externally by scripts/gadget.sh;
// ffsd only owns the FunctionFS side: it writes the descriptors to ep0 and runs
// the MCTP event loop (see ffs_daemon.hpp). This keeps the step-by-step debug
// path (up-ffs -> ffsd -> bind) for bring-up. For a self-contained binary that
// also creates and binds the gadget in-process, see mctpusbd.
//
//   --echo : raw loopback (read OUT -> write IN), to prove bulk I/O moves bytes.
//   default: MCTP responder (read OUT -> mctpep::process -> write IN), answering
//            Set/Get EID, Get UUID, Get Message Type Support.

#include "ffs_daemon.hpp"

#include <format>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
	ffsd::Mode mode = ffsd::Mode::Mctp;
	std::string mount;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--echo")
			mode = ffsd::Mode::Echo;
		else if (mount.empty())
			mount = arg;
		else {
			std::cerr << std::format(
				"usage: {} [--echo] <functionfs-mountpoint>\n",
				argv[0]);
			return 2;
		}
	}
	if (mount.empty()) {
		std::cerr << std::format(
			"usage: {} [--echo] <functionfs-mountpoint>\n", argv[0]);
		return 2;
	}

	// No on_ready hook: gadget.sh binds the UDC after ffsd has written the
	// descriptors, and no signal handler is installed, so ffs_serve behaves
	// exactly as the previous monolithic ffsd did.
	return ffsd::ffs_serve(mount, mode);
}
