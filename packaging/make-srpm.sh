#!/bin/bash
# Produce a source RPM for the patched Honeykrisp driver.
#
# Everything the package needs comes from two places and nowhere else:
#   * the Mesa commit pinned in ../mesa-source.env, exported with git archive
#   * the files in this directory
#
# so the SRPM is reproducible from the repository alone and can be rebuilt by
# anyone with "rpmbuild --rebuild".
#
# USAGE
#   ./make-srpm.sh [OUTDIR]        # default: ./srpm
#
# ENVIRONMENT
#   MESA_LOCAL   an existing mesa checkout to export from, instead of cloning

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUTDIR=$(mkdir -p "${1:-$HERE/srpm}" && cd "${1:-$HERE/srpm}" && pwd)

# shellcheck source=../mesa-source.env
. "$ROOT/mesa-source.env"

MESA_SHORT=${MESA_COMMIT:0:12}
WORK=$(mktemp -d /tmp/hkgot-srpm-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

# ------------------------------------------------------------------ the source

SRC="${MESA_LOCAL:-}"
if [ -z "$SRC" ] || [ ! -d "$SRC/.git" ]; then
    SRC="$WORK/mesa"
    echo "==> cloning $MESA_REPO ($MESA_BRANCH)"
    git clone --quiet --branch "$MESA_BRANCH" "$MESA_REPO" "$SRC"
fi
git -C "$SRC" cat-file -e "$MESA_COMMIT^{commit}" 2>/dev/null ||
    git -C "$SRC" fetch --quiet origin "$MESA_BRANCH"
git -C "$SRC" cat-file -e "$MESA_COMMIT^{commit}" 2>/dev/null || {
    echo "make-srpm: $MESA_COMMIT is not reachable in $MESA_REPO $MESA_BRANCH" >&2
    exit 1
}

echo "==> exporting mesa $MESA_SHORT"
git -C "$SRC" archive --format=tar --prefix="mesa-$MESA_SHORT/" "$MESA_COMMIT" \
    | gzip -9 > "$WORK/mesa-$MESA_SHORT.tar.gz"

# ------------------------------------------------------------------- the spec

# The date is the pinned commit's, not today's: two runs of this script over the
# same pin must produce the same package, and a build timestamp would break that.
SNAPDATE=$(git -C "$SRC" show -s --format=%cd --date=format:%Y%m%d "$MESA_COMMIT")
RPMDATE=$(LC_ALL=C git -C "$SRC" show -s --format=%cd --date=format:'%a %b %d %Y' "$MESA_COMMIT")

sed -e "s|@MESA_COMMIT@|$MESA_COMMIT|g" \
    -e "s|@MESA_SHORT@|$MESA_SHORT|g" \
    -e "s|@MESA_VERSION@|$MESA_VERSION|g" \
    -e "s|@SNAPDATE@|$SNAPDATE|g" \
    -e "s|@RPMDATE@|$RPMDATE|g" \
    "$HERE/honeykrisp-got.spec.in" > "$WORK/honeykrisp-got.spec"

# --------------------------------------------------------------------- rpmbuild

TOPDIR="$WORK/rpmbuild"
mkdir -p "$TOPDIR"/{SOURCES,SPECS,SRPMS}
cp "$WORK/mesa-$MESA_SHORT.tar.gz" "$TOPDIR/SOURCES/"
cp "$HERE/honeykrisp-got" "$TOPDIR/SOURCES/"
cp "$ROOT/drirc-hk-vkd3d.conf" "$TOPDIR/SOURCES/"
cp "$HERE/README.package.md" "$TOPDIR/SOURCES/"
cp "$WORK/honeykrisp-got.spec" "$TOPDIR/SPECS/"

echo "==> rpmbuild -bs"
rpmbuild --define "_topdir $TOPDIR" -bs "$TOPDIR/SPECS/honeykrisp-got.spec"

cp "$TOPDIR"/SRPMS/*.src.rpm "$OUTDIR/"
echo
echo "srpm: $(ls "$OUTDIR"/*.src.rpm)"
