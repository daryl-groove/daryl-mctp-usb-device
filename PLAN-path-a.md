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

### Step B — libmctp-core FunctionFS binding (P1 spike → full binding) ✓ build done

**Scope:** `mctp/mctp_core_binding.{hpp,cpp}` (new). Replaces the `mctp_packet` +
`mctp_endpoint` standalone path in mctpusbd with libmctp-core as the engine.
`mctpctrl::handle` stays as the type-0x00 handler registered via `mctp_set_rx_all`.

Sub-steps:
1. [x] Add libmctp as a meson subproject — NVIDIA fork via `subprojects/libmctp.wrap` +
   `subprojects/packagefiles/libmctp/meson.build` (core-only; statically linked).
2. [x] Write `mctpcore::FfsBinding`: `recv_frame()` strips USB header → `mctp_bus_rx()`;
   `tx_cb()` prepends USB header → writes ep-IN. EID re-registration after Set EID.
3. [x] Wire via `FrameProcessor` hook in `ffs_serve`; `mctpusbd` passes lambda that
   lazily constructs `FfsBinding` on first frame post-ENABLE.
4. [ ] **Hardware verify** (gate): 4 control commands pass on aspeed-2700 via libmctp-core.

**Note:** SDK libmctp (openbmc 0.11) `mctp_rx_fn` order: `(src_eid, tag_owner, msg_tag,
data, msg, len)` — `data` is 4th; NVIDIA fork reverses `data`/`msg`.

**Gate to Step C:** 4 control commands pass on aspeed-2700 with libmctp-core engine.
No regression on the step-5 baseline.

### Step C — Demux socket server + pldmd integration

**Scope:** new `mctp/mctp_demux_server.{hpp,cpp}` (NOT `ffs/` — pure Unix socket IPC,
no FunctionFS dependency; placing it in `ffs/` would invert the app→ffs→mctp DAG).
mctpusbd listens on a Unix socket; pldmd connects using `mctp-demux` transport.

#### Pinned wire protocol (libpldm dialect) — confirmed 2026-06-25

Source: `/home/daryl/work/project/obmc_new/libs/libpldm/src/transport/mctp-demux.c`

**Key delta from NVIDIA `mctp-demux-daemon.c`**: the two are different protocols.
pldmd uses libpldm's dialect (2-byte prefix, no tag byte), NOT the NVIDIA 3-byte format.

| | NVIDIA daemon | libpldm (what pldmd actually uses) |
|---|---|---|
| Socket path | `"\0mctp-*-mux"` (per-binding) | `"\0mctp-mux"` (fixed) |
| TX prefix (client→server) | `[tag_info][eid][type]` = 3 bytes | `[eid][type]` = 2 bytes |
| RX prefix (server→client) | `[(tag<<3)\|owner][eid][type]` = 3 bytes | `[eid][type]` = 2 bytes |

**Wire format**:
- Socket: `AF_UNIX`, `SOCK_SEQPACKET`, abstract path `"\0mctp-mux"`
- Registration (first write after connect): `[0x01]` — 1 byte, PLDM msg type
- RX (server → pldmd): `[src_eid][0x01][pldm_payload...]`
  where `[0x01][pldm_payload]` = full MCTP message body from libmctp `rx_all_cb`
- TX (pldmd → server): `[dest_eid][0x01][pldm_payload...]`
  mctpusbd strips `dest_eid`, calls `mctp_message_tx(mctp, dest_eid, false, saved_tag,
  buf+1, len-1)` where `buf+1` = `[0x01][pldm_payload]`

**Tag tracking** (no tag in socket protocol):
- `rx_all_cb` type `0x01`: save `{src_eid, msg_tag}` (host sends TO=1, tag=N)
- On TX response: use saved `msg_tag`, pass `tag_owner=false`
- v1: single pending-request slot (one PLDM request in flight at a time — sufficient for device-side responder)

#### Implementation design

```
DemuxServer (mctp/mctp_demux_server.{hpp,cpp}) — pure socket layer
  server_fd() / client_fd()         ← fds for poll loop
  accept_client()                   ← reads 1-byte registration, stores type
  forward_to_client(eid, mctp_msg)  ← sendmsg [src_eid][msg...]
  recv_from_client() → optional<{dest_eid, mctp_msg}>

FfsBinding::Impl additions
  DemuxServer* demux_server         ← optional; null = type 0x01 still dropped
  uint8_t pending_eid, pending_tag  ← single-slot tag tracking

ffs_daemon.hpp: add PollExt
  struct PollExt {
      function<void(vector<pollfd>&)>      collect;   // add fds before poll()
      function<void(span<const pollfd>)>   dispatch;  // handle revents after poll()
  };
  ffs_serve(..., PollExt poll_ext = {})
  (pollfds array → vector; DemuxServer provides the two lambdas)
```

Sub-steps:
1. [x] Read `libpldm/transport/mctp-demux.c` — protocol pinned (see above).
2. Implement `DemuxServer` — listen + accept + registration + forward/recv.
   Unit-testable in isolation (no libmctp dependency).
3. Extend `FfsBinding`: add `DemuxServer*` + tag slot; `rx_all_cb` type `0x01` → forward.
4. Extend `ffs_serve`: `PollExt`; `mctpusbd.cpp` constructs `DemuxServer` + wires lambdas.
5. Build pldmd with `transport-implementation=mctp-demux` (Yocto recipe).
6. End-to-end test: host sends a PLDM Get TID request; pldmd responds; verify on host.
7. Update `GetMessageTypeSupport` to return `[0x00, 0x01]`.

**Gate:** at least one PLDM command (Get TID or Get Terminus UID) round-trips correctly
end-to-end on aspeed-2700.

---

## Pre-implementation research still needed

- [x] Decide libmctp fork: **NVIDIA fork** confirmed (already in use as subproject).
      `mctp_rx_fn` order is identical to openbmc 0.11; NVIDIA fork is a strict superset.
- [x] Confirm which libmctp version pldmd's libpldm was built against: **not applicable** —
      pldm/libpldm does not link libmctp at all; it speaks the demux socket protocol only.
- [x] Read `libpldm/transport/mctp-demux.c` (and NVIDIA `mctp-demux-daemon.c`) — protocol
      pinned 2026-06-25. libpldm uses a **different** dialect from the NVIDIA daemon:
      `"\0mctp-mux"` socket, 2-byte `[eid][type]` prefix (no tag byte). Full details in
      Step C above.

---

## What this does NOT change

- `ffsd` and `gadget.sh` — untouched, permanent debug path
- `ffs_daemon.{hpp,cpp}` — the FunctionFS event loop; Step C adds a lightweight `PollExt`
  parameter (collect/dispatch lambdas for extra fds); the existing interface is unchanged
- `ffs_descriptors.hpp` — descriptor blob unchanged
- `mctpctrl.{hpp,cpp}` and its tests — the control responder is demoted to a handler,
  its logic and tests are unchanged
- libusbgx gadget lifecycle (`mctpusbd.cpp` app layer) — L1 is orthogonal to L2/L3 engine
