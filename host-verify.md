# host-verify — verifying MCTP responses from the host side

Host-side procedure to confirm the device actually answers MCTP control commands.
It is **the same regardless of which device daemon is running** — `mctpusbd`
([`mctpusbd.md`](./mctpusbd.md)) or `ffsd` + `gadget.sh` ([`ffsd.md`](./ffsd.md)).
Bring the device up first, then run this.

Everything here runs in the **same guest** (`dummy_hcd` loops host and device inside
one kernel). Run as root.

## 1. Let libusb claim the interface

The interface is class `0x14`. If the guest kernel's in-kernel `mctp-usb` **host**
driver binds it first, libusb (and the demux daemon) cannot claim it.

```sh
lsusb -t                              # the class-0x14 interface should show Driver=[none]
rmmod mctp_usb 2>/dev/null || true    # if a driver is bound, unbind it
```

## 2. Find the port path (the gotcha that bites)

libmctp's USB binding addresses the device by **port path**, e.g. `1-1` (bus 1, root
port 1) — **NOT** the `Dev <n>` number. The demux daemon and the requester must use
the same value.

```sh
ls /sys/bus/usb/devices/                  # find the entry, e.g. 1-1
cat /sys/bus/usb/devices/1-1/product      # "MCTP FFS device" confirms which one
```

## 3. Start the demux daemon

```sh
mctp-demux-daemon usb port_path=1-1 mode=0 -v &
```

## 4. Send the control commands

Use `daryl-mctp-usb-tester` (preferred) or `mctp-ctrl`. The four a simple endpoint
answers: **Set Endpoint ID, Get Endpoint ID, Get Endpoint UUID, Get Message Type
Support**.

```sh
# example with mctp-ctrl (port path 1-1, EID 9)
mctp-ctrl -t 3 -w 1-1 -e 9 -m 0 -b "00" -s "<set-eid payload>"   # Set EID = 9
# then Get EID (expect 9), Get UUID, Get Message Type Support
```

Expected: every command gets a correct response, and a Set EID is reflected by a
later Get EID. The device daemon prints a decoded RX/TX trace for each transfer.

## Cautions / gotchas

- **`port_path = 1-1`**, not the `Dev <n>` number; demux daemon and requester must
  agree. A mismatch makes libmctp silently ignore the device (no response at all).
- **The kernel `mctp-usb` host driver must not own the class-0x14 interface**, or
  libusb can't claim it. (STRATEGY §4.)
- All four control responses are single-packet (< 64 B); large / multi-packet
  payloads are out of scope for the current device.
