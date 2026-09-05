#!/bin/bash
# Decide whether a run actually RENDERED, and whether it rendered the same thing
# as a reference run.
#
#   ./verify-frame.sh runs/<name> [runs/<reference>]
#   ./verify-frame.sh runs/norobust runs/shaders
#
# WHY THIS EXISTS
# A performance number from a run that rendered a black screen is worse than no
# number: it looks like a result. HK_PERFTEST=noflush produced 71 fps and then a
# GPU job timeout -- the frame rate was real, the frames were not. Every flag on
# this branch that changes barriers, robustness or tessellation can silently
# stop the game drawing while the harness happily reports throughput.
#
# WHAT IT CAN AND CANNOT TELL YOU
# Two gameplay screenshots from different runs are never pixel-identical -- the
# camera, the weather and the time of day all move -- so a pixel diff against a
# reference is not evidence of a rendering bug. What IS reliable is detecting a
# frame that has stopped being an image at all:
#
#   mean near 0            black screen
#   std dev near 0         flat / uniform fill, nothing drawn
#   entropy collapsed      posterised or corrupt
#
# The reference comparison is therefore reported as a number to look at, not a
# pass/fail. Anything flagged should be eyeballed; the thumbnails exist for that.
set -uo pipefail
RUN="${1:?usage: verify-frame.sh runs/<name> [runs/<reference>]}"
REF="${2:-}"

# mean and stddev normalised 0..1, plus the number of distinct colours at 256px
# wide. Unique-colour count is the detail proxy: a real frame has thousands, a
# black or posterised one has a handful, and unlike entropy it is a property
# ImageMagick reports reliably here.
stat_of() {
    local ms k
    ms=$(magick "$1" -resize 512x -colorspace sRGB \
            -format "%[fx:mean] %[fx:standard_deviation]" info: 2>/dev/null)
    k=$(magick "$1" -resize 256x -format "%k" info: 2>/dev/null)
    echo "${ms:-0 0} ${k:-0}"
}

verdict() {  # $1 mean  $2 std  $3 unique colours
    awk -v m="$1" -v s="$2" -v k="$3" 'BEGIN{
        if (m < 0.02) { print "BLACK"; exit }
        if (s < 0.03) { print "FLAT"; exit }
        if (k < 500)  { print "LOW-DETAIL"; exit }
        print "ok"
    }'
}

echo "=== $RUN ==="
printf '%-22s %8s %8s %9s  %s\n' image mean stddev colours verdict
shopt -s nullglob
for img in "$RUN"/*.png; do
    [ "$(basename "$img")" = "thumbs.png" ] && continue
    read -r m s k <<< "$(stat_of "$img")"
    printf '%-22s %8.4f %8.4f %9d  %s\n' "$(basename "$img")" "$m" "$s" "$k" "$(verdict "$m" "$s" "$k")"
done

# A small thumbnail strip so the frames can actually be looked at without
# opening 2.5 MB screenshots one at a time.
THUMB="$RUN/thumbs.png"
imgs=(); for f in "$RUN"/*.png; do [ "$(basename "$f")" = "thumbs.png" ] || imgs+=("$f"); done
if [ ${#imgs[@]} -gt 0 ]; then
    magick montage "${imgs[@]}" -tile 4x -geometry 320x200+4+4 -background '#202020' \
        -label '%f' "$THUMB" 2>/dev/null && echo "thumbnails: $THUMB"
fi

if [ -n "$REF" ] && [ -f "$REF/02-gameplay.png" ] && [ -f "$RUN/02-gameplay.png" ]; then
    echo
    echo "gameplay frame vs $REF (both downscaled to 64x40 to ignore camera motion):"
    for metric in RMSE NCC; do
        v=$(magick compare -metric "$metric" \
              \( "$RUN/02-gameplay.png" -resize 64x40! -colorspace gray \) \
              \( "$REF/02-gameplay.png" -resize 64x40! -colorspace gray \) \
              null: 2>&1 | tr -d '()')
        printf '  %-5s %s\n' "$metric" "$v"
    done
    echo "  (NCC near 1 = same kind of image. A low NCC is a prompt to LOOK, not a failure.)"
fi
