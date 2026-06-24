#!/bin/sh
# USB gadget plumbing for dummy_hcd + configfs + FunctionFS.
#
# Usage:
#   gadget.sh up-acm
#       Create and bind a standard ACM gadget.
#
#   gadget.sh up-ffs
#       Create a FunctionFS gadget shell and mount FunctionFS.
#       This does NOT bind UDC.
#
#   gadget.sh bind
#       Bind the gadget to the first available UDC.
#       For FunctionFS, run ffsd first before binding.
#
#   gadget.sh down
#       Tear down gadget in a safe order.
#
# Expected FunctionFS sequence:
#   ./gadget.sh up-ffs
#   ./ffsd /dev/ffs-mctp &
#   ./gadget.sh bind
#
# Notes:
#   - This script does not implement MCTP.
#   - MCTP-over-USB descriptors, endpoint I/O, and framing belong in ffsd.

set -eu

CONFIGFS_ROOT=/sys/kernel/config
GADGET_ROOT="$CONFIGFS_ROOT/usb_gadget"
GADGET_NAME=mctp
G="$GADGET_ROOT/$GADGET_NAME"

FFS_NAME=mctp
FFS_MNT=/dev/ffs-mctp

CONFIG_NAME=c.1
CONFIG_DIR="$G/configs/$CONFIG_NAME"

ACM_FUNC=acm.0
FFS_FUNC="ffs.$FFS_NAME"

ACM_LINK="$CONFIG_DIR/$ACM_FUNC"
FFS_LINK="$CONFIG_DIR/$FFS_FUNC"

VID=0x1d6b       # Linux Foundation test VID
PID=0x0104       # Multifunction Composite Gadget test PID
BCD_USB=0x0200
BCD_DEVICE=0x0100

SERIAL=0123456789
MANUFACTURER=daryl
PRODUCT="MCTP FFS device"
CONFIG_STR="config 1"
MAX_POWER=120


log() {
    echo "[gadget] $*"
}

err() {
    echo "[gadget][error] $*" >&2
}

is_mounted() {
    mnt="$1"
    grep -q " $mnt " /proc/mounts 2>/dev/null
}

ensure_configfs_mounted() {
    if [ ! -d "$CONFIGFS_ROOT" ]; then
        err "$CONFIGFS_ROOT does not exist"
        exit 1
    fi

    if ! is_mounted "$CONFIGFS_ROOT"; then
        mount -t configfs none "$CONFIGFS_ROOT" 2>/dev/null || true
    fi

    if ! is_mounted "$CONFIGFS_ROOT"; then
        err "configfs is not mounted at $CONFIGFS_ROOT"
        exit 1
    fi
}

ensure_gadget_subsystem() {
    # Best-effort only. If built-in, modprobe may fail or be unavailable.
    modprobe dummy_hcd 2>/dev/null || true
    modprobe libcomposite 2>/dev/null || true

    ensure_configfs_mounted

    if [ ! -d "$GADGET_ROOT" ]; then
        err "$GADGET_ROOT unavailable; need USB_GADGET + USB_CONFIGFS/libcomposite"
        exit 1
    fi

    if ! ls /sys/class/udc 2>/dev/null | grep -q .; then
        err "no UDC found in /sys/class/udc; need dummy_hcd or real UDC"
        exit 1
    fi
}

get_first_udc() {
    ls /sys/class/udc 2>/dev/null | head -n1
}

is_bound() {
    [ -f "$G/UDC" ] && [ -n "$(cat "$G/UDC" 2>/dev/null || true)" ]
}

write_file() {
    file="$1"
    value="$2"

    if [ ! -e "$file" ]; then
        err "missing configfs attribute: $file"
        exit 1
    fi

    echo "$value" > "$file"
}

base_gadget() {
    ensure_gadget_subsystem

    if is_bound; then
        err "gadget is already bound; run '$0 down' first if you want to reconfigure"
        exit 1
    fi

    mkdir -p "$G"

    write_file "$G/idVendor" "$VID"
    write_file "$G/idProduct" "$PID"
    write_file "$G/bcdUSB" "$BCD_USB"
    write_file "$G/bcdDevice" "$BCD_DEVICE"

    mkdir -p "$G/strings/0x409"
    write_file "$G/strings/0x409/serialnumber" "$SERIAL"
    write_file "$G/strings/0x409/manufacturer" "$MANUFACTURER"
    write_file "$G/strings/0x409/product" "$PRODUCT"

    mkdir -p "$CONFIG_DIR/strings/0x409"
    write_file "$CONFIG_DIR/strings/0x409/configuration" "$CONFIG_STR"
    write_file "$CONFIG_DIR/MaxPower" "$MAX_POWER"
}

link_function() {
    func_path="$1"
    link_path="$2"

    if [ ! -d "$func_path" ]; then
        err "function path does not exist: $func_path"
        exit 1
    fi

    if [ -L "$link_path" ]; then
        return 0
    fi

    if [ -e "$link_path" ]; then
        err "link path exists but is not symlink: $link_path"
        exit 1
    fi

    ln -s "$func_path" "$link_path"
}

setup_acm() {
    base_gadget

    mkdir -p "$G/functions/$ACM_FUNC"
    link_function "$G/functions/$ACM_FUNC" "$ACM_LINK"

    udc="$(get_first_udc)"
    if [ -z "$udc" ]; then
        err "no UDC available"
        exit 1
    fi

    echo "$udc" > "$G/UDC"
    log "bound ACM gadget to UDC: $udc"
    log "check with: lsusb"
}

setup_ffs() {
    base_gadget

    mkdir -p "$G/functions/$FFS_FUNC"
    link_function "$G/functions/$FFS_FUNC" "$FFS_LINK"

    mkdir -p "$FFS_MNT"

    if is_mounted "$FFS_MNT"; then
        log "FunctionFS already mounted at $FFS_MNT"
    else
        mount -t functionfs "$FFS_NAME" "$FFS_MNT"
        log "FunctionFS mounted at $FFS_MNT"
    fi

    log "next step:"
    log "  run your ffsd first, for example:"
    log "    ./ffsd $FFS_MNT &"
    log "  then bind:"
    log "    $0 bind"
}

check_ffs_ready_if_needed() {
    if [ ! -d "$G/functions/$FFS_FUNC" ]; then
        return 0
    fi

    if ! is_mounted "$FFS_MNT"; then
        err "FunctionFS is not mounted at $FFS_MNT; run '$0 up-ffs' first"
        exit 1
    fi

    # ep0 is always present after mount. The bulk ep1/ep2 files are created by
    # the kernel at function bind (when the UDC is attached), i.e. as part of
    # the very `echo UDC` this function precedes — they do NOT exist yet here,
    # and only become usable after the host ENABLEs the config. So there is no
    # pre-bind endpoint file to check; rely on ffsd servicing ep0 instead.
}

bind_gadget() {
    ensure_gadget_subsystem

    if [ ! -d "$G" ]; then
        err "gadget not created; run '$0 up-acm' or '$0 up-ffs' first"
        exit 1
    fi

    if is_bound; then
        log "gadget already bound to: $(cat "$G/UDC")"
        return 0
    fi

    check_ffs_ready_if_needed

    udc="$(get_first_udc)"
    if [ -z "$udc" ]; then
        err "no UDC available"
        exit 1
    fi

    echo "$udc" > "$G/UDC"
    log "bound gadget to UDC: $udc"
    log "check with: lsusb"
}

remove_config_links() {
    if [ -d "$CONFIG_DIR" ]; then
        find "$CONFIG_DIR" -maxdepth 1 -type l -exec rm -f {} \; 2>/dev/null || true
    fi
}

teardown_gadget() {
    if [ ! -d "$G" ]; then
        log "gadget does not exist; nothing to tear down"
        return 0
    fi

    log "unbind UDC"
    echo "" > "$G/UDC" 2>/dev/null || true

    # Give gadget/ffs userspace a short window to observe disconnect.
    sleep 0.2

    log "unmount FunctionFS if mounted"
    umount "$FFS_MNT" 2>/dev/null || true

    log "remove function links"
    remove_config_links

    log "remove functions"
    rmdir "$G/functions/$FFS_FUNC" 2>/dev/null || true
    rmdir "$G/functions/$ACM_FUNC" 2>/dev/null || true

    log "remove config"
    rmdir "$CONFIG_DIR/strings/0x409" 2>/dev/null || true
    rmdir "$CONFIG_DIR" 2>/dev/null || true

    log "remove strings"
    rmdir "$G/strings/0x409" 2>/dev/null || true

    log "remove gadget"
    rmdir "$G" 2>/dev/null || true

    if [ -d "$G" ]; then
        err "gadget directory still exists; possible busy reference"
        err "check running ffsd/processes and mounted filesystems:"
        err "  ps | grep ffs"
        err "  cat /proc/mounts | grep functionfs"
        err "  ls -l $G"
        exit 1
    fi

    log "torn down"
}

case "${1:-}" in
up-acm)
    setup_acm
    ;;
up-ffs)
    setup_ffs
    ;;
bind)
    bind_gadget
    ;;
down)
    teardown_gadget
    ;;
*)
    echo "usage: $0 {up-acm|up-ffs|bind|down}" >&2
    exit 2
    ;;
esac