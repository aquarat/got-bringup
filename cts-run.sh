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
#   MESA=local   -> run against /home/aquarat/Projects/mesa/install (VK_DRIVER_FILES)
#   MESA=system  -> run against system Mesa (default)
#   OUTDIR       -> where to put logs (default /home/aquarat/Projects/cts-results)

set -u
CTS_DIR=/home/aquarat/Projects/VK-GL-CTS
VKDIR="$CTS_DIR/build/external/vulkancts/modules/vulkan"
OUTDIR="${OUTDIR:-/home/aquarat/Projects/cts-results}"
MESA="${MESA:-system}"

LABEL="$1"; shift
SEL="$1"; shift

mkdir -p "$OUTDIR"

if [ "$MESA" = "local" ]; then
    export VK_DRIVER_FILES=/home/aquarat/Projects/mesa/install/share/vulkan/icd.d/asahi_icd.aarch64.json
    export LD_LIBRARY_PATH="/home/aquarat/Projects/mesa/install/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
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
