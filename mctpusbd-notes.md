# mctpusbd — notes, internals & troubleshooting

The quickstart is in [`mctpusbd.md`](./mctpusbd.md). This file is the deeper stuff:
prerequisites, what happens internally, and how to get unstuck.

## Prerequisites

`mctpusbd` does **not** `modprobe` or mount configfs itself — do it first (as root):

```sh
modprobe dummy_hcd                                       # dev only: provides dummy_udc.0
modprobe libcomposite                                    # usb_gadget configfs support
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
ls /sys/class/udc/                                       # must list at least one UDC
```

Kernel config: the guest must **not** have the legacy `CONFIG_USB_FUNCTIONFS` (`g_ffs`)
gadget — it forces functionfs into single-device mode and blocks `mctpusbd`. See
[`RUNBOOK.md`](./RUNBOOK.md) prerequisites.

## How it works

On start, in order:

1. `usbg_init` → create gadget `mctp` (VID:PID `1d6b:0104`, strings
   `daryl` / `MCTP FFS device` / `0123456789`).
2. Create a FunctionFS function (instance `mctp`) + config `1`, and link them.
3. `mount -t functionfs mctp <mountpoint>` so `ep0` appears.
4. Write the MCTP descriptors + strings to `ep0` (interface class `0x14` + two bulk
   endpoints; `iInterface` = "MCTP over USB (DSP0283)").
5. **Enable the UDC** (`--udc`, or auto-pick) — enumeration happens here.
6. Service `ep0` events; on `ENABLE` open `ep1`(OUT)/`ep2`(IN) and run the data loop.

**Teardown:** on `SIGINT`/`SIGTERM` it reverses — disable UDC → unmount functionfs →
remove the gadget (`usbg_rm_gadget` recursive) → `usbg_cleanup`. So Ctrl-C self-cleans;
no `gadget.sh down` needed. (A `kill -9` skips this and can leave a stale gadget/mount.)

**Data loop:** default = MCTP responder (read OUT → `mctpep::process` → write IN,
answering Set/Get EID, Get UUID, Get Message Type Support); `--echo` = raw loopback.
It logs verbosely — every `ep0` event plus a decoded hex trace of each transfer.

## Choosing the UDC

With no `--udc`, libusbg binds the **first** UDC — correct when there is exactly one
(dev `dummy_udc.0`, or a lone real UDC). With more than one (e.g. `aspeed-vhub` +
`dummy_udc.0`), the first may be the one the platform image already uses, so pass
`--udc dummy_udc.0` (dev) or the explicit hardware UDC.

## Reading `lsusb` output

```sh
lsusb -v -d 1d6b:0104    # bInterfaceClass 0x14, 2 bulk EPs, iInterface "MCTP over USB (DSP0283)"
lsusb -t                 # the interface: Class=[unknown] (= 0x14), Driver=[none]
```

- `Class=[unknown]` — `lsusb`'s `usb.ids` has no name for class `0x14`; the class code
  is correct.
- `Driver=[none]` — no host kernel driver claimed the interface, which is what the
  libusb/demux host path wants.

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `create_gadget` fails | `libcomposite` not loaded, or a `mctp` gadget already exists |
| `create_function(ffs): Busy` — fresh boot, empty configfs | the legacy `CONFIG_USB_FUNCTIONFS`/`g_ffs` gadget is built-in → functionfs is in single-device mode (fails name-independently). Rebuild the kernel without it; keep `USB_CONFIGFS_F_FS`. |
| `create_function(ffs): Busy` — after a prior run | a leftover `mctp` gadget/mount — clean it (below) |
| `mount functionfs ... failed` | configfs not mounted |
| `enable_gadget` fails | no UDC present (`dummy_hcd` not loaded); check `dmesg` |
| `UDC '<name>' not found` | the `--udc` name is not in `/sys/class/udc` |
| nothing comes back over MCTP | host-side — see [`host-verify.md`](./host-verify.md) |

Inspect and clean a leftover gadget (root):

```sh
ls /sys/kernel/config/usb_gadget/                       # is 'mctp' present?
cat /sys/kernel/config/usb_gadget/mctp/UDC 2>/dev/null  # non-empty = still enabled

G=/sys/kernel/config/usb_gadget/mctp
echo "" > "$G/UDC" 2>/dev/null                          # unbind the UDC first
find "$G/configs" -maxdepth 2 -type l -exec rm {} + 2>/dev/null
rmdir "$G"/configs/*/strings/* "$G"/configs/* 2>/dev/null
rmdir "$G"/functions/* "$G"/strings/* 2>/dev/null
umount /dev/ffs-mctp 2>/dev/null
rmdir "$G" 2>/dev/null
```

## Relationship to `ffsd`

`ffsd` and `mctpusbd` share the same MCTP core and FunctionFS event loop (`ffs_serve`).
`ffsd` ([`ffsd.md`](./ffsd.md)) is the step-by-step debug path (`gadget.sh` does the
gadget); `mctpusbd` is the one-command, self-cleaning path. Use `ffsd` to isolate
plumbing-vs-descriptor problems, `mctpusbd` for real bring-up.
