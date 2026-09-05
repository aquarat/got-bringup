#!/bin/bash
# Build the Honeykrisp driver this repository measures, from the pinned Mesa fork.
#
# WHY THIS EXISTS
# ---------------
# deploy-system-driver.sh has always defaulted to $HOME/Projects/mesa-local,
# a directory that exists on exactly one machine. Nothing said which repository
# or which commit that was. This script makes that link explicit: it reads
# mesa-source.env, checks out that exact commit, and builds it into the layout
# deploy-system-driver.sh already expects.
#
# It builds ONLY the Asahi Vulkan driver. No GL, no gallium, no video codecs.
# That is the whole of what gets deployed -- libvulkan_asahi.so is a
# self-contained ICD -- and it cuts the build to a fraction of a full Mesa.
#
# USAGE
#   ./build-driver.sh                 # clone/update, configure, build, install
#   ./build-driver.sh --pull          # move the checkout to the pinned commit even
#                                     #   if the worktree is already on another one
#   ./build-driver.sh --dirty         # build whatever is checked out, no fetch,
#                                     #   no pin check (for driver development)
#
# ENVIRONMENT
#   MESA_LOCAL   where to clone/build       (default $HOME/Projects/mesa-local)
#   JOBS         parallel compile jobs      (default: nproc)
#
# AFTERWARDS
#   ./deploy-system-driver.sh deploy
#
# If you only want the driver and not the source tree, you do not need this
# script at all -- INSTALL.md has an RPM.

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=mesa-source.env
. "$HERE/mesa-source.env"

MESA_LOCAL="${MESA_LOCAL:-$HOME/Projects/mesa-local}"
JOBS="${JOBS:-$(nproc)}"
MODE=pinned

for arg in "$@"; do
    case "$arg" in
    --pull)  MODE=pull  ;;
    --dirty) MODE=dirty ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

die() { echo "build-driver: $*" >&2; exit 1; }

for tool in git meson ninja; do
    command -v "$tool" >/dev/null || die "$tool is not installed. On Fedora:
    sudo dnf install -y git meson ninja-build
    sudo dnf builddep -y mesa"
done

# ---------------------------------------------------------------- source tree

if [ "$MODE" = dirty ]; then
    [ -d "$MESA_LOCAL/.git" ] || die "--dirty needs an existing checkout at $MESA_LOCAL"
    echo "==> building whatever is checked out at $MESA_LOCAL (--dirty)"
    echo "    HEAD: $(git -C "$MESA_LOCAL" log -1 --oneline)"
else
    if [ ! -d "$MESA_LOCAL/.git" ]; then
        echo "==> cloning $MESA_REPO ($MESA_BRANCH) into $MESA_LOCAL"
        mkdir -p "$(dirname "$MESA_LOCAL")" || die "cannot create $(dirname "$MESA_LOCAL")"
        git clone --branch "$MESA_BRANCH" "$MESA_REPO" "$MESA_LOCAL" || die "clone failed"
    fi

    # Fetch the pinned commit specifically. A shallow or old clone will not
    # have it, and "git checkout <sha>" would fail with a confusing message.
    if ! git -C "$MESA_LOCAL" cat-file -e "$MESA_COMMIT^{commit}" 2>/dev/null; then
        echo "==> fetching $MESA_COMMIT"
        git -C "$MESA_LOCAL" fetch origin "$MESA_BRANCH" || die "fetch failed"
    fi
    git -C "$MESA_LOCAL" cat-file -e "$MESA_COMMIT^{commit}" 2>/dev/null \
        || die "$MESA_COMMIT is not in $MESA_REPO $MESA_BRANCH.
    Either the pin in mesa-source.env is stale, or the branch was rewritten."

    HEAD_NOW=$(git -C "$MESA_LOCAL" rev-parse HEAD)
    if [ "$HEAD_NOW" != "$MESA_COMMIT" ]; then
        if [ "$MODE" != pull ]; then
            echo "$MESA_LOCAL is at $HEAD_NOW, the pin is $MESA_COMMIT." >&2
            echo "Re-run with --pull to move it, or --dirty to build it as-is." >&2
            exit 1
        fi
        echo "==> checking out $MESA_COMMIT"
        git -C "$MESA_LOCAL" checkout --detach "$MESA_COMMIT" || die "checkout failed"
    fi
fi

# ------------------------------------------------------------------- configure

BUILD="$MESA_LOCAL/build"
INSTALL="$MESA_LOCAL/install"

# --libdir=lib64 is not cosmetic: deploy-system-driver.sh looks for
# install/lib64/libvulkan_asahi.so, and meson's default libdir is not lib64
# everywhere.
#
# The option list is the same one packaging/honeykrisp-got.spec.in uses, so a
# locally built driver and an installed RPM are the same binary configuration.
# If you change it here, change it there.
MESON_ARGS=(
    --prefix=/usr
    --libdir=lib64
    --buildtype=release
    -Dvulkan-drivers=asahi
    -Dgallium-drivers=
    -Dplatforms=x11,wayland
    -Dopengl=false
    -Dgles1=disabled
    -Dgles2=disabled
    -Dglx=disabled
    -Degl=disabled
    -Dgbm=disabled
    -Dglvnd=disabled
    -Dvideo-codecs=
    -Dtools=
    -Dvulkan-layers=
    -Dbuild-tests=false
    -Dllvm=enabled
    -Dshared-llvm=enabled
    -Dxmlconfig=enabled
    -Dshader-cache=enabled
)

echo "==> configuring"
if [ -d "$BUILD" ]; then
    meson setup --reconfigure "${MESON_ARGS[@]}" "$BUILD" "$MESA_LOCAL" || die "meson setup failed"
else
    meson setup "${MESON_ARGS[@]}" "$BUILD" "$MESA_LOCAL" || die "meson setup failed"
fi

echo "==> building with $JOBS jobs"
ninja -C "$BUILD" -j "$JOBS" || die "build failed"

echo "==> installing to $INSTALL"
rm -rf "$INSTALL"
DESTDIR="$INSTALL" meson install -C "$BUILD" --quiet || die "install failed"

# meson installs under DESTDIR/prefix, i.e. install/usr/lib64. deploy-system-driver.sh
# wants install/lib64. Flatten it rather than teach every consumer about the
# extra level.
if [ -d "$INSTALL/usr" ]; then
    mv "$INSTALL/usr"/* "$INSTALL/" && rmdir "$INSTALL/usr"
fi

SO="$INSTALL/lib64/libvulkan_asahi.so"
[ -f "$SO" ] || die "build produced no $SO"

echo
echo "built: $SO"
echo "  $(stat -c %s "$SO") bytes, from $(git -C "$MESA_LOCAL" log -1 --format=%h)"
echo
echo "Deploy it with:  $HERE/deploy-system-driver.sh deploy"
echo "Or test it without touching the system:"
echo "  VK_DRIVER_FILES=$INSTALL/share/vulkan/icd.d/asahi_icd.aarch64.json vulkaninfo --summary"
