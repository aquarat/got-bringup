#!/bin/bash
# Swap the SYSTEM Honeykrisp driver for the locally-built one, and back.
#
# WHY THIS IS NEEDED
# ------------------
# Pointing VK_DRIVER_FILES at a local build does NOT reach the game. Verified
# repeatedly (CHECKLIST.md items 6, 7, 7a, 7b):
#   - FEX's ThunksDB overlays libvulkan.so.1 only at fixed, standard paths.
#   - The game loads pressure-vessel's IMPORTED copy of the HOST driver:
#       /run/pressure-vessel/.../var/pressure-vessel/gfx/main/usr/lib64/libvulkan_asahi.so
#   - muvm's --execute-pre hook, which would allow a guest-scoped bind mount,
#     is broken in muvm-0.6.0-3 (any use aborts the launch).
#   - PRESSURE_VESSEL_FILESYSTEMS_RO does not help either (tested).
#
# pressure-vessel imports from the host's standard library path, so replacing
# the file there is the one mechanism that actually delivers a custom driver
# to the game.
#
# THE RISK
# --------
# /usr/lib64/libvulkan_asahi.so is ALSO what KWin and every desktop Vulkan
# client uses. This deliberately inverts the safety property in STATE.md
# ("the desktop still uses system Mesa ... so a bad build can never cost you
# your compositor"). If the local build is broken, the desktop can go down.
#
# Recovery if that happens: switch to a TTY (Ctrl-Alt-F3), log in, run
#     "$GOT_BRINGUP"/deploy-system-driver.sh restore
# then restart the session. The backup is a plain file copy, so this works
# even with no working GPU driver.
#
# USAGE
#   ./deploy-system-driver.sh deploy [SRC]   # SRC defaults to mesa-local
#   ./deploy-system-driver.sh restore
#   ./deploy-system-driver.sh status
#
# BUILD THE SOURCE FIRST
#   ./build-driver.sh      clones the Mesa fork pinned in mesa-source.env and
#                          builds it into exactly the layout SRC_DEFAULT expects.
#
# FOR ANYONE WHO IS NOT DEVELOPING THE DRIVER
#   The RPM in INSTALL.md does the same swap with the rough edges taken off: it
#   replaces the file by rename rather than rewriting pages mapped into a live
#   compositor, refreshes the backup so a restore after a mesa update does not
#   roll the system back to an older Mesa, and re-applies itself when a dnf
#   upgrade puts the distro driver back. This script is kept for the case it was
#   written for -- swapping in a build you just made, over and over.

set -uo pipefail

SYS=/usr/lib64/libvulkan_asahi.so
BACKUP=/usr/lib64/libvulkan_asahi.so.distro-backup
# Override with MESA_LOCAL, or pass the .so as the second argument.
MESA_LOCAL="${MESA_LOCAL:-$HOME/Projects/mesa-local}"
SRC_DEFAULT="$MESA_LOCAL/install/lib64/libvulkan_asahi.so"

case "${1:-status}" in

deploy)
    SRC="${2:-$SRC_DEFAULT}"
    [ -f "$SRC" ] || { echo "no such driver: $SRC" >&2; exit 1; }

    # Refuse to deploy something that cannot even enumerate the GPU.
    echo "sanity-checking $SRC before touching the system..."
    tmpicd=$(mktemp /tmp/icd-XXXX.json)
    printf '{"ICD":{"api_version":"1.4.359","library_arch":"64","library_path":"%s"},"file_format_version":"1.0.1"}\n' "$SRC" > "$tmpicd"
    if ! VK_DRIVER_FILES="$tmpicd" vulkaninfo --summary 2>/dev/null | grep -q 'Apple M1'; then
        rm -f "$tmpicd"
        echo "REFUSING: that driver does not enumerate the GPU. Not deploying." >&2
        exit 1
    fi
    rm -f "$tmpicd"
    echo "  ok, it enumerates the GPU"

    if [ ! -f "$BACKUP" ]; then
        sudo cp -a "$SYS" "$BACKUP" || exit 1
        echo "backed up distro driver -> $BACKUP"
    else
        echo "backup already exists (keeping the original distro copy)"
    fi

    sudo cp -f "$SRC" "$SYS" || exit 1
    echo "deployed: $SRC -> $SYS"
    echo
    echo "Launch the game with:  ASAHI_LOCAL_MESA=0 steam"
    echo "  (ASAHI_LOCAL_MESA=0 is now CORRECT and REQUIRED -- it stops the"
    echo "   wrapper setting VK_DRIVER_FILES, so everything uses the system"
    echo "   path, which is now your build.)"
    echo
    echo "Revert with: $0 restore"
    ;;

restore)
    [ -f "$BACKUP" ] || { echo "no backup at $BACKUP - nothing to restore" >&2; exit 1; }
    sudo cp -f "$BACKUP" "$SYS" || exit 1
    echo "restored the distro driver from $BACKUP"
    ;;

status)
    echo "system driver : $SYS"
    echo "  size        : $(stat -c %s "$SYS" 2>/dev/null) bytes"
    echo "  mtime       : $(stat -c %y "$SYS" 2>/dev/null)"
    if [ -f "$BACKUP" ]; then
        if cmp -s "$SYS" "$BACKUP"; then
            echo "  state       : DISTRO (matches backup)"
        else
            echo "  state       : LOCAL BUILD DEPLOYED (differs from backup)"
        fi
    else
        echo "  state       : untouched (no backup taken yet)"
    fi
    echo
    export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/1000}
    unset DISPLAY WAYLAND_DISPLAY
    echo "what the system path currently reports:"
    VK_DRIVER_FILES=/usr/share/vulkan/icd.d/asahi_icd.aarch64.json \
        vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverInfo' | sed 's/^/  /'
    ;;

*)
    sed -n '2,40p' "$0"
    ;;
esac
