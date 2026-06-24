#!/bin/sh
# Staged cross-build for the OpenBMC target + run the unit tests under qemu-user.
#
# Prerequisite: source the SDK first, e.g.  `sdk-daryl`  (sets CXX, SDKTARGETSYSROOT
# and puts the SDK's meson wrapper + qemu-aarch64 on PATH). The meson wrapper adds
# the cross/native files automatically, so the project's plain meson.build is used.
#
# `meson test` itself skips cross binaries (the SDK cross file sets
# needs_exe_wrapper with no wrapper), so we run them explicitly under qemu.
set -eu

if [ -z "${SDKTARGETSYSROOT:-}" ]; then
	echo "error: SDK not sourced (run e.g. 'sdk-daryl' first)" >&2
	exit 1
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/build-sdk"

if [ -d "$build" ]; then
	meson setup --reconfigure "$build" "$root" >/dev/null
else
	meson setup "$build" "$root" >/dev/null
fi
ninja -C "$build"

echo "=== unit tests under qemu-aarch64 (target ABI) ==="
fail=0
for t in "$build"/test_*; do
	case "$t" in
	*.p) continue ;; # meson's private object dirs
	esac
	[ -x "$t" ] && [ -f "$t" ] || continue
	name=$(basename "$t")
	if out=$(qemu-aarch64 -L "$SDKTARGETSYSROOT" "$t" 2>&1); then
		printf "  OK   %-16s %s\n" "$name" "$out"
	else
		printf "  FAIL %-16s %s\n" "$name" "$out"
		fail=1
	fi
done
[ "$fail" -eq 0 ] && echo "ALL OK"
exit "$fail"
