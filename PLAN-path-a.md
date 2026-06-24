# Plan — Path A: PLDM over MCTP-USB (mctpusbd as demux server)

> Design and implementation plan for adding PLDM support to mctpusbd.
> Status lives in [`PROGRESS.md`](./PROGRESS.md). Context in [`STRATEGY.md`](./STRATEGY.md).

## Goal

Make mctpusbd the **device-side demux server**: it handles USB transport + MCTP
reassembly + MCTP control (type 0x00), and forwards PLDM messages (type 0x01) to
`pldmd` over a demux socket. pldmd runs as a separate process and handles all
PLDM semantics (sensor/FRU/PDR via D-Bus).

```
USB wire
  │
  ▼  FunctionFS (ep0/ep1/ep2)
mctpusbd
  ├── libmctp-core          L2: reassembly / fragmentation
  ├── type 0x00 handler     L3: MCTP control (Set/Get EID, UUID, Msg Type)
  └── demux socket server   L3: dispatch type 0x01 → pldmd
            │
            ▼ Unix socket (mctp-demux dialect)
          pldmd              L4: PLDM responder (libpldmresponder → D-Bus)
```

## Design decisions

### No separate binary for the simple case

The router design means mctpusbd **naturally degrades to control-only** when pldmd is
not connected: type 0x01 messages hit the router but find no registered handler (drop
or error); the type 0x00 control path is completely unaffected. Same binary, two
operational modes — no need for a separate `mctpusbd-simple`.

`ffsd` is kept as a separate binary because it isolates FunctionFS protocol issues from
libusbgx gadget lifecycle — a unique debug entry point. The simple mctpusbd has no
equivalent unique role.

### Router position

The message-type router lives in `mctpep::process`, after `mctppkt::parse` and before
calling any handler. It reads `packet.body[0] & 0x7F` (the message type) and dispatches:

```
body[0] & 0x7F == 0x00  →  mctpctrl::handle   (existing, unchanged)
body[0] & 0x7F == 0x01  →  demux socket forward
other                   →  drop (no response)
```

This is the seam described in PROGRESS.md §Future. The existing control handler and
its tests are untouched.

### Get Message Type Support: update last

`mctpctrl` currently advertises `[0x00]` (control only). Change to `[0x00, 0x01]`
**only after** the PLDM path is fully wired and tested — so we never advertise a
capability we cannot deliver.

### Reassembly: libmctp-core

The current standalone `mctp_packet.cpp` accepts single-packet only (SOM+EOM both set).
PLDM payloads (PDR, firmware, large sensor tables) routinely exceed the 64B MCTP BTU.
Reassembly is handled by libmctp-core as a library (pure byte-buffer, no AF_MCTP, no
kernel dependency). The FunctionFS binding wires `ep1`/`ep2` I/O to libmctp's send/recv
callbacks.

### Demux socket dialect

The wire protocol is in libpldm `transport/mctp-demux.c` (not in pldmd itself — pldmd
calls `pldm_transport_mctp_demux_init()`). **Read that file before implementing the
server side** to pin the exact dialect. The NVIDIA demux daemon's socket half is a
secondary reference but may differ.

### pldmd transport-implementation

phosphor-pldm supports `mctp-demux` and `af-mctp` at compile time (no default — set in
the Yocto recipe). For Path A to work, pldmd on the target must be built with
`mctp-demux`. We control the recipe so this is our choice.

---

## Implementation steps

### Step A — Message type router (standalone engine)

**Scope:** `mctp_endpoint.cpp` only, ~5 lines. No change to mctp_control, mctp_packet,
or any test.

```cpp
// in mctpep::process, after mctppkt::parse:
const uint8_t msg_type = packet.body.empty() ? 0 : (packet.body[0] & 0x7F);
if (msg_type != 0x00) {
    // placeholder: log / echo / drop — PLDM handler wired in Step C
    return {};
}
// existing: mctpctrl::handle(state, packet.body)
```

**Verify:** send a USB frame with type `0x7F` + raw bytes from the host; mctpusbd logs
receipt (or echoes back if we add an echo branch). Proves the router seam works before
any libmctp-core complexity.

**Gate to Step B:** router receives non-0x00 type without crashing; control path (type
0x00) still passes all 4 commands.

### Step B — libmctp-core FunctionFS binding (P1 spike → full binding)

**Scope:** new `mctp/mctp_core_binding.{hpp,cpp}` (or similar). Replaces the
`mctp_packet` + `mctp_endpoint` standalone path with libmctp-core as the engine.
`mctpctrl::handle` stays; it becomes the type-0x00 handler registered via
`mctp_set_rx_all` (or a type-specific callback if the core supports it).

Sub-steps:
1. Add libmctp as a meson subproject (`.wrap` → openbmc/libmctp or NVIDIA fork — decide
   which based on what pldmd's libpldm expects).
2. Write the FunctionFS binding: `send` callback = write ep1 (IN); `recv` loop = read
   ep2 (OUT) → `mctp_input()`.
3. Wire `mctp_set_rx_all` to the router from Step A.
4. Verify: all 4 control commands still work through libmctp-core.

**Gate to Step C:** 4 control commands pass on aspeed-2700 with libmctp-core engine.
No regression on the step-5 baseline.

### Step C — Demux socket server + pldmd integration

**Scope:** new `ffs/mctp_demux_server.{hpp,cpp}`. mctpusbd listens on a Unix socket;
pldmd connects using `mctp-demux` transport.

Sub-steps:
1. Read `libpldm/transport/mctp-demux.c` — pin the exact wire protocol (registration
   byte, message header format, routing rules).
2. Implement the server side: accept pldmd's connection, receive its type registration
   (0x01 = PLDM), forward received type-0x01 MCTP messages, deliver replies back.
3. Wire into the router (Step A seam): type 0x01 → `mctp_demux_server::forward()`.
4. Build pldmd with `transport-implementation=mctp-demux` (Yocto recipe).
5. End-to-end test: host sends a PLDM Get TID request; pldmd responds; verify on host.
6. Update `GetMessageTypeSupport` to return `[0x00, 0x01]`.

**Gate:** at least one PLDM command (Get TID or Get Terminus UID) round-trips correctly
end-to-end on aspeed-2700.

---

## Pre-implementation research still needed

- [ ] Read `libpldm/transport/mctp-demux.c` — pin demux wire protocol before Step C
- [ ] Decide libmctp fork: openbmc/libmctp vs NVIDIA libmctp (affects Step B subproject
      setup and pldmd dialect compatibility)
- [ ] Confirm which libmctp version pldmd's libpldm was built against (to match engine)

---

## What this does NOT change

- `ffsd` and `gadget.sh` — untouched, permanent debug path
- `ffs_daemon.{hpp,cpp}` — the FunctionFS event loop; Step B wires into it, not replaces it
- `ffs_descriptors.hpp` — descriptor blob unchanged
- `mctpctrl.{hpp,cpp}` and its tests — the control responder is demoted to a handler,
  its logic and tests are unchanged
- libusbgx gadget lifecycle (`mctpusbd.cpp` app layer) — L1 is orthogonal to L2/L3 engine
