#!/bin/bash
# Run a subset of the Vulkan CTS against Honeykrisp, headless, and summarise.
#
#   ./cts-run.sh <label> <caselist-file-or-glob> [extra deqp args...]
#
# Examples:
#   ./cts-run.sh smoke      'dEQP-VK.api.smoke.*'
#   ./cts-run.sh interlock  @/path/to/fragment-shader-interlock.txt
#
# Env:
#   MESA=local   -> run against $MESA_LOCAL/install (default ~/Projects/mesa-local),
#                   i.e. whatever build-driver.sh last built
#   MESA=system  -> run against system Mesa (default)
#   CTS_DIR      -> VK-GL-CTS checkout (default ~/Projects/VK-GL-CTS)
#   OUTDIR       -> where to put logs (default ~/Projects/cts-results)

set -u
CTS_DIR="${CTS_DIR:-$HOME/Projects/VK-GL-CTS}"
VKDIR="$CTS_DIR/build/external/vulkancts/modules/vulkan"
OUTDIR="${OUTDIR:-$HOME/Projects/cts-results}"
MESA="${MESA:-system}"

LABEL="$1"; shift
SEL="$1"; shift

mkdir -p "$OUTDIR"

if [ "$MESA" = "local" ]; then
    # Must match build-driver.sh's MESA_LOCAL (default $HOME/Projects/mesa-local).
    # This used to name $HOME/Projects/mesa, which build-driver.sh never creates.
    #
    # asahi_icd.local.json, not asahi_icd.aarch64.json: the latter is meson's,
    # and because the build sets --prefix=/usr it names /usr/lib64 -- i.e. the
    # DISTRO driver. Using it here would run the whole suite against stock
    # Honeykrisp while the label said "local". build-driver.sh writes the
    # .local.json alongside it for exactly this reason.
    MESA_LOCAL="${MESA_LOCAL:-$HOME/Projects/mesa-local}"
    ICD="$MESA_LOCAL/install/share/vulkan/icd.d/asahi_icd.local.json"
    [ -f "$ICD" ] || { echo "no $ICD -- run build-driver.sh first" >&2; exit 1; }
    grep -q "$MESA_LOCAL" "$ICD" || { echo "$ICD does not point into $MESA_LOCAL" >&2; exit 1; }
    export VK_DRIVER_FILES="$ICD"
    export LD_LIBRARY_PATH="$MESA_LOCAL/install/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
    # Pin to the system Asahi/Honeykrisp ICD. Fedora also ships lavapipe
    # (llvmpipe), which deqp-vk would otherwise be free to enumerate --
    # and lavapipe supports extensions Honeykrisp does not.
    export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/asahi_icd.aarch64.json
fi

QPA="$OUTDIR/$LABEL.qpa"
LOG="$OUTDIR/$LABEL.log"

ARGS=(--deqp-surface-type=fbo
      --deqp-log-images=disable
      --deqp-log-shader-sources=disable
      --deqp-log-flush=disable
      --deqp-log-filename="$QPA")

case "$SEL" in
  @*) ARGS+=(--deqp-caselist-file="${SEL#@}") ;;
  *)  ARGS+=(--deqp-case="$SEL") ;;
esac

cd "$VKDIR" || exit 1
START=$(date +%s)
./deqp-vk "${ARGS[@]}" "$@" > "$LOG" 2>&1
RC=$?
END=$(date +%s)

echo "=== $LABEL (MESA=$MESA) rc=$RC elapsed=$((END-START))s ==="
grep -oP '(?<=<Result StatusCode=")[^"]+' "$QPA" | sort | uniq -c | sort -rn
echo "total: $(grep -c '<Result StatusCode=' "$QPA")"
