# Runbook — building and deploying to the guest

> How to cross-build the device daemons and get them into the QEMU OpenBMC guest.
> Strategy lives in [`STRATEGY.md`](./STRATEGY.md); status in [`PROGRESS.md`](./PROGRESS.md).
>
> **Per-daemon operation lives in its own doc** (see below); this file is just
> build + deploy.

## Usage docs

| Doc | What it covers |
|-----|----------------|
| [`mctpusbd.md`](./mctpusbd.md) | `mctpusbd` — the self-contained one-command daemon (libusbgx) |
| [`ffsd.md`](./ffsd.md) | `ffsd` + `scripts/gadget.sh` — the step-by-step debug path |
| [`host-verify.md`](./host-verify.md) | verifying MCTP responses from the host (same for both daemons) |

> Two daemons, one shared core. **`ffsd`** isolates plumbing-vs-descriptor problems
> (`gadget.sh` does the gadget, `ffsd` does FunctionFS). **`mctpusbd`** folds the whole
> gadget lifecycle in-process — no `gadget.sh` — and is the production-shaped binary.

## Prerequisites

- **Guest kernel** (all `=y` is fine): `USB`, `USB_DUMMY_HCD`, `USB_GADGET`,
  `USB_LIBCOMPOSITE`, `USB_CONFIGFS`, `USB_CONFIGFS_F_FS`, `CONFIGFS_FS`.
  For the `ffsd` 1a ACM plumbing test also `USB_CONFIGFS_ACM` (pulls in `USB_U_SERIAL`).
- **Must NOT have `CONFIG_USB_FUNCTIONFS`** (the *legacy* FunctionFS gadget, a.k.a.
  `g_ffs`) built-in/loaded. It registers a "single" ffs device at boot, which puts
  functionfs into **single-device mode**; the kernel then returns `EBUSY` on every
  configfs (named) ffs function, so `mctpusbd`/`ffsd` fail at `create_function(ffs)`
  — name-independently, even with an empty configfs and a fresh boot. **Keep
  `USB_CONFIGFS_F_FS` (the configfs one), drop `USB_FUNCTIONFS` (the legacy one).**
  Hit on the aspeed-2700 image; fixed by rebuilding the kernel without it.
- **Build host**: the OpenBMC SDK sourced via `sdk-daryl` (aarch64 cortex-a57,
  gcc 16). The guest rootfs must be the same build as the SDK so the binaries' dynamic
  libs (libstdc++/libc) match.
- Run everything in the guest as **root** (writes `/sys`, mounts, opens ep0).

## 1. Build on the host

```sh
sdk-daryl                  # source the OpenBMC SDK
./scripts/cross-test.sh    # cross-compile + run unit tests under qemu; builds build-sdk/
```

Binaries only, skipping tests:

```sh
sdk-daryl && meson setup build-sdk && ninja -C build-sdk
```

> **libusbgx is a required dependency** (mctpusbd is the primary deliverable), so
> `meson setup` hard-errors if it is missing — there is no skip. It lives in the SDK
> sysroot, so the cross build above just works. A plain native host must have libusbgx
> installed before it can even configure; without it, run the unit tests through the
> cross build (`cross-test.sh` runs them under qemu).

## 2. Copy to the guest

| File | Source | Role |
|------|--------|------|
| `build-sdk/mctpusbd` | cross-compiled above | self-contained daemon (gadget + FFS in one) |
| `build-sdk/ffsd` | cross-compiled above | FunctionFS daemon, script-driven |
| `scripts/gadget.sh` | this repo | configfs plumbing for the `ffsd` path |

Transfer via scp / 9p share / baked into the image. The `test_*` binaries run on the
build host; the host-side `daryl-mctp-usb-tester` is only for the MCTP round-trip
(see [`host-verify.md`](./host-verify.md)).

## 3. Run

Pick a daemon and follow its doc:

- **`mctpusbd`** (one command, self-cleaning): [`mctpusbd.md`](./mctpusbd.md)
- **`ffsd` + `gadget.sh`** (step-by-step): [`ffsd.md`](./ffsd.md)

Then verify the MCTP responses from the host: [`host-verify.md`](./host-verify.md).
