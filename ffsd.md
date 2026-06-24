# ffsd — usage (script-driven, with `gadget.sh`)

`ffsd` is the **step-by-step** FunctionFS MCTP daemon. The USB-gadget lifecycle is
done externally by `scripts/gadget.sh` (configfs plumbing + UDC bind); `ffsd` only
writes the descriptors and runs the MCTP event loop. Use this path to isolate
**plumbing-vs-descriptor** problems. For the one-command, self-contained daemon see
[`mctpusbd.md`](./mctpusbd.md).

Build + deploy: see [`RUNBOOK.md`](./RUNBOOK.md). Run as **root** in the guest.

## Run it

```sh
./gadget.sh up-ffs         # create ffs function + mount /dev/ffs-mctp (no UDC yet)
./ffsd /dev/ffs-mctp &     # write descriptors, service ep0
./gadget.sh bind           # attach the UDC -> enumeration happens
lsusb                      # expect 1d6b:0104; ffsd prints BIND / ENABLE
# ... test (below) ...
kill %1                    # stop ffsd
./gadget.sh down           # clean up (safe unwind order)
```

Modes: default = MCTP responder (`read OUT → mctpep::process → write IN`); `--echo` =
raw bulk loopback.

## Verify, stage by stage (optional)

Each stage is the same flow with a different check — do as many as you need.

### 1a — plumbing sanity check (no custom code)

Proves `configfs → UDC → lsusb` works, independent of `ffsd`:

```sh
./gadget.sh up-acm     # standard ACM gadget
lsusb                  # expect an ACM device
./gadget.sh down
```

### 2 — descriptors recognised

```sh
./gadget.sh up-ffs; ./ffsd /dev/ffs-mctp & ; ./gadget.sh bind
lsusb -v -d 1d6b:0104   # bInterfaceClass 0x14 + 2 bulk EPs; iInterface "MCTP over USB (DSP0283)"
```

### 3a — raw echo

```sh
./ffsd --echo /dev/ffs-mctp &
./gadget.sh bind
# from a libusb host: write N bytes to the OUT ep, read the same N back on IN
```

### 3b — MCTP responder

```sh
./ffsd /dev/ffs-mctp &     # default MCTP mode
./gadget.sh bind
```

Then verify the responses from the host side: see [`host-verify.md`](./host-verify.md).

## Interpreting results

- **1a up, 1b down** → plumbing is fine; the problem is `ffsd`'s descriptor blob.
  Check `dmesg` for UDC bind errors and `ffsd`'s ep0 write return. (This isolation is
  exactly why 1a exists.)
- **1a already down** → a plumbing / UDC / configfs problem; ignore `ffsd` for now.
- **Endpoints**: the interface has two bulk endpoints; if bind fails (ep0 write
  `EINVAL` or silent failure), check
  [`ffs/ffs_descriptors.hpp`](./ffs/ffs_descriptors.hpp) against `dmesg`.

## Notes

- `gadget.sh` `modprobe` is best-effort; it then verifies configfs + a UDC exist,
  failing with a clear message otherwise.
- `gadget.sh down` unwinds configfs in the safe order; run it before retrying to
  avoid a wedged gadget.
- Do **not** drive the same `mctp` gadget with both `gadget.sh` and `mctpusbd` — pick
  one path.
