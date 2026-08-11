# Progress — MCTP USB Device

> Living status tracker. Strategy/context lives in [`STRATEGY.md`](./STRATEGY.md).
> **Update this file as work proceeds** (flip checkboxes, append to the log, record
> decisions and blockers). Keep entries short and factual.

Status legend: `[ ]` not started · `[~]` in progress · `[x]` done · `[!]` blocked

---

## Current focus

- **Milestone:** M1b → M3b **verified end-to-end in the guest** — the device
  enumerates as interface class 0x14 + 2 bulk EPs and answers all four control
  commands (Set/Get EID, Get UUID, Get Msg Type) via host `mctp-demux-daemon` +
  `mctp-ctrl`. `ffsd` now prints a verbose RX/TX trace for the demo.
- **Test environment: complete.** native + cross build (ffsd + mctpusbd,
  5/5 unit tests on qemu-aarch64); dummy_hcd single-kernel loopback; host tooling
  (mctp-demux-daemon + mctp-ctrl/tester); ffsd+gadget.sh verified end-to-end on the
  earlier qemuarm64; **mctpusbd verified end-to-end on aspeed-2700** — enumerates as
  `1d6b:0104`, all four control commands answered (session log 2026-06-24). Step-5 closed.

- **Option A1 status (2026-06-25):**
  - ~~**P0 — close step-5**~~ **DONE** (2026-06-24)
  - ~~**Step A** — message type router~~ **DONE** (2026-06-24, `bc47414`)
  - ~~**Step B** (libmctp-core binding) + **Step C** (demux socket server)~~
    **DISCARDED** (2026-06-25, `0753217`) — wrong direction; see
    [`ARCH-TRANSPORT.md`](./ARCH-TRANSPORT.md)
  - ~~**Option A1 implementation**~~ **DONE** (2026-06-25) — C17 rewrite; PTY + N_MCTP
    bridge implemented in `ffs_daemon.c` (`FFS_MODE_PTY_BRIDGE`). New files:
    `mctp/mctp_serial_frame.{h,c}` (DSP0253 framing), `ffs/ffs_pty.{h,c}` (PTY open
    + N_MCTP attach). 18/18 unit tests pass.

- **Next actions — bridge interface refactor (A1/A2 compile-flag switching)**

  **Goal:** extract the bridge backend from `ffs_daemon.c` into a stable interface
  (`ffs_bridge.h`) so that A1 (PTY + mctp-serial) and A2 (kernel module chardev) can
  coexist in the same source tree and be selected at compile time via a meson option.
  A1 is **not** a throwaway — it is a permanent backend for environments without the
  kernel module. Design fully specified in [`DESIGN-BRIDGE.md`](./DESIGN-BRIDGE.md).

  - [ ] Add `meson_options.txt` with `option('bridge', choices: ['a1', 'a2'], value: 'a1')`
  - [ ] Create `ffs/ffs_bridge.h` — declare 4-function interface:
        `ffs_bridge_open`, `ffs_bridge_handle_out`, `ffs_bridge_handle_in`, `ffs_bridge_close`
  - [ ] Create `ffs/ffs_bridge_a1.c` — move PTY bridge logic out of `ffs_daemon.c`
        (state machine, DSP0253 encode/decode, PTY open); implement the 4 functions
  - [ ] Refactor `ffs_daemon.c` to call through `ffs_bridge.h`; rename
        `FFS_MODE_PTY_BRIDGE` → `FFS_MODE_BRIDGE`; zero `#ifdef` in `ffs_daemon.c`
  - [ ] Update `meson.build` to select `ffs_bridge_a1.c` vs `ffs_bridge_a2.c` based on option
  - [ ] Gate: `meson setup build -Dbridge=a1 && ninja && meson test` — 18/18 pass,
        behaviour identical to current A1

  See [`DESIGN-BRIDGE.md §7–§8`](./DESIGN-BRIDGE.md) for step-by-step refactor guide.

- **Future: A2 kernel module** (after bridge interface is in place)
  - [ ] Write `mctp_gadget.ko` (~300 lines) — ARPHRD_MCTP net device + char device
  - [ ] Write `ffs/ffs_bridge_a2.c` (~50 lines) — open `/dev/mctp-gadget`, raw MCTP r/w
  - [ ] `meson setup build-a2 -Dbridge=a2` selects A2 backend; A1 source unchanged
  - See [`DESIGN-BRIDGE.md §9`](./DESIGN-BRIDGE.md) for kernel module skeleton

  Architecture and rationale: [`ARCH-TRANSPORT.md`](./ARCH-TRANSPORT.md),
  [`DESIGN-BRIDGE.md`](./DESIGN-BRIDGE.md)

## Next phase: libusbgx migration — steps 1–4 DONE, step 5 (guest verify) pending

Approved 2026-06-23. Goal: fold the USB-gadget lifecycle into the daemon binary
(replacing `scripts/gadget.sh`) via **libusbgx**, **without touching the MCTP core**.

Verified prerequisites (checked this session):
- libusbgx is already in the OpenBMC SDK sysroot (`libusbgx.so`, `usbg/usbg.h`,
  `libusbgx.pc`) → meson `dependency('libusbgx')` links under cross-build; no need
  to build it ourselves. Native WSL may lack it → make the dep **optional**.
- Reference: libusbgx `examples/gadget-ffs.c`. libusbg does `usbg_init →
  create_gadget(VID/PID/strings) → create_function(USBG_F_FFS) → create_config →
  add_config_function`; the daemon still does ① mount functionfs, ② write ep0
  descriptors, ③ enable UDC (`usbg_enable_gadget`). Matches STRATEGY §6 exactly.

Design — **two binaries, shared core** (`mctp_*` + `ffs_descriptors.hpp` UNCHANGED):
- Extract the ffsd event loop into `ffs_daemon.{hpp,cpp}`:
  `int ffs_serve(mount, mode, on_ready = {})` = open ep0 → write descriptors →
  `on_ready()` → poll loop. `on_ready` is the seam for "descriptors written, now
  safe to enable UDC".
- `ffsd` (test): thin main → `ffs_serve(mount, mode)`. functionfs mounted by
  `gadget.sh`, UDC enabled by `gadget.sh bind`. Keeps the step-by-step debug path.
- `mctpusbd` (full): thin main → libusbg create gadget/ffs/config → mount
  functionfs → `ffs_serve(mount, mode, enable-UDC lambda)` → SIGINT/SIGTERM →
  disable UDC + `usbg_cleanup`. Self-contained binary for systemd on real HW.

UDC as a runtime parameter (dev vs production = same binary):
- Only dev/prod difference is which UDC you bind. `usbg_enable_gadget(g, udc)`
  takes a UDC name; NULL = auto-pick first UDC.
- `mctpusbd [--udc <name>]`: default = auto-pick first `/sys/class/udc` entry
  (covers `dummy_udc.0` in dev and a lone real UDC); `--udc aspeed-vhub-...` for
  explicit selection. One binary serves dummy_hcd testing AND aspeed-vhub deploy.

Steps for the implementing session:
1. [x] Extract `ffs_daemon.{hpp,cpp}` from `ffsd.cpp`; reduce `ffsd.cpp` to a thin
   main. Done — `ffs_serve(mount, mode, on_ready={})` holds the descriptor write +
   poll loop; `ffsd.cpp` is arg-parse + one call. Behaviour unchanged (no signal
   handler, no on_ready). Native + cross build clean, 5/5 tests pass.
2. [x] Add `mctpusbd.cpp` (libusbg create gadget/ffs/config → `mount(2)` functionfs
   → `ffs_serve` with enable-UDC `on_ready` → SIGINT/SIGTERM teardown: disable UDC
   + umount + `usbg_rm_gadget(RECURSE)` + `usbg_cleanup`). `--udc <name>`, default
   NULL = auto-pick first UDC.
3. [x] meson: `dependency('libusbgx', required: false)` → `mctpusbd` built only when
   present. Native (WSL, no libusbgx) builds ffsd + tests only; cross (SDK, libusbgx
   0.2.0) builds both. `mctpusbd` confirmed `NEEDED libusbgx.so.3`.
4. [x] RUNBOOK §5 added (`mctpusbd` one-shot usage, no gadget.sh); PROGRESS updated.
5. [x] Guest-verify `mctpusbd` enumerates + answers the 4 commands, same as gadget.sh
   + ffsd. On **aspeed-2700** it first failed at `create_function(ffs): BUSY` until
   `CONFIG_USB_FUNCTIONFS` (legacy g_ffs) was removed from the kernel — see session log
   2026-06-24. Confirmed: `lsusb` shows `1d6b:0104 daryl MCTP FFS device`; all four
   control commands (Set EID=9, Get EID→9, Get UUID, Get Msg Type) answered correctly.

---

## Future (planned, NOT started): proprietary data interfaces over MCTP

> **⚠️ 2026-06-25 — architecture direction changed.** This section describes the original
> Path A (demux socket) design. That direction was **discarded** (commit `0753217`) after
> architectural analysis revealed a dual-role conflict with pldmd. The PLDM integration goal
> (pldmd as a separate process receiving type 0x01 messages) remains valid; only the
> transport changes: instead of a demux socket, the new path is **Option A1** (PTY +
> `mctp-serial` → kernel MCTP stack → pldmd via `AF_MCTP`). The demux shelf-life problem
> disappears entirely. See [`ARCH-TRANSPORT.md`](./ARCH-TRANSPORT.md).
>
> Historical analysis below is kept because the PLDM protocol details and wire-format notes
> remain accurate. Treat demux-socket references as superseded.

Context: real use exposes our own system data (e.g. D-Bus inventory/sensors) over
MCTP — potentially *lots* of commands/content. This is the plan for making that
easy to add without churning the core. **Deliberately deferred** behind two gates
(see ordering). Discussed 2026-06-24; recorded, not implemented.

Key domain point — **our data does NOT go in control commands.** MCTP control
(msg type `0x00`) is MCTP self-management (EID/discovery). System data belongs in
an **application message type**: PLDM (`0x01`, the DMTF standard for
sensors/FRU/inventory — maps ~1:1 to what OpenBMC already models on D-Bus; pick for
standard-host interop) or **vendor-defined** (`0x7E` PCI / `0x7F` IANA; pick for
pure-proprietary). Either way it's "another message type alongside control".

Current limitation: `mctpep::process` calls `mctpctrl::handle` unconditionally, and
`handle` drops anything whose msg-type byte != `0x00`. So there is **no message-type
router yet** — adding an app interface today means hacking `process` or mis-filing it
under control. The frame/packet layers are already msg-type-agnostic (zero change
needed there), so only the dispatch seam is missing.

Target shape — **message-type handler registry (open/closed)**:
- `MessageHandler` interface: `handle(body) -> response body` (body from the
  msg-type byte; empty = no reply). A `Router` maps `body[0] & 0x7f -> handler`.
- `process` becomes `parse -> router.dispatch(body) -> build_response -> encode`.
- The current `mctpctrl::handle` is **demoted to one handler** registered at type
  `0x00` (its logic/tests unchanged). New interface = new handler file + one
  `router.on(type, h)` line; **nothing else changes** → linear to scale to "lots".
- Within a type, recurse the same `command -> fn` table pattern (not a giant switch).
- Generalises the state coupling: each handler owns its own state/deps (control owns
  eid/uuid; a sensor handler owns its data source). `process` stops threading the
  control-specific `EndpointState`.

Discipline — **D-Bus must NOT enter the `mctp/` core.** Inject the data source into
the handler (`SensorHandler(DataSource&)`); core stays pure + unit-testable with a
mock. `sdbusplus` lives in the handler layer only.

The real cost driver — **multi-packet.** "Lots of content" overruns the single
packet the standalone `mctp_packet` allows (BTU ≈ 64 B), so it forces the
**standalone-vs-libmctp-core decision the decisions log deferred** (its named trigger
was exactly "carries application protocols / large payloads"). libmctp-core gives
reassembly/fragmentation/type-demux for free + shared wire structs with the host; its
`mctp_set_rx_all` callback is where the Router would hang. The registry design above
is **orthogonal to the engine** (it sits on top either way), so it survives that
choice — but the *wiring point* depends on it.

Ordering (router is downstream of both):
1. Guest-verify step 5 first → lock a known-good baseline on real HW (riskiest
   unknown; never run). **Don't refactor the `process` hot path before this** — a
   step-5 failure must not be ambiguous between the libusbgx migration and a router
   change, and the router adds nothing step 5 exercises.
2. Decide the engine (standalone vs libmctp-core), now informed by "data volume is
   large" → leans libmctp-core. (Resolves the open question in the decisions log.)
3. Introduce the Router seam on the chosen engine; wrap control as the first handler.

### Where the data comes from: PLDM + the pldmd integration target (2026-06-24)

Two layers people conflate: **PLDM = the on-wire representation/contract** (msg type
`0x01`; how sensors/FRU/inventory are encoded/requested) vs **sourcing the values =
querying D-Bus/sysfs/hardware** (our job, PLDM-agnostic). PLDM is optional — vendor
type works too — but in OpenBMC it's the natural fit (the data is already on D-Bus,
and libpldm/pldmd exist). `pldmd`'s responder side (`libpldmresponder`) literally maps
PLDM sensor/PDR requests to D-Bus, so "how to get the sensor value" is pldmd's job.

**Target architecture (north star) — manager/provider split, two processes:**

```
  USB wire ──(host: kernel mctp-usb + AF_MCTP + pldmd as *requester*)
                       │
            ┌──────────▼───────────┐
            │  mctpusbd            │  L1 FunctionFS/USB
            │  (device-side demux  │  L2 MCTP transport + reassembly
            │   + transport)       │  L3 control responder 0x00
            └──────────┬───────────┘  + a demux socket server
              demux socket │  register type 0x01; exchange WHOLE PLDM messages
            ┌──────────▼───────────┐
            │  pldmd (responder)   │  L4 PLDM + get sensor values
            │  libpldmresponder    │  ← reads D-Bus
            └──────────────────────┘
```

mctpusbd becomes the **device-side mirror of the host's mctp-demux-daemon**; pldmd
connects as a client. Clean low coupling: pldmd never touches USB, mctpusbd never
knows about sensors. Prerequisites to make the hook work:
1. mctpusbd must do **reassembly/fragmentation** (demux delivers whole messages) →
   the same engine decision (build vs libmctp-core).
2. The **demux socket dialect** must match the pldmd build (NVIDIA libmctp demux vs
   upstream differ).
3. Confirm **pldmd responder mode** + PDR/sensor config fits a peripheral role.
4. `pldmd` only speaks MCTP via the **demux socket** or **kernel AF_MCTP** — not an
   arbitrary custom IPC. So the hook is specifically "mctpusbd speaks demux".

**LOAD-BEARING CONSTRAINT (the wall everything hits):** a *clean* AF_MCTP/pldmd
integration on the device side **requires a device/gadget-side in-kernel MCTP-USB
driver** — which does not exist in mainline (host-side `mctp-usb` exists; the gadget
side is the gap, and is the whole reason this project hand-rolls FunctionFS). You
**cannot dodge the demux-deprecation problem by jumping to AF_MCTP**, because
AF_MCTP is downstream of having a kernel MCTP *link*, and userspace can't create one
+ inject USB traffic (a `mctp-serial`-over-pty + framing-transcode bridge is possible
but ugly and not worth it). "demux is deprecated" and "kernel lacks gadget USB" are
the **same wall**.

Consequence — until that kernel driver exists, the device side is userspace, and
wiring a *separate* pldmd has exactly two userspace options:

| Path | Keeps pldmd a separate process? | Demux-deprecation exposure | Notes |
|------|----|----|----|
| **A. mctpusbd speaks demux socket** | **Yes** | Yes (shelf-life) | The north-star diagram above; only way today to keep pldmd independent |
| **C. link `libpldm` in-process** | No | None (no demux at all) | Reimplement responder in mctpusbd; more future-proof but loses process separation |

**Short-term decision (priority = keep pldmd independent): go Path A (demux socket),
accept the shelf-life.** Revisit if/when a kernel gadget MCTP-USB driver lands → then
AF_MCTP is clean and L1–L3 mostly dissolve into the kernel, leaving mctpusbd ≈ L4.

Host side is independent and already supported: host = kernel `mctp-usb` host driver
→ AF_MCTP → pldmd as requester, **no libmctp needed** (planned direction). Only the
device/gadget side lacks kernel support.

**Kernel status confirmed (2026-06-24, web search):** Linux 6.15 merged the host-side
`mctp-usb` driver (`drivers/net/mctp/mctp-usb.c`, by CodeConstruct). The driver page
makes **no mention of a gadget/device-side counterpart** — the wall remains. AF_MCTP
on the device side is not possible today.

### Device libmctp-core vs host kernel stack are orthogonal — no conflict (2026-06-24)

Concern raised: the future box is **both a USB host** (downstream, to other MCTP-USB
devices) **and a USB device/gadget** (upstream). If mctpusbd adopts **libmctp-core** on
the device side while the host side runs the **kernel MCTP stack**, does the device
linkage collide with the host stack the way the earlier host-side conflict did?

**No.** The conflict you actually hit is about **transport/link ownership**, not about
linking libmctp: on the *host role* you can't run **both** kernel `mctp-usb` **and**
libmctp's **demux daemon** over the *same* link — two MCTP stacks fighting for one
transport. That is purely a host-role choice and is **orthogonal** to whether the
device daemon links libmctp-core.

Why device-side libmctp-core owns no kernel transport:
- libmctp-core is used as a **pure library** (reassembly / fragmentation / control
  responder) operating on **byte buffers**. It does **not** open AF_MCTP, bind a kernel
  mctp netdev, or run a demux daemon.
- mctpusbd's transport is the **FunctionFS fds** (ep0/ep1/ep2). Pulling libmctp-core in
  as a subproject is **transport-agnostic pure code** — it does **not** drag in the
  kernel stack or AF_MCTP. Dependency graph stays clean.
- Device side = USB **gadget** subsystem (functionfs/configfs/UDC); host side = USB
  **host** subsystem (`mctp-usb` + AF_MCTP). Different role, different controller,
  **no shared kernel object** → nothing to collide.

The **one rule to keep:** on the host role, don't double-stack (kernel `mctp-usb` +
libmctp demux) on the same link — pick one (we picked kernel AF_MCTP). Edge case: a
single-box `dummy_hcd` loopback (self-host + self-device) → the host-role kernel driver
*will* claim your own gadget's MCTP interface, but that touches the **host** side only;
the device side still just sees functionfs bytes, and in that test you *want* them to
talk — expected behaviour, not a conflict.

**Packaging corollary — splitting this repo out of libmctp.** Today the MCTP engine is
the **standalone** one (no libmctp dependency), so extracting the repo is near-zero
cost. The **subproject** mechanism (meson `.wrap` → libmctp git, or system libmctp) is
only needed **if/when we adopt libmctp-core** — a separate decision from extraction, not
coupled to it.

**Layer clarification — libusbgx is L1, orthogonal to the engine decision.** libusbgx
does the gadget *lifecycle* (create gadget / mount FunctionFS / enable UDC) = L1, the
"be a USB device" plumbing `gadget.sh` used to do. The engine decision (standalone vs
libmctp-core) only swaps **L2/L3** (the MCTP transport/protocol). So **going libmctp-core
does NOT discard libusbgx** — it stays, FunctionFS stays, the descriptors stay; only
`mctp_packet`/`mctp_endpoint` change. The *only* future that removes libusbgx+FunctionFS
is the **kernel-gadget-driver** one (the kernel presents the device itself) — that is
where "L1–L3 dissolve into the kernel" applies, NOT the demux/libmctp-core path. The
step-5 libusbgx work is foundational, not throwaway.

Design pull-through for now: **keep L2 (MCTP transport) cleanly separable from the
USB I/O**, so a later swap to a kernel link (or libmctp-core) is cheap.

### Reference: NVIDIA `utils/mctp-demux-daemon.c` splits along the engine seam (2026-06-24)

Read the existing demux daemon (2049 lines). It is two halves divided by exactly the
standalone-vs-libmctp-core seam, which decides how much is reusable for mctpusbd:

- **A. demux/socket half — directly referenceable, ~portable.** `struct client {active,
  sock, type, eid}`, `accept4`, `client_process_recv`, `forward_message`,
  `client_remove_inactive`, poll integration. Transport-agnostic socket plumbing, and
  it pins **the exact socket protocol pldmd speaks as a client** (so matching it = pldmd
  unchanged):
  ```
  register:        on connect, client sends 1 byte = the msg type it handles
  client→daemon:   [tag_owner|tag][dest eid][MCTP msg: type byte first]   (to wire)
  daemon→client:   [(tag_owner<<3)|tag][src eid][MCTP msg]                (from wire)
  route:           deliver to clients whose registered type == msg[0] & 0x7f
  ```
- **B. transport half — NOT standalone-portable; it delegates to libmctp core.** The
  daemon does **no** reassembly/fragmentation itself: outbound it calls
  `mctp_message_tx(...)` (core fragments); inbound it registers `mctp_set_rx_all(...)`
  and receives already-**reassembled whole messages**. So "give the client a whole
  message" is core's job, not in this file.

Consequence — **"reference the demux daemon" naturally means "port the socket half +
adopt libmctp-core underneath", because that is how this file is built.** Adopting core
⇒ mctpusbd ≈ this daemon with a **FunctionFS/gadget binding** swapped in for its
libusb/i2c binding: socket half copied, reassembly free, lowest interop risk with pldmd.
Staying standalone ⇒ the socket half still helps, but half B does not — reassembly is
still ours to build. Either way, reading it pins the engine decision with concrete code.

Don't copy: the MCTP-control `tx_pvt_message` path (per-bus private-binding headers,
PCIe/SPI/I2C/USB EID offsets) is bus-owner/host-flavoured; our device answers control
(type 0x00) **in-process**, not by forwarding. And confirm the socket dialect against the
actual pldmd build (the same demux-support one-vote-veto noted above).

### Path A + libmctp-core + spike are one line (2026-06-24)

Path A (architecture), libmctp-core (implementation tool), and the P1 spike (validation)
are not three separate decisions — they form a single implementation path:

- **Path A** = mctpusbd acts as the device-side demux server; pldmd connects as a client
  via mctp-demux transport. Chosen over **Path C** (link libpldm in-process) because
  `libpldmresponder` (D-Bus → PLDM sensor/FRU/PDR) is the valuable part pldmd already
  provides; Path C would require reimplementing exactly that, negating the reuse benefit.
- **libmctp-core** = the implementation tool that makes Path A tractable: reassembly /
  fragmentation are free, and the NVIDIA demux daemon's socket-server half is directly
  referenceable. Without it, reassembly is ours to hand-roll (error-prone).
- **P1 spike** = validates that the FunctionFS/gadget binding (the one piece libmctp-core
  does *not* provide) has no unexpected friction — bounded work, but unproven.

The three parts together: **libmctp-core + FunctionFS binding (①, spike target) +
demux socket server half (③, reference from NVIDIA daemon) = Path A implementation.**
Reassembly (②) is free from libmctp-core.

**pldmd analysis (2026-06-24):** phosphor-pldm supports both `mctp-demux` and `af-mctp`
transports, selected at compile time via `transport-implementation` (no default — must be
set in the build recipe). The demux dialect is abstracted inside `libpldm`'s
`pldm_transport_mctp_demux_init()` — we need to read `libpldm/transport/mctp-demux.c` to
pin the wire protocol before implementing the server side. pldmd's responder role is
passive (no EID assignment, reads host EID from config) → compatible with device-side use.

### Conditional preferred shape (leaning, to ratify after step-5) — 2026-06-24

**If/when we build the data-rich + separate-pldmd future, the preferred shape is:
adopt libmctp-core ⇒ mctpusbd ≈ the demux daemon with a FunctionFS/gadget binding.**
Rationale: reassembly/fragmentation is the easiest thing to get wrong and it's free in
core; the socket half is referenceable; lowest interop risk with pldmd. Don't hand-roll
what already exists. **This is the concrete answer to the deferred engine decision** —
recorded as a leaning, not yet ratified.

Conditions it is recorded WITH (so it isn't mistaken for unconditional):
1. **Not 100% free — a gadget binding is ours to write.** libmctp has no device-side
   binding (host = libusb). But STRATEGY §4 already found `usb.c`'s DSP0283 framing is
   role-neutral/reusable, so the binding ≈ "reuse that framing + swap libusb I/O for
   FunctionFS ep read/write" → bounded, but real work.
2. **Does NOT escape the demux shelf-life.** core gives reassembly, not a
   non-deprecated pldmd transport; the socket is still demux, the kernel wall remains.
3. **Conditionally reverses the standalone decision, doesn't overturn it.** standalone
   stays right for a *minimal control-only* endpoint (zero-dep, pure tests);
   libmctp-core is preferred only for the *data-rich + separate-pldmd* scope. Both hold.
4. **It's a re-platform, not a tweak.** `mctp_packet`/`mctp_endpoint` mostly replaced by
   core; `mctp_control` becomes a handler or is taken over by core; L1 (FunctionFS) +
   the new binding + the demux socket half are what's kept. Ratify after the step-5
   baseline.

---

## Milestones

- [~] **1a** — `dummy_hcd` + configfs standard function (acm) → `lsusb` sees it
  - verify: `ls /sys/class/udc/` shows `dummy_udc.0`; `lsusb` lists the gadget
  - code: `scripts/gadget.sh up-acm` (written; needs guest run)
- [x] **1b** — FunctionFS daemon enumerates
  - verified in guest: `lsusb` shows `1d6b:0104 daryl MCTP FFS device`; ffsd
    stays up servicing ep0 (prints BIND/ENABLE)
- [x] **2** — descriptors: interface class `0x14` + bulk IN/OUT
  - verified in guest: `lsusb -t` shows the interface (Class=[unknown] = 0x14,
    Driver=[none]); device enumerates at HS. `ffs_descriptors.hpp` = class 0x14,
    subclass 0 / proto 1, 2 bulk EPs (OUT 0x01 / IN 0x81, FS 64 / HS 512), counts=3;
    `test_descriptors.cpp` has byte-level EP checks.
- [x] **3a** — bulk endpoint I/O moves
  - `ffsd` opens ep1/ep2 on ENABLE, read OUT → write IN (ZLP on full-MPS frames).
    The bulk path is proven by M3b round-trips below; the `--echo` flag itself was
    not separately exercised (same read/write path).
- [x] **3b** — MCTP-USB framing + control responder (Set/Get EID, UUID, Msg Type)
  - verified end-to-end in guest: host `mctp-demux-daemon usb port_path=1-1 mode=0`
    + `mctp-ctrl -t 3 -w 1-1` got correct responses for all four commands
    (Set EID=9, Get EID→9, Get UUID, Get Msg Type).
  - pipeline: `mctp_usb_frame` (DSP0283) → `mctp_packet` (transport hdr) →
    `mctp_control` (responder) → `mctp_endpoint` (`mctpep::process`), all also
    unit-tested off-target.
  - engine choice (standalone vs libmctp-core) still deferred — see decisions log.

---

## Decisions log

- _(date)_ Folder `daryl-mctp-usb-device/`, language C++20 (consistent with
  `daryl-mctp-usb-tester`), own meson.build, standalone build.
- _(date)_ Dev environment: QEMU guest + `dummy_hcd` single-kernel loopback;
  real-hardware UDC will be `aspeed-vhub`.
- _2026-06-23_ Single language C++ (no C/C++ core/app split): no external C
  consumer needs the core, and the FunctionFS uABI header includes cleanly in
  C++. Get the desired decoupling from a module seam (transport vs protocol),
  deferred to M3b when the MCTP responder appears — not a language boundary now.
- _2026-06-23_ Style follows the sibling `daryl-mctp-usb-tester` (snake_case
  functions / trailing-underscore members / PascalCase types / `enum class`),
  not the c-style summary's camelCase — existing-file consistency wins.
- _2026-06-23_ 1b stays minimal: vendor class `0xff`, **zero data endpoints**,
  single-file `ffsd.cpp`. Class `0x14` + bulk EPs are M2; fewer variables for the
  first guest bring-up. No premature module split.
- _2026-06-23_ **M3b responder engine: standalone vs libmctp-core — DEFERRED to
  the M3b seam.** Analysed both (incl. Zephyr's `subsys/pmci/mctp/mctp_usb.c`,
  which is a libmctp-core *binding*). Key findings:
  - Either way **we write the control responder ourselves**: libmctp (this NVIDIA
    tree) has **no device-side responder** — the only Set/Get EID code is the
    *requester* side (`ctrld/mctp-encode.c`). So `mctp_control.cpp` is needed in
    both designs and is the reusable core. (`core.c:496` routes non-transport
    control requests to the `mctp_set_rx_all` callback — a clean hook either way.)
  - **libmctp-core only earns its keep for "large messages"** (>64 B = MCTP BTU →
    multi-packet, SOM/EOM/seq reassembly + fragmentation, and per-msg-type demux).
    Our 4 control commands are all single-packet, so standalone's single-packet
    limit (`mctp_packet.cpp` drops non-SOM+EOM) is fine for control-only.
  - Standalone wins on: zero external dep, 4 fully-owned files, pure-fn tests, no
    C/C++ impedance. libmctp-core wins on: free reassembly/fragmentation, msg-type
    demux (future PLDM/SPDM), and **single source of truth with the host** (same
    lib/wire structs). Zephyr uses a *different* libmctp fork (`.tx_storage` vs
    NVIDIA `.pkt_priv_size/.control_rx`): the *pattern* ports, the code does not.
  - **Decision hinges on roadmap**: minimal discoverable endpoint → standalone;
    will carry application protocols (PLDM/SPDM/firmware/large payloads) →
    libmctp-core. Not yet answered, so deferred. Reversible: both routes keep
    `mctp_control.cpp` + `mctp_usb_frame.cpp`; only `mctp_packet.cpp` +
    `mctp_endpoint.cpp` are standalone-only and would be replaced by core.
  - Unblocks now: **M2 + M3a are route-agnostic** (descriptors + FunctionFS ep
    I/O don't care who processes the bytes), so proceed with them and decide at
    M3b. The default MCTP path is wired to the existing standalone
    `mctpep::process` for now (it's built + tested) — swapping to a core binding
    later only touches that one seam.

---

## Responder behaviours & simplifications

Things the device does on purpose that are worth knowing. Only the first is a
shortcut a fuller implementation would tighten; the other two are correct as-is.

- **No dest-EID filtering.** The responder answers regardless of the packet's
  destination EID. This is *correct for point-to-point USB* — there is exactly one
  device on the bulk pipe, so every request is for us — and it is what lets us
  answer Set EID before we have an EID, and answer Get-EID-addressed-to-9 while
  still unassigned (we reply "EID 0", which is right). A spec-complete endpoint on
  a *shared / bridged* bus would instead accept only dest == own EID / null (0) /
  broadcast (0xFF) and drop the rest. libmctp-core does exactly this
  (`mctp_rx_dest_is_local`), so adopting it later (see decisions log) gets it for
  free. → tighten only if this device ever sits behind an MCTP bridge.
- **Small single-packet responses, with ZLP.** All control responses are < 64 B,
  so the host's bulk-IN transfer completes on the short packet; the full-MPS edge
  is handled by appending a zero-length packet (`ffsd` `write_in_frame`). Correct,
  not a shortcut.
- **Unknown commands answered, not dropped.** Any command we don't implement (e.g.
  Get MCTP Version 0x04, routing-table commands) gets a well-formed response with
  completion code 0x05 (ERROR_UNSUPPORTED_CMD), per DSP0236. Correct behaviour.

## Blockers / open questions

- To confirm at first guest run: does a vendor-class interface with **zero data
  endpoints** enumerate cleanly on dummy_hcd/ffs? Only runtime unknown left for 1b
  (cannot test on WSL — no UDC). Fallback if it rejects: declare the two bulk EPs
  early (pull M2's endpoint descriptors forward).
- Candidate to confirm when host-testing 3a/3b: ensure kernel `mctp-usb` host driver
  is not bound to the class-0x14 interface, else libusb can't claim it.
- **OPEN (M3b): responder engine = standalone `mctpep::process` vs libmctp-core
  binding.** Deferred per the decisions-log entry; pick when wiring M3b. Needs one
  product input: will this device ever carry application protocols / large
  (multi-packet) messages, or stay a minimal MCTP control endpoint?
  - _Update 2026-06-24:_ that input effectively arrived — intent is to expose lots
    of our own system data (D-Bus → MCTP), i.e. large/multi-packet app protocols →
    **leans libmctp-core**. Still formally open (pick after the step-5 baseline); see
    "Future: proprietary data interfaces over MCTP" above for the full plan + ordering.

---

## Session log

> Append newest entries at the top. Format: `### YYYY-MM-DD — summary`

### 2026-06-25 — A1 implementation done; bridge interface design (A1/A2 compile-flag switching) established

**A1 implementation (C17).** PTY + N_MCTP bridge complete. New files:
- `mctp/mctp_serial_frame.{h,c}` — DSP0253 encode/decode, CRC-16/CCITT-FALSE
- `ffs/ffs_pty.{h,c}` — open `/dev/ptmx`, attach N_MCTP line discipline (`TIOCSETD`)
- `ffs_daemon.c` — added `FFS_MODE_PTY_BRIDGE` with 7-state rx state machine and
  slot-based poll loop (ep0 + ep_out + pty_master); `ffs_serve()` gains `pty_master_fd` param
- `mctpusbd.c` — defaults to `FFS_MODE_PTY_BRIDGE`; opens PTY, passes both fds to lifecycle
- `ffsd.c` — unchanged in behaviour; passes `-1` for pty_master_fd

18/18 unit tests pass (no regression). A1 implementation is embedded in `ffs_daemon.c`
(not yet behind the bridge interface abstraction — that is the next step).

**Bridge interface design (`DESIGN-BRIDGE.md` created).** Key decisions:
- A1 is a **permanent backend**, not a throwaway. Rationale: works without a kernel module
  (requires only `CONFIG_MCTP_SERIAL=y`); remains the preferred backend for standard
  OpenBMC images. A2 adds better performance but A1 is not removed.
- Bridge backend is the **only** thing that differs between A1 and A2. Everything else
  (`ffs_daemon.c`, USB header handling, gadget lifecycle) is identical.
- Interface: 4 functions — `ffs_bridge_open`, `ffs_bridge_handle_out`,
  `ffs_bridge_handle_in`, `ffs_bridge_close`. `ffs_daemon.c` calls through this
  interface with zero `#ifdef`.
- Meson option `bridge` (`a1` / `a2`) selects which `.c` file is compiled.
  `meson setup build -Dbridge=a1` / `-Dbridge=a2`. Default: `a1`.
- A1 source (`ffs_bridge_a1.c`) and A2 source (`ffs_bridge_a2.c`) coexist in the tree.

**Next immediate step:** refactor — extract A1 bridge logic from `ffs_daemon.c` into
`ffs/ffs_bridge_a1.c` implementing the `ffs_bridge.h` interface. No behaviour change.
Checklist in PROGRESS.md "Current focus" section.

### 2026-06-25 — architectural review: demux socket path has a dual-role ceiling; kernel MCTP stack is the target direction

**Context.** After completing Step C (demux socket server), a deeper analysis of the
full system requirements revealed a fundamental conflict. Full analysis in
[`ARCH-TRANSPORT.md`](./ARCH-TRANSPORT.md).

**Key findings:**

- `mctp-demux-daemon`'s USB binding (`libmctp/usb.c`) uses `libusb` and is **host-only**.
  It is not a device-side implementation. Our `mctpusbd` (FunctionFS / UDC) is the correct
  device-side answer — these are opposite ends of the USB cable.

- `pldmd` is not host-specific (it is a PLDM daemon, can be requester or responder), but it
  supports only **one transport backend at a time** (`transport-af-mctp` OR `transport-mctp-demux`,
  compile-time choice). A machine that is simultaneously USB host (kernel `mctp-usb` → AF_MCTP)
  and USB device (our FunctionFS → demux socket) would require two incompatible backends in
  one pldmd — they collide.

- The kernel MCTP stack (5.15+) is a **multi-interface router**: if both USB host and USB
  device sides register as kernel MCTP net devices, pldmd uses `AF_MCTP` and the kernel
  routes across both. Conflict disappears.

- The upstream USB gadget-side MCTP kernel driver does not exist (Linux 6.15 merged the host
  side `mctp-usb.ko`; no gadget counterpart). We must bridge FunctionFS to the kernel stack
  ourselves.

**New options (supersede the earlier "Path A" demux plan):**

| | | |
|---|---|---|
| **A1** | PTY + `mctp-serial` line discipline | No kernel module; needs DSP0253 framing in userspace |
| **A2** | Small kernel module (~200 LOC) | Cleanest; A1 stepping stone leads here naturally |
| **B**  | Full kernel MCTP gadget driver | Correct long-term upstream direction; highest effort |

A1 → A2 is incremental (ffs_daemon + USB packet handling reused; only framing/PTY layer
replaced by chardev + kernel module). Not a full rewrite.

**Superseded earlier notes.** Two earlier entries in this log are now contradicted:
- *"mctp-serial-over-pty… possible but ugly and not worth it"* — this assessment did not
  account for the A1→A2 stepping-stone value. A1 is a reasonable intermediate step.
- *"Short-term decision: go Path A (demux socket), accept the shelf-life"* — this decision
  is under review; the dual-role conflict makes the shelf-life shorter than acceptable.

**Decision made (2026-06-25, commit `0753217`).** Step B/C C++ changes discarded:
`mctp_core_binding`, `mctp_demux_server`, `subprojects/libmctp` removed; `ffs_daemon`
and `mctpusbd` restored to `bc47414` state. **Pivoting to Option A1** (PTY +
`mctp-serial`). First gate: confirm `CONFIG_MCTP_SERIAL` on aspeed-2700 kernel.

### 2026-06-25 — Step C implementation: C2+C3+C4 done; 28/28 tests pass

- **C2 — `mctp/mctp_demux_server.{hpp,cpp}`**: `mctpcore::DemuxServer` — pure Unix socket
  server (AF_UNIX SOCK_SEQPACKET `"\0mctp-mux"`). API: `server_fd()` / `client_fd()` (for
  poll); `accept_client()` (accept + 1-byte registration); `forward_to_client(src_eid, msg)`
  (→ `[src_eid][msg...]`); `recv_from_client()` (→ `{dest_eid, msg}`). No libmctp dependency.
- **C3 — `mctp/mctp_core_binding.{hpp,cpp}`**: Added `DemuxServer*` + single-slot
  `{pending_eid, pending_tag, pending_valid}` to `FfsBinding::Impl`; `rx_all_cb` type 0x01
  branch → `demux->forward_to_client(src_eid, body)` + saves tag. New public methods:
  `set_demux(DemuxServer*)` and `on_client_rx()` (reads from client, calls `mctp_message_tx`
  with saved tag). Type 0x01 is silently dropped when `demux == nullptr`.
- **C4 — `ffs/ffs_daemon.{hpp,cpp}` + `app/mctpusbd.cpp`**: Added `PollExt` struct
  (`collect` / `dispatch` lambdas); `ffs_serve` poll loop migrated from fixed array to
  `std::vector<pollfd>` with PollExt hooks. `mctpusbd.cpp`: constructs `DemuxServer`,
  builds collect/dispatch lambdas, calls `set_demux` on first ENABLE.
- **Tests**: `test/test_demux_server.cpp` — 6 GTest cases (construction, accept, forward,
  recv, disconnect, no-client). All 6 new + 22 existing = **28/28 pass** (native g++ build).
- **meson.build**: `mctp/mctp_demux_server.cpp` added to mctpusbd sources; `test_demux_server`
  target added.
- **next**: Step B hardware gate (deploy build-sdk/mctpusbd to aspeed-2700 — 4 control
  commands via libmctp-core); then Step C hardware gate (pldmd with `transport=mctp-demux`,
  PLDM Get TID round-trip). Step C7 (update GetMessageTypeSupport → `[0x00, 0x01]`) is
  intentionally last, after end-to-end PLDM test passes.

### 2026-06-25 — Step C pre-research: demux wire protocol pinned; implementation design fixed

- **Demux wire protocol pinned** (pre-implementation research item for Step C now complete):
  - Read both `libmctp/utils/mctp-demux-daemon.c` (NVIDIA, 2049 lines) and
    `libpldm/src/transport/mctp-demux.c` (openbmc, the file pldmd actually uses).
  - **Key finding — two different protocols**: NVIDIA daemon uses a 3-byte prefix
    `[tag_info][eid][type]` with per-binding socket names (`"\0mctp-*-mux"`); libpldm uses
    a simpler 2-byte prefix `[eid][type]` with fixed socket name `"\0mctp-mux"` and no tag
    byte in either direction. We must implement the libpldm dialect (pldmd connects to that).
  - Pinned protocol: `AF_UNIX SOCK_SEQPACKET "\0mctp-mux"`; registration = 1 byte `0x01`;
    RX (server→pldmd) = `[src_eid][0x01][pldm_payload]`; TX (pldmd→server) = `[dest_eid][0x01][pldm_payload]`.
    Full details in PLAN-path-a.md Step C.
- **Design error corrected in PLAN-path-a.md**: Step C scope had `ffs/mctp_demux_server.{hpp,cpp}`;
  corrected to `mctp/mctp_demux_server.{hpp,cpp}` — `DemuxServer` is pure Unix socket IPC with
  no FunctionFS dependency; `ffs/` placement would invert the app→ffs→mctp DAG.
- **Step C design concretised**: `DemuxServer` (socket layer) + `FfsBinding` tag-tracking slot
  + `PollExt` extension to `ffs_serve` (collect/dispatch lambdas for extra poll fds). Tag
  tracking: single pending `{src_eid, msg_tag}` slot (sufficient for device-side responder).
  Sub-steps updated in PLAN-path-a.md.
- **`transport-implementation=mctp-demux` confirmed** as the only viable choice: `af-mctp`
  requires a kernel MCTP netdev; the gadget-side driver does not exist (same wall as noted
  in the decisions log). Accepting demux shelf-life is the correct trade-off.
- **next**: Step B hardware gate (deploy build-sdk/mctpusbd to aspeed-2700, verify 4 control
  commands via libmctp-core engine); then Step C implementation.

### 2026-06-24 — Step B: libmctp-core FunctionFS binding implemented; cross build + 22 tests OK
- **Step B complete (build + unit-test gate passed)**:
  - `subprojects/libmctp.wrap` + `subprojects/packagefiles/libmctp/meson.build` — wraps the
    NVIDIA fork's core (alloc.c + core.c + log.c only; no json-c / systemd / libusb); statically
    linked into mctpusbd. daryl SDK (aarch64) has no system libmctp, so the subproject fallback
    always fires.
  - `mctp/mctp_core_binding.{hpp,cpp}` — `mctpcore::FfsBinding` class: `recv_frame()` strips the
    DSP0283 USB header, feeds the MCTP packet to `mctp_bus_rx`, libmctp reassembles and calls
    `rx_all_cb`; `tx_cb` prepends the USB header and writes to ep-IN (ep2). EID is re-registered
    via `mctp_unregister_bus` + `mctp_register_bus` after Set EID so libmctp accepts packets to
    the new EID. Type 0x00 → `mctpctrl::handle`; other types dropped (PLDM handler = Step C).
  - `ffs/ffs_daemon.{hpp,cpp}` — added optional `FrameProcessor` parameter to `ffs_serve`;
    `ffsd` passes nothing (keeps standalone trace path unchanged); `mctpusbd` passes a lambda
    that lazily constructs `FfsBinding` on the first frame after ENABLE (ep_in_fd known then).
  - `meson.build` — `dependency('libmctp', fallback: ['libmctp', 'libmctp_dep'])` +
    `mctp/mctp_core_binding.cpp` in mctpusbd sources.
- **Compatibility note**: SDK libmctp (openbmc 0.11) `mctp_rx_fn` parameter order is
  `(src_eid, tag_owner, msg_tag, **data**, msg, len)` — `data` (user context) is 4th, before
  `msg`. NVIDIA fork reverses `data`/`msg`. Implementation matches the SDK's order.
- **cross-build + 22/22 gtest tests pass** under qemu-aarch64 (daryl SDK, Cortex-A57). libmctp
  statically linked into mctpusbd (no new .so NEEDED on target). No new warnings.
- **Gate to Step C (hardware)**: deploy build-sdk/mctpusbd to aspeed-2700; confirm all 4 control
  commands (Set EID, Get EID, Get UUID, Get Msg Type) still pass through the libmctp-core engine.
- **next**: Step C — demux socket server + pldmd integration (gated on hardware verify of Step B).
  Before that: read `libpldm/transport/mctp-demux.c` to pin the exact wire protocol.

### 2026-06-24 — Path A design fleshed out; PLAN-path-a.md created
- **PLDM = type 0x01 MCTP message**: same mechanism as any raw custom payload; only
  difference is the type byte. Current mctpusbd drops all non-0x00 types at
  `mctpctrl::handle` line 61 — PLDM is equally unreachable today.
- **Router is the missing seam**: `mctpep::process` needs a dispatch on `body[0] & 0x7F`
  before calling any handler. This is the exact seam described in PROGRESS.md §Future;
  now unblocked (step-5 baseline locked).
- **No separate binary for "simple" mctpusbd**: the router design means mctpusbd
  naturally runs in control-only mode when pldmd is absent — same binary, different
  operational state. Contrast with `ffsd` (kept because it uniquely isolates FunctionFS
  from libusbgx; no equivalent unique role for a simple mctpusbd variant).
- **Implementation order**: Step A (router, ~5 lines) → Step B (libmctp-core binding,
  P1 spike) → Step C (demux socket server + pldmd). `GetMessageTypeSupport` updated to
  `[0x00, 0x01]` only at the end of Step C — never advertise before we can deliver.
- **Created [`PLAN-path-a.md`](./PLAN-path-a.md)**: full design, step breakdown,
  decision log, pre-implementation research checklist.

### 2026-06-24 — direction ratified: Path A + libmctp-core + P1 spike
- **af-mctp device-side wall confirmed** via web search: Linux 6.15 merged host-side
  `mctp-usb` (CodeConstruct); no gadget-side driver exists or is mentioned. Device side
  stays userspace FunctionFS for the foreseeable future.
- **pldmd analysed** (`/home/daryl/work/project/dep/pldm`): supports both `mctp-demux`
  and `af-mctp` at compile time (recipe choice); responder role is passive endpoint —
  compatible with device-side use. Demux dialect lives in `libpldm/transport/mctp-demux.c`
  (not pldmd itself) — need to read that before implementing server side.
- **Path A confirmed over Path C**: Path C (link libpldm in-process) would require
  reimplementing `libpldmresponder` (D-Bus → PLDM), which is exactly what pldmd already
  provides — bad trade. Path A reuses pldmd wholesale; only adds demux server in mctpusbd.
- **Unified picture**: Path A + libmctp-core + P1 spike are one line. libmctp-core gives
  reassembly free; NVIDIA demux daemon gives socket-half reference; spike validates the
  FunctionFS binding (the only unproven piece). See new "Path A + libmctp-core" section.
- next: P1 spike — wire Get EID through libmctp-core + FunctionFS binding; read
  `libpldm/transport/mctp-demux.c` to pin the demux wire protocol.

### 2026-06-24 — step-5 CLOSED: mctpusbd end-to-end verified on aspeed-2700
- confirmed on aspeed-2700 QEMU guest: `lsusb` shows `1d6b:0104 daryl MCTP FFS device`
  (enumerate OK); all four control commands answered correctly — Set EID=9, Get EID→9,
  Get UUID, Get Msg Type. mctpusbd round-trip identical to the earlier ffsd+gadget.sh run.
- libusbgx migration (steps 1–5) is **complete**. The self-contained daemon is the
  verified baseline for real hardware going forward.
- next: P1 — ratify engine decision (standalone vs libmctp-core spike); see Next actions.

### 2026-06-24 — step-5 on aspeed-2700: kernel-config gotcha found + fixed
- bringing mctpusbd up on the **aspeed-2700 QEMU image** (vs the earlier bare
  qemuarm64): it failed at `create_function(ffs): USBG_ERROR_BUSY`, on a **fresh
  boot, empty configfs, no prior run** (only mctpusbd present).
- root cause (traced through libusbgx `usbg.c:2091` `mkdir(.../functions/ffs.mctp)`
  → kernel EBUSY): the image had **`CONFIG_USB_FUNCTIONFS`** (the *legacy* FunctionFS
  gadget, `g_ffs`) built-in. It registers a "single" ffs device at boot →
  functionfs goes into **single-device mode** → the kernel rejects every configfs
  (named) ffs function with EBUSY, name-independently. configfs being empty is
  consistent (legacy gadget isn't configfs-based).
- fix: **rebuild the kernel without `CONFIG_USB_FUNCTIONFS`**, keep
  `CONFIG_USB_CONFIGFS_F_FS` (the configfs one we use). mctpusbd then starts.
  Recorded in RUNBOOK prerequisites + mctpusbd.md troubleshooting.
- also doc reorg this session: split usage docs into `mctpusbd.md` (simple) +
  `ffsd.md` + shared `host-verify.md`; RUNBOOK slimmed to a build/deploy hub.
- next: confirm how far step-5 got (enumerate / `lsusb` vs full 4-command MCTP
  round-trip) before ticking it.

### 2026-06-23 — repo layout: mctp/ + ffs/ + app/ split
- did: with two daemons now sharing one engine, split the flat tree by the
  transport-vs-protocol seam into three dirs:
  - **`mctp/`** — pure MCTP engine, no I/O, fully unit-tested off-target:
    `mctp_usb_frame` (DSP0283 USB framing codec), `mctp_packet`, `mctp_control`,
    `mctp_endpoint`. Zero dependency on `ffs/`.
  - **`ffs/`** — Linux FunctionFS glue: `ffs_descriptors.hpp` (descriptor blob) +
    `ffs_daemon.{hpp,cpp}` (ep0/ep event loop). Depends on `mctp/`.
  - **`app/`** — the two thin entry points: `ffsd.cpp`, `mctpusbd.cpp`.
  Dependency direction is a clean DAG: app → ffs → mctp. `mctp_usb_frame` lives in
  `mctp/` (it's a pure framing codec in the endpoint pipeline, not FunctionFS).
  `test/`, `scripts/` unchanged. (Briefly staged a 2-dir `core/`+`app/` split first,
  then refined to this 3-dir per request.)
- how: `meson.build` puts both engine dirs on the include path
  (`engine_inc = include_directories('mctp', 'ffs')`), so every source keeps
  **unprefixed** includes (`#include "mctp_control.hpp"`); only the test files lost
  their `../` prefix. No code logic touched; binaries still emit to `build*/ffsd`,
  `build*/mctpusbd`.
- result / verified: native `meson test` 5/5 OK; cross builds ffsd + mctpusbd and
  5/5 tests pass under qemu-aarch64. Pure move + meson-path change.

### 2026-06-23 — libusbgx migration steps 1–4 (mctpusbd), cross-verified
- did (step 1): extracted the FunctionFS event loop into `ffs_daemon.{hpp,cpp}` —
  `int ffs_serve(mount, Mode, on_ready={})` = open ep0 → write descriptors →
  `on_ready()` (the "safe to enable UDC" seam) → poll loop. `ffsd.cpp` reduced to a
  thin main calling `ffs_serve(mount, mode)` — no on_ready, no signal handler, so
  behaviour is byte-identical to the old monolith. Added a shared
  `volatile sig_atomic_t ffsd::g_stop` the loop checks on EINTR (only mctpusbd sets
  it). `Mode` enum moved to the header.
- did (step 2): `mctpusbd.cpp` — libusbg `usbg_init → create_gadget(VID/PID/strs) →
  create_function(USBG_F_FFS,"mctp") → create_config → add_config_function` →
  `mount(2)` functionfs → `ffs_serve(..., enable-UDC lambda)`. SIGINT/SIGTERM (via
  sigaction, **no SA_RESTART** so poll wakes with EINTR) → disable UDC + umount +
  `usbg_rm_gadget(USBG_RM_RECURSE)` + `usbg_cleanup`, so Ctrl-C self-cleans (no
  `gadget.sh down`). `--udc <name>`, default NULL = libusbg auto-pick first UDC
  (covers dummy_udc.0 dev + lone aspeed-vhub prod with one binary). Gadget identity
  constants mirror gadget.sh exactly (1d6b:0104, daryl / "MCTP FFS device").
- did (step 3): meson `dependency('libusbgx', required: false)`; shared `ffs_core`
  source list; `mctpusbd` built only `if dep.found()`. Native WSL (no libusbgx)
  builds ffsd + 5 tests; cross SDK (libusbgx 0.2.0) builds both. `mctpusbd` ELF
  confirmed `NEEDED libusbgx.so.3`.
- did (step 4): RUNBOOK §5 (`mctpusbd` one-shot usage, two-daemon explanation,
  UDC-default note, don't-mix-with-gadget.sh warning); this PROGRESS update.
- result / verified: native `meson test` 5/5 OK; cross `scripts/cross-test.sh`
  builds ffsd + mctpusbd clean (only the known cosmetic `_FORTIFY_SOURCE` -O warning)
  and all 5 tests pass under qemu-aarch64. MCTP core + `ffs_descriptors.hpp` UNTOUCHED.
- next: step 5 — guest-run `mctpusbd` (enumerate + 4 control commands), can't be done
  from WSL (no UDC). Responder-engine decision still deferred (orthogonal).

### 2026-06-23 — design discussion: roles, discovery, auto-assign, libusbgx plan
- clarified (for the record): **EID is bus-owner-assigned, not device-chosen**
  (EID≈DHCP IP, UUID≈MAC). The device is the endpoint (client); detecting "device
  arrived" + assigning EID is the **bus owner = host** side, not ours.
- **Discovery Notify (0x0D)**: we don't send it and don't need to on USB — USB's
  native enumeration (libusb hotplug, `usb.c`) replaces the "announce arrival" role.
  Verified the reference device `mctp-dev` also omits it. It is binding-specific
  (required on shared buses like SMBus, not USB). We have no initiator/requester
  path at all (responder-only) — fine for USB; would need adding for shared bus.
- **Auto "plug-in → assign EID" on the host**: NVIDIA `mctp-ctrl` has a daemon mode
  with USB discovery, but it is **bridge/pool-oriented** (Prepare-for-EP-Discovery,
  Allocate EID) — not a clean fit for our simple endpoint. Practical options: a thin
  host wrapper (udev/poll → run the verified Set EID) or `mctpd` (kernel stack).
  Recorded that `mctpd` lives in the **kernel-MCTP world** (AF_MCTP + netlink), not
  libmctp; it needs the kernel `mctp-usb` driver bound (mutually exclusive with the
  libusb path we use).
- **Decision: do the libusbgx migration next** (two binaries, UDC as a parameter).
  Full plan + verified prerequisites recorded under "Next phase" above. To be done
  in a fresh session.

### 2026-06-23 — END TO END in the guest + verbose ffsd trace
- VERIFIED in the QEMU gadget guest: device enumerates (`lsusb`: `1d6b:0104 daryl
  MCTP FFS device`; `lsusb -t`: Class=[unknown]=0x14, Driver=[none] → libusb free
  to claim). All four control commands answered through host
  `mctp-demux-daemon usb port_path=1-1 mode=0 -v` + `mctp-ctrl -t 3 -w 1-1 -e 9`:
  Set EID=9, Get EID→9, Get UUID, Get Msg Type. M1b / M2 / M3a / M3b closed.
- gotcha that bit (worth remembering): the USB port_path is `1-1` (bus 1, root
  port 1), NOT the `Dev 008` number. `-w 1-8` made libmctp port-path-mismatch and
  *silently ignore* the device (no recv). `mctp-ctrl` and the demux daemon must use
  the same value; confirm it from `/sys/bus/usb/devices/<name>/product`.
- did: added forced verbose logging to `ffsd` (it is an example) — every ep0 event,
  plus a hex + decoded trace of each bulk transfer (RX/TX, MCTP header, control
  command name via new `mctpctrl::command_name`, completion code, current EID).
  Reuses the real codec so the trace can't drift from behaviour. Native + cross
  build clean; 5/5 unit tests pass.
- next: responder-engine decision (standalone vs libmctp-core) still open pending
  the large-message / PLDM question (decisions log + open questions).

### 2026-06-23 — M2 descriptors + M3a/M3b data path wired (route-agnostic)
- analysis: compared device responder engines — standalone (current) vs libmctp-core
  binding (the Zephyr `subsys/pmci/mctp/mctp_usb.c` pattern). Verified in-tree that
  NVIDIA libmctp has **no device-side control responder** (only requester-side encode
  in `ctrld/`), and `core.c:496` hands non-transport control requests to the
  `mctp_set_rx_all` callback. Conclusion: responder is ours either way; libmctp-core
  only pays off for large/multi-packet messages (PLDM/SPDM). **Decision DEFERRED to
  the M3b seam** (see decisions log); M2/M3a are route-agnostic so we proceed.
- did (M2): `ffs_descriptors.hpp` → interface class 0x14, subclass 0 / protocol 1,
  two bulk EPs (OUT 0x01 / IN 0x81, FS 64 / HS 512), per-speed count 3. Extended
  `test/test_descriptors.cpp` with byte-level endpoint assertions.
- did (M3a/M3b): rewrote `ffsd.cpp` as a `poll()` loop — services ep0 lifecycle,
  opens ep1/ep2 on ENABLE, closes on DISABLE/UNBIND. Data loop: `--echo` does raw
  loopback (M3a); default drives `mctpep::process` (M3b). ZLP appended when a frame
  fills whole MPS packets (MPS read via `FUNCTIONFS_ENDPOINT_DESC`). `meson.build`
  now links the mctp_*.cpp into ffsd; fixed the stale ep-check note in `gadget.sh`.
- result / verified: native + **cross (aarch64 OpenBMC SDK)** build clean (only the
  known cosmetic `_FORTIFY_SOURCE` -O warning); 5/5 unit tests pass under qemu-user
  on the target ABI. `build-sdk/ffsd` ready to deploy.
- next: GUEST — `lsusb -v` (class 0x14 + 2 bulk EP), `ffsd --echo` bytes move, then
  default `ffsd` answers Get/Set EID via host `mctp-demux-daemon` + tester. Watch the
  two known gotchas: ep1/ep2 only open after ENABLE (handled), and the host kernel
  `mctp-usb` driver must NOT claim the class-0x14 interface (else libusb can't).

### 2026-06-23 — deployed to guest; ffsd runs (1b)
- did: cross-built ffsd (build-sdk/ffsd, aarch64) deployed into the QEMU OpenBMC
  guest; runs there. (Trap hit + fixed: build/ffsd is x86 native, build-sdk/ffsd is
  the aarch64 one to copy.)
- OPEN GATE: confirm 1b actually *enumerates* — `lsusb` shows 1d6b:0104 and ffsd
  prints BIND/ENABLE. This is also where the 0-endpoint risk would surface.
- NEXT PHASE — "basic MCTP over the real device" = M2 + M3a + wire M3b (logic already
  done/tested). Concretely:
    1. M2: ffs_descriptors.hpp -> class 0x14 + 2 bulk endpoints (IN/OUT); ffsd must
       open the new ep1/ep2 files after ENABLE. Verify `lsusb -v`.
    2. M3a: in ffsd, raw bulk echo loop (read ep-OUT -> write ep-IN). Verify bytes
       move with a libusb host / daryl-mctp-usb-tester.
    3. M3b wire: replace echo with mctpep::process (read OUT -> process -> write IN).
       Verify Get/Set EID etc. from the host tester.
  Gotchas to expect: endpoint files only openable after ENABLE; blocking vs AIO ep
  I/O; host-side kernel mctp-usb driver must NOT claim the class-0x14 interface
  (else libusb can't, per STRATEGY §4).

### 2026-06-23 — OpenBMC toolchain integration (cross-build + qemu-user)
- did: cross-compiled the whole project with the SDK (`sdk-daryl` → aarch64
  cortex-a57 OpenBMC, gcc 16.1.0). SDK's meson wrapper auto-applies the cross file,
  so the plain meson.build works unchanged. Added `scripts/cross-test.sh`
  (sdk-sourced → meson cross-build + run unit tests under `qemu-aarch64 -L sysroot`)
  and `.gitignore` for build dirs.
- result / verified: ffsd + all 5 test suites cross-compile clean; all 5 pass under
  qemu-user on the **target ABI** (not just WSL host gcc). Target sysroot has
  functionfs.h. Cosmetic warning only: _FORTIFY_SOURCE wants -O (debug build).
- note: qemu-user runs userspace syscalls only — no UDC/gadget. 1a/1b enumeration
  still needs the full QEMU OpenBMC guest; ffsd is now cross-built and ready to deploy.

### 2026-06-23 — M3b transport layer + end-to-end endpoint (TDD)
- did: `mctp_packet.{hpp,cpp}` (parse single-packet MCTP hdr, build swapped/TO-cleared
  response — mirrors libmctp mctp_hdr); `mctp_endpoint.{hpp,cpp}` orchestrator
  (frame->packet->control->packet->frame) = the seam `ffsd` will call per bulk
  transfer. `test/test_endpoint.cpp` runs a real Set-EID then Get-EID request frame
  through and checks the decoded response frames.
- result / verified: 5/5 `meson test` OK on WSL. The whole device responder works
  in memory without a UDC.
- note: leaf modules were stub-red->green; the orchestrator is pure composition (no
  stub step). Module seam (frame/packet/control/endpoint) is the low-coupling split
  we deferred — it materialised here, justified by testability not for its own sake.
- next: INFLECTION — remaining work (M2 descriptors: class 0x14 + 2 bulk EPs; M3a
  bulk I/O loop calling process) is FunctionFS I/O that can only be runtime-verified
  in the guest. Blind-writable + compile-checkable here, not runnable.

### 2026-06-23 — M3b control responder (TDD)
- did: `mctp_control.{hpp,cpp}` — DSP0236 responder for Set/Get EID, Get UUID, Get
  Msg Type Support; unknown cmd → unsupported-command response; EID starts
  unassigned and Set-EID mutates state. `test/test_control.cpp` covers all four +
  short-data, unsupported, instance-id echo, and dropped (non-control/response/short)
  inputs. Red on stub → green.
- chosen defaults: fixed `DefaultUuid` placeholder; advertise only MCTP control
  (0x00); simple endpoint, no EID pool.
- result / verified: `meson test` → descriptors + frame + control all OK on WSL.
- next: MCTP transport/packet header layer (route message body to responder, swap
  src/dst EID, tag owner, single-packet SOM/EOM) — next TDD increment, still off-target.

### 2026-06-23 — M3b frame codec (TDD, framing only)
- did: `mctp_usb_frame.{hpp,cpp}` — DSP0283 4-byte header encode/decode, mirrors
  libmctp usb.c (`dmtfId 0xB41A`, length == total bytes). Built test-first:
  `test/test_frame.cpp` (round-trip + rejects bad id / length / too-short), saw it
  go red on the stub, then green.
- result / verified: `meson test` → descriptors + frame both OK on WSL.
- next: M3b control responder (Set/Get EID, Get UUID, Get Msg Type Support) — next
  TDD increment; still off-target. Then guest run wires it to endpoints.

### 2026-06-23 — extracted descriptor module + unit test
- did: pulled the blob builders into header-only `ffs_descriptors.hpp` (namespace
  `ffsdesc` — `ffs` collides with libc `ffs()`); `ffsd.cpp` now just does ep0 I/O;
  added `test/test_descriptors.cpp` (dependency-free, byte-level LE checks) + meson
  `test()`.
- result / verified: `meson test` passes here on WSL — the risky wire layout is now
  guarded without a UDC. Extraction justified by testability, not decoupling.
- next: same guest run as below; M3b framing/responder is the next TDD target.

### 2026-06-23 — wrote 1a/1b artifacts, compile-verified
- did: `scripts/gadget.sh` (up-acm/up-ffs/bind/down), `ffsd.cpp` FunctionFS
  skeleton (V2 descriptors+strings to ep0, ep0 event loop), standalone `meson.build`.
- result / verified: builds clean here (g++, c++20, `warning_level=2`, no warnings).
  Descriptor blob written field-by-field against `/usr/include/linux/usb/functionfs.h`.
  NOT yet run — WSL kernel has no UDC.
- next: run in the QEMU gadget guest; expect at most one EINVAL/bind-fail debug pass
  on the 0-endpoint interface.

### (template)
- did:
- result / verified:
- next:
