# mctpusbd

Makes the machine appear as an **MCTP USB device** and answers MCTP control requests.
One process does everything — gadget + FunctionFS + MCTP — so there is no `gadget.sh`
to run. This is the binary for real hardware (systemd).

## Quick start

As **root**, in the guest:

```sh
modprobe dummy_hcd libcomposite      # dev: provide a virtual UDC + gadget support
./mctpusbd --udc dummy_udc.0         # start (Ctrl-C tears everything down cleanly)
lsusb                                # → 1d6b:0104 daryl MCTP FFS device
```

The device is now up and answering MCTP control. To drive the control commands from
the host, see [`host-verify.md`](./host-verify.md).

On real hardware you can usually drop `--udc` (it auto-picks the only UDC):

```sh
./mctpusbd
```

## Options

```
mctpusbd [--echo] [--udc <name>] [<mountpoint>]
```

| Option | Default | Meaning |
|--------|---------|---------|
| `--udc <name>` | first UDC | Which UDC to bind. Pass it whenever more than one exists (e.g. `dummy_udc.0` for dev). |
| `--echo` | off | Raw bulk loopback instead of the MCTP responder — a byte-moving test. |
| `<mountpoint>` | `/dev/ffs-mctp` | FunctionFS mountpoint. |

Exit: `0` clean, `1` error, `2` bad arguments.

---

- **Build & deploy:** [`RUNBOOK.md`](./RUNBOOK.md)
- **Verify from the host:** [`host-verify.md`](./host-verify.md)
- **How it works, prerequisites in depth, troubleshooting:** [`mctpusbd-notes.md`](./mctpusbd-notes.md)
