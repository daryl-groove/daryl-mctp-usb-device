// mctpusbd — self-contained MCTP-over-USB gadget daemon.
//
// Unlike ffsd (which relies on scripts/gadget.sh for the configfs plumbing),
// mctpusbd folds the whole USB-gadget lifecycle into the binary via libusbg:
//
//   usbg_init -> create gadget (VID/PID/strings) -> create FunctionFS function
//   -> create config -> add function to config   [gadget exists, no UDC yet]
//   -> mount functionfs                           [ep0/epN files appear]
//   -> ffs_serve(..., on_ready = enable UDC)       [descriptors, THEN bind UDC]
//   -> SIGINT/SIGTERM -> disable UDC -> umount -> remove gadget -> cleanup
//
// The MCTP core and the FunctionFS event loop (ffs_daemon) are shared with
// ffsd, unchanged. Which UDC to bind is the only dev-vs-production difference,
// so it is a runtime parameter: --udc <name>, or auto-pick the first UDC.
//
// This is the binary intended for systemd on real hardware (e.g. aspeed-vhub).

#include "ffs_daemon.hpp"

#include <csignal>
#include <cstring>
#include <format>
#include <iostream>
#include <string>

#include <sys/mount.h>
#include <sys/stat.h>

#include <usbg/usbg.h>

namespace {

// Gadget identity — mirrors scripts/gadget.sh so mctpusbd and ffsd enumerate
// identically. The FunctionFS instance name doubles as the mount source.
constexpr const char *ConfigfsPath = "/sys/kernel/config";
constexpr const char *GadgetName = "mctp";
constexpr const char *FfsInstance = "mctp"; // -> functions/ffs.mctp; mount src
constexpr std::uint16_t Vid = 0x1d6b;	    // Linux Foundation test VID
constexpr std::uint16_t Pid = 0x0104;	    // Multifunction Composite test PID

bool usbg_ok(int ret, const char *what)
{
	if (ret == USBG_SUCCESS)
		return true;
	const auto err = static_cast<usbg_error>(ret);
	std::cerr << std::format("{}: {} ({})\n", what, usbg_strerror(err),
				 usbg_error_name(err));
	return false;
}

void on_signal(int)
{
	ffsd::g_stop = 1;
}

// Install SIGINT/SIGTERM handlers WITHOUT SA_RESTART, so a signal interrupts
// the blocking poll() in ffs_serve with EINTR and the loop notices g_stop.
void install_signal_handlers()
{
	struct sigaction sa = {};
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0; // no SA_RESTART
	sigaction(SIGINT, &sa, nullptr);
	sigaction(SIGTERM, &sa, nullptr);
}

struct Options {
	ffsd::Mode mode = ffsd::Mode::Mctp;
	std::string udc;		   // empty => auto-pick first UDC
	std::string mount = "/dev/ffs-mctp";
};

bool parse_args(int argc, char **argv, Options &opt)
{
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--echo") {
			opt.mode = ffsd::Mode::Echo;
		} else if (arg == "--udc") {
			if (++i >= argc) {
				std::cerr << "--udc needs a name\n";
				return false;
			}
			opt.udc = argv[i];
		} else if (!arg.empty() && arg[0] == '-') {
			std::cerr << std::format("unknown option: {}\n", arg);
			return false;
		} else {
			opt.mount = arg;
		}
	}
	return true;
}

} // namespace

int main(int argc, char **argv)
{
	Options opt;
	if (!parse_args(argc, argv, opt)) {
		std::cerr << std::format(
			"usage: {} [--echo] [--udc <name>] "
			"[functionfs-mountpoint]\n",
			argv[0]);
		return 2;
	}

	install_signal_handlers();

	usbg_state *s = nullptr;
	if (!usbg_ok(usbg_init(ConfigfsPath, &s), "usbg_init"))
		return 1;

	// Tracks how far bring-up got, so teardown only undoes what was done.
	bool mounted = false;
	usbg_gadget *g = nullptr;
	int rc = 1;

	struct usbg_gadget_attrs gAttrs = {};
	gAttrs.bcdUSB = 0x0200;
	gAttrs.bDeviceClass = 0x00; // per-interface
	gAttrs.bMaxPacketSize0 = 64;
	gAttrs.idVendor = Vid;
	gAttrs.idProduct = Pid;
	gAttrs.bcdDevice = 0x0100;

	struct usbg_gadget_strs gStrs = {};
	gStrs.manufacturer = const_cast<char *>("daryl");
	gStrs.product = const_cast<char *>("MCTP FFS device");
	gStrs.serial = const_cast<char *>("0123456789");

	struct usbg_config_strs cStrs = {};
	cStrs.configuration = const_cast<char *>("config 1");

	usbg_function *f = nullptr;
	usbg_config *c = nullptr;

	if (!usbg_ok(usbg_create_gadget(s, GadgetName, &gAttrs, &gStrs, &g),
		     "create_gadget"))
		goto cleanup;
	if (!usbg_ok(usbg_create_function(g, USBG_F_FFS, FfsInstance, nullptr,
					  &f),
		     "create_function(ffs)"))
		goto cleanup;
	if (!usbg_ok(usbg_create_config(g, 1, nullptr, nullptr, &cStrs, &c),
		     "create_config"))
		goto cleanup;
	if (!usbg_ok(usbg_add_config_function(c, "ffs.mctp", f),
		     "add_config_function"))
		goto cleanup;

	// Mount FunctionFS; the source name is the function instance name. This
	// makes ep0 (and, after enable, epN) appear under the mountpoint.
	::mkdir(opt.mount.c_str(), 0755); // ignore EEXIST
	if (::mount(FfsInstance, opt.mount.c_str(), "functionfs", 0, nullptr) !=
	    0) {
		std::cerr << std::format("mount functionfs at {} failed: {}\n",
					 opt.mount, std::strerror(errno));
		goto cleanup;
	}
	mounted = true;

	std::cout << std::format("gadget '{}' created; functionfs mounted at {}\n",
				 GadgetName, opt.mount);

	// Run the shared event loop. on_ready enables the UDC once ffs_serve has
	// written the descriptors — the order the gadget framework requires.
	rc = ffsd::ffs_serve(opt.mount, opt.mode, [&]() -> bool {
		usbg_udc *udc = nullptr;
		if (!opt.udc.empty()) {
			udc = usbg_get_udc(s, opt.udc.c_str());
			if (!udc) {
				std::cerr << std::format(
					"UDC '{}' not found\n", opt.udc);
				return false;
			}
		}
		if (!usbg_ok(usbg_enable_gadget(g, udc), "enable_gadget"))
			return false;
		std::cout << std::format(
			"UDC bound ({}); enumerating\n",
			opt.udc.empty() ? "auto" : opt.udc.c_str());
		return true;
	});

cleanup:
	// Undo in reverse: clear the UDC, unmount, then remove the gadget.
	if (g)
		usbg_disable_gadget(g); // no-op if never enabled
	if (mounted && ::umount(opt.mount.c_str()) != 0)
		std::cerr << std::format("umount {} failed: {}\n", opt.mount,
					 std::strerror(errno));
	if (g)
		usbg_rm_gadget(g, USBG_RM_RECURSE);
	usbg_cleanup(s);
	return rc;
}
