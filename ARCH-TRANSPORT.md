# Transport Architecture — MCTP USB Device Side

> Architectural analysis of the MCTP transport layer options for the device (gadget)
> side. Written 2026-06-25 after the demux-socket approach (Step C) was found to
> conflict with the dual-role (USB host + USB device) requirement. See
> [`PROGRESS.md`](./PROGRESS.md) session log 2026-06-25 for the decision trail.

---

## 1. Why the current demux socket path has a ceiling

### mctp-demux-daemon is host-only

`libmctp/usb.c` uses `libusb-1.0` — it opens a USB device handle, polls for hotplug
events, and does bulk transfers to a downstream device. This is exclusively the **USB
host controller** perspective. The `mctp-demux-daemon` USB binding cannot run on the
device (gadget) side.

Our `mctpusbd` fills the gap with FunctionFS — making the system *present itself* as a
USB device via the USB device controller (UDC). These are different kernel subsystems,
different hardware roles, opposite ends of the USB cable.

### pldmd has one transport backend at a time

pldmd (phosphor-pldm) supports two transport backends, selected at compile time:

- `transport-af-mctp` — connects to the **kernel MCTP stack** via `AF_MCTP` socket
- `transport-mctp-demux` — connects to a **mctp-demux-daemon** Unix socket

A single pldmd process can only use one. This creates a conflict when the same machine
needs to be:

- **USB device** (being managed by a host) → our plan: `transport-mctp-demux` → mctpusbd socket
- **USB host** (managing downstream devices) → natural path: `transport-af-mctp` → kernel MCTP stack

Both roles on the same machine, one pldmd, two incompatible backends → **they collide**.

---

## 2. Why the kernel MCTP stack resolves it

Linux 5.15+ implements MCTP as a multi-interface routing layer, analogous to IP routing:

```
kernel MCTP routing layer
  ├── mctp0   USB host side    (mctp-usb.ko — merged Linux 6.15)
  ├── mctp1   I2C side         (mctp-i2c.ko)
  └── mctp2   USB device side  (← does NOT exist upstream yet; this is the gap)
```

pldmd via `AF_MCTP` is transport-agnostic — it sends/receives MCTP messages through the
kernel socket API and the kernel routes across all registered interfaces. If both USB host
and USB device sides register as kernel MCTP interfaces, pldmd sees one unified MCTP
network and the conflict disappears.

The kernel-side USB host driver (`mctp-usb.ko`) was merged in Linux 6.15. There is no
upstream gadget/device-side counterpart — that is the gap this project addresses.

---

## 3. Options

### Option A1 — PTY + `mctp-serial` line discipline (no kernel module)

#### Three concepts

**kernel MCTP net devices**: each physical transport registers a `ARPHRD_MCTP` net device;
pldmd uses `AF_MCTP` and the kernel routes across all of them.

**`mctp-serial` line discipline** (`drivers/net/mctp/mctp-serial.c`, kernel 5.15+): a TTY
line discipline that frames/deframes MCTP packets per DSP0253 (byte-stream format). Can be
attached to *any* tty — including a PTY slave. "Serial" refers to the framing format, not
a physical UART. When attached, the kernel creates a `mctpserial0` net device automatically.

**PTY (pseudo-terminal)**: a purely-software kernel data structure. `open(/dev/ptmx)` gives
a master fd; a slave tty (`/dev/pts/N`) appears. Data written to master appears at slave and
vice versa. To the kernel, the slave is an ordinary tty — any line discipline can be attached.
Analogy: TUN/TAP lets userspace inject IP packets into the kernel IP stack; PTY+mctp-serial
lets userspace inject MCTP frames into the kernel MCTP stack.

#### Full data flow

```
FunctionFS (UDC)        mctpusbd userspace              kernel
────────────────        ──────────────────         ─────────────────────
ep-OUT bulk read
[4B USB hdr][MCTP frame]
        │
        ▼
   ffs_daemon.cpp
   (unchanged)
        │ strip USB header
        │ wrap in DSP0253 framing:
        │   [0x7E][len hi][len lo][MCTP frame][FCS 2B][0x7E]
        ▼
  write(pty_master_fd) ────────────────────► PTY slave tty
                                              │
                                   N_MCTP line discipline
                                   (attached via ioctl TIOCSETD)
                                              │ DSP0253 deframe
                                              ▼
                                        mctpserial0 net device
                                              │
                                   kernel MCTP routing layer
                                              │
                                              ▼
                                   pldmd via AF_MCTP socket
                                   (transport-af-mctp)

TX path is fully symmetric (pldmd → AF_MCTP → kernel → mctpserial0 →
DSP0253 frame → PTY slave → PTY master → mctpusbd reads → add USB hdr
→ ffs write ep-IN → USB wire).
```

Attaching the line discipline:
```c
int n = N_MCTP;   /* 25 in Linux 5.15+ */
ioctl(pty_slave_fd, TIOCSETD, &n);
```

**Pros**: no kernel module to write; validates the full AF_MCTP + pldmd path quickly.  
**Cons**: requires DSP0253 framing code in mctpusbd; PTY management adds some complexity;
depends on `CONFIG_MCTP_SERIAL` being enabled on the target.

---

### Option A2 — small kernel module (cleanest userspace path)

A minimal kernel module registers a virtual MCTP net device and exposes a character device
for userspace to exchange raw MCTP frames:

```
FunctionFS ←→ mctpusbd ←→ /dev/mctp-gadget ←→ mctp-gadget.ko ←→ kernel MCTP stack
```

mctpusbd only strips/adds the 4-byte USB header; no format translation needed. The kernel
module is ~200–300 lines.

**Pros**: cleanest design; mctpusbd logic is minimal; no DSP0253 framing overhead.  
**Cons**: requires writing a kernel module.

---

### Option B — full kernel MCTP gadget driver

Move FunctionFS handling entirely into the kernel. A `mctp-gadget.ko` driver registers as
both a USB gadget function and a kernel MCTP net device. mctpusbd userspace daemon is no
longer needed (or is minimal configuration only).

This is the correct long-term upstream direction.

**Pros**: pldmd is completely unaware of the transport; correct architecture for upstream.  
**Cons**: significant kernel development effort; userspace FunctionFS daemon is entirely
replaced.

---

## 4. A1 → A2 evolution: not a rewrite

A1 and A2 share the same overall structure. Moving from A1 to A2 is a targeted replacement:

| Component | A1 | A2 | Fate |
|---|---|---|---|
| `ffs_daemon.cpp` (USB endpoint I/O) | ✓ | ✓ | **keep as-is** |
| USB header strip/add | ✓ | ✓ | **keep as-is** |
| DSP0253 serial framing encode/decode | ✓ | ✗ | remove (A2 doesn't need it) |
| PTY create + line discipline attach | ✓ | ✗ | remove |
| chardev read/write (`/dev/mctp-gadget`) | ✗ | ✓ | add (simpler than PTY path) |
| kernel module (`mctp-gadget.ko`) | ✗ | ✓ | add (independent new code) |

A1 discards ~100–200 lines of framing + PTY code. A2 adds a kernel module as new,
independent code. `ffs_daemon.cpp` and the USB packet handling are reused verbatim.

A1 is therefore a useful stepping stone: it validates the full pldmd ↔ AF_MCTP ↔ kernel ↔
userspace data path without requiring kernel module development, and the investment is
largely preserved when moving to A2.

---

## 5. Impact on current codebase

```
Component             Current path      Option A fate
────────────────────  ──────────────    ─────────────────────────────
ffs_daemon.cpp        keep              keep (unchanged in all options)
mctp_core_binding     libmctp engine    remove (kernel handles MCTP)
mctp_demux_server     Unix socket       remove (kernel handles demux)
mctpusbd.cpp          orchestrates all  rework (bridge logic replaces above)
```

`mctp_core_binding` and `mctp_demux_server` are the discard scope. Any new features
built on top of them accumulate waste.

---

## 6. Status (2026-06-25)

- Step C (demux socket: `mctp_core_binding` + `mctp_demux_server`) is implemented and
  unit-tested (28/28 pass) but **not yet hardware-verified**.
- Decision pending: discard Step C C++ changes (commit `e618f40e` and unstaged) and
  pivot to Option A, or continue and accept the architectural debt.
- A1 feasibility requires confirming `CONFIG_MCTP_SERIAL` on the aspeed-2700 kernel.
- A1 vs A2 choice: A1 first (no kernel module, faster validation), then A2 naturally.
