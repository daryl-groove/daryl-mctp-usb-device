# MCTP over USB — Device (Gadget) Side Strategy

> Handoff / context document for the work of implementing a **userspace
> MCTP-over-USB device (peripheral)** on OpenBMC. It is written so that a fresh
> session (or new contributor) can read it and start working without re-deriving
> the prior analysis.
>
> **Living progress is tracked separately in [`PROGRESS.md`](./PROGRESS.md).**
> This file = stable strategy; that file = current status / checklist / log.

---

## 0. One-line goal

Write a userspace daemon using the **Linux USB gadget subsystem (FunctionFS)**
so the machine (a QEMU guest during development) can present itself as an **MCTP
USB device**, and ultimately complete MCTP control-protocol communication with an
MCTP host (initially NVIDIA libmctp). **Make it work first, add flexibility later.**

Development assumption: **all required kernel configs are enabled**
(USB_GADGET / DUMMY_HCD / CONFIGFS / FunctionFS).

---

## 1. Why (background)

- Our OpenBMC embedded device must be **both a USB host and a USB device**.
- **Host side already works**: using NVIDIA libmctp's `mctp-demux-daemon` (libusb)
  + `mctp-ctrl`, we have verified MCTP communication with an MCTP device.
- **Device side is the gap**: libmctp's USB binding (`usb.c`) is **host-only
  (libusb)**; it cannot make the machine *become* a USB peripheral. Acting as a
  device requires the **USB gadget subsystem + a UDC**.
- We started with a **deliberate userspace-first approach** (FunctionFS + demux
  socket) to learn feasibility before committing to kernel development.
- **2026-06-25 update — direction shifting toward kernel MCTP stack.** Analysis
  revealed the demux socket path conflicts with the dual-role requirement: one
  pldmd instance cannot simultaneously use `transport-af-mctp` (USB host side,
  kernel stack) and `transport-mctp-demux` (USB device side, our socket). The
  kernel MCTP stack's multi-interface routing is the correct unifier. See
  [`ARCH-TRANSPORT.md`](./ARCH-TRANSPORT.md) for the full analysis and Options
  A1 / A2 / B. Decision on whether to discard the demux socket work (Step C) is
  pending.

---

## 2. Development environment architecture (key concept)

### Use `dummy_hcd` for a host+device loopback inside a single kernel

`dummy_hcd` is a kernel module that creates **both a virtual UDC and a virtual host
controller within the same kernel**, wired together. So a **single QEMU guest** can
play both roles:

```
            single QEMU guest / single kernel
   ┌──────────────────────────────────────────────┐
   │  our gadget daemon ─► dummy_udc (device side)   │
   │                          ║  (software loopback) │
   │  lsusb / libusb / host  ◄─ virtual HCD (host)   │
   └──────────────────────────────────────────────┘
```

- **device side**: the gadget daemon runs on `dummy_udc`
- **host side**: the same guest's USB subsystem (`lsusb` / libusb / libmctp)
- Both ends live **inside the same guest**; the outer machine (WSL / QEMU host)
  is NOT involved in USB enumeration.
- ⚠️ `dummy_hcd` is a **single-kernel loopback and does NOT cross the VM boundary**.
  Do not expect "device in the guest, scanned by the outer host" — that would need
  usbip/usbredir and is unnecessary for now.

### Real deployment (later)

Development uses `dummy_udc`; on real hardware (Aspeed BMC) bind the gadget to the
**`aspeed-vhub`** UDC instead. The gadget logic stays essentially the same.

---

## 3. Milestone plan (each step yields an independently verifiable artifact)

| #  | Milestone | Proves | Custom code |
|----|-----------|--------|-------------|
| 1a | `dummy_hcd` + configfs bound to a **standard function** (acm / mass_storage) | gadget plumbing works, `lsusb` sees it | none |
| 1b | **FunctionFS** minimal custom-interface daemon | we can enumerate "our own" device via FunctionFS | small |
| 2  | descriptors set to **interface class 0x14 + bulk IN/OUT** | host recognizes it as an MCTP device (`lsusb -v`) | small |
| 3a | bulk endpoint **raw echo** ↔ libusb | bytes move both ways (FunctionFS I/O + USB transfers) | medium |
| 3b | device implements **MCTP-USB framing + control responder** | real MCTP comms (Get/Set EID get answered) | large |

> Terminology corrections:
> - Milestone 2 is "**get the descriptors right so it is recognized**"; **no MCTP
>   traffic flows yet**.
> - Milestone 3 "communication" does **not** happen just by pointing libmctp at it —
>   **the device side must implement the MCTP responder itself** (effectively a
>   C/C++ version of an mctp-estack endpoint). This is the bulk of the work.

---

## 4. Key technical facts (already verified — avoid re-investigating)

- **MCTP interface identification**: `bInterfaceClass = 0x14` (libmctp `usb.c`'s
  `MCTP_CLASS_ID` matches exactly this), plus 2 **bulk endpoints (IN + OUT)**;
  subclass/protocol per **DSP0283 (MCTP over USB)**.
- **MCTP-USB framing**: each USB transfer carries an MCTP-over-USB header (DSP0283).
  libmctp `usb.c` already has encode/decode that can be reused (that part is
  role-neutral, not host/device specific).
- **libmctp core is role-neutral**: `core.c` `mctp_rx`, under `ROUTE_ENDPOINT`,
  hands incoming control **requests** to a callback; but there is **no built-in
  control responder** (only transport commands 0xF0–0xFF have a hook, and the USB
  binding doesn't set `control_rx`). → Set/Get EID etc. must be answered by us.
- **Control commands the device must support** (matching mctp-dev / a simple endpoint):
  - `0x01` Set Endpoint ID, `0x02` Get Endpoint ID, `0x03` Get Endpoint UUID,
    `0x05` Get Message Type Support.
  - **Not required**: `0x04` Get MCTP Version, `0x06+` Allocate EIDs / Get Routing
    Table (those are bridge / bus-owner concerns).
- **Host-side claim gotcha**: when using libmctp (libusb) as host, if the kernel
  `mctp-usb` host driver binds the class-0x14 interface first, libusb cannot claim
  it. For same-kernel testing, ensure the kernel mctp-usb host driver is **not
  loaded** (or use `libusb_detach_kernel_driver`).

---

## 5. Reference implementations and resources (paths)

- **NVIDIA libmctp (host side, used in our tests)**: `/home/daryl/work/project/dep/libmctp`
  - `usb.c` — USB binding (libusb host); reusable MCTP-USB header encode/decode
  - `core.c` — role-neutral MCTP engine (`mctp_rx` / ROUTE_ENDPOINT)
- **CodeConstruct mctp-dev (Rust, device emulator, responder reference)**:
  `/home/daryl/work/project/dep/CodeConstruct/mctp-dev`
  - It is a **usbredir emulator** (for QEMU), not a real gadget; but its **control
    responder behavior** (provided by the `mctp-estack` crate) is the template our
    device side should match.
- **CodeConstruct mctp / mctpd (correct bus-owner enumeration reference)**:
  `/home/daryl/work/project/dep/CodeConstruct/mctp` (`src/mctpd.c`, `docs/mctpd.md`).
  Note: `mctpd` is the **kernel-MCTP-stack** bus owner (AF_MCTP + netlink, D-Bus),
  a different world from the libmctp `mctp-demux-daemon`/`mctp-ctrl` we test with.
- **Our host-side CLI tester (already working)**:
  `/home/daryl/work/project/dep/libmctp/daryl-mctp-usb-tester` (C++20, connects to
  the demux socket and sends the 4 control commands)
- **Usage docs**:
  - `/home/daryl/work/project/dep/libmctp/mctp-usb-quickstart.txt` (quick start)
  - `/home/daryl/work/project/dep/libmctp/mctp-usb-demux-guide.txt` (detailed, with
    source line refs)

---

## 6. Future improvement: replace gadget.sh with libusbgx

`/home/daryl/work/project/dep/libusbgx` is a mature C library that wraps all
configfs operations (create gadget, bind function, enable UDC, teardown) into a
structured API.

**What it replaces**: everything `gadget.sh` does today — the manual `mkdir`,
`echo VID/PID`, `ln -sf`, `echo UDC`, `rmdir` sequence.

**What it does NOT replace**: `ffsd.cpp` is unaffected. libusbgx stops at the
FunctionFS mount boundary; writing descriptors to ep0, the enumeration event
loop, and MCTP protocol handling remain our responsibility.

**Key benefit**: gadget lifecycle (create → mount → bind → teardown) can be
integrated directly into the daemon binary, eliminating the shell script
dependency and enabling structured error handling and UDC auto-discovery.

**When to migrate**: when packaging for real hardware deployment (e.g. as a
systemd unit on Aspeed BMC). The current shell script approach is fine through
the full milestone chain on dummy_hcd.

**Confirmed approach (2026-06-23), now the immediate next phase** — full plan and
verified prerequisites in [`PROGRESS.md`](./PROGRESS.md) "Next phase":
- libusbgx is already in the OpenBMC SDK sysroot (links via meson `dependency()`).
- Two binaries sharing the core: `ffsd` (test, script-driven) and `mctpusbd`
  (full, libusbg lifecycle), both calling a shared `ffs_serve(mount, mode,
  on_ready)`. The MCTP core (`mctp_*` + `ffs_descriptors.hpp`) is untouched.
- **UDC is a runtime parameter** (`usbg_enable_gadget(g, udc)`, NULL = auto-pick
  first): the dev (`dummy_udc.0`) vs real-HW (`aspeed-vhub`) difference collapses
  to an optional `--udc` flag, so one binary serves both.

---

## 8. Already done (prior to this strategy)

- Brought up the host with `mctp-demux-daemon usb port_path=1-1 mode=0 -v`.
- Used `mctp-ctrl -t 3 -w 1-1 -e 9 -m 0 -b "00" -s "..."` to send Get/Set EID,
  Get UUID, Get Message Type Support to mctp-dev — **verified working**.
- Wrote `daryl-mctp-usb-tester` (a long-running C++ daemon, CLI thread + poll loop)
  as a replacement for mctp-ctrl for testing.

> In short: **the entire host-side chain is verified.** This strategy covers the
> **device side**.

---

## 9. Next step (where a fresh session begins)

Start at **Milestone 1a → 1b**:

1. In the QEMU guest (kernel configs enabled) `modprobe dummy_hcd`; `ls /sys/class/udc/`
   should show `dummy_udc.0`.
2. First bind a standard function via configfs (e.g. acm) and verify plumbing with
   `lsusb`.
3. Then write a FunctionFS skeleton daemon (enumerate-only first, optional echo) and
   confirm `lsusb` sees "our own" device.
4. Put code in this folder `daryl-mctp-usb-device/` with its own meson.build (use
   C++20, consistent with the tester).

After 1b, proceed to Milestone 2 (descriptors → class 0x14 + bulk EPs) → 3a (echo)
→ 3b (MCTP responder). **Update [`PROGRESS.md`](./PROGRESS.md) as you go.**
