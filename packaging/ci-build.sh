#!/bin/bash
# Build the honeykrisp-got RPM inside a Fedora container.
#
# This is what .github/workflows/build-driver.yml runs, and it is deliberately
# runnable by hand so a failing CI job can be reproduced without GitHub:
#
#   podman run --rm -v "$PWD:/src" -w /src fedora:44 packaging/ci-build.sh
#
# It must run as root inside a Fedora aarch64 container (or an x86_64 one, if
# you only want to find out whether the source still compiles -- the resulting
# package is useless anywhere but Apple Silicon, and the spec's ExclusiveArch
# will refuse to build it).
#
# Output: ./out/*.src.rpm and ./out/*.rpm

set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT="$ROOT/out"
mkdir -p "$OUT"

echo "::group::install build tooling" 2>/dev/null || true
dnf -y install --setopt=install_weak_deps=False \
    git-core rpm-build redhat-rpm-config gzip which
# dnf5 (Fedora 41+) keeps builddep in dnf5-plugins; older releases in dnf-plugins-core.
dnf -y install 'dnf5-command(builddep)' 2>/dev/null || dnf -y install dnf-plugins-core
echo "::endgroup::" 2>/dev/null || true

# git refuses to work in a directory owned by another uid, which is exactly what
# a bind-mounted workspace looks like from inside the container.
git config --global --add safe.directory '*'

echo "::group::make srpm" 2>/dev/null || true
"$ROOT/packaging/make-srpm.sh" "$OUT"
echo "::endgroup::" 2>/dev/null || true

SRPM=$(ls "$OUT"/*.src.rpm | head -1)
echo "srpm: $SRPM"

echo "::group::builddep" 2>/dev/null || true
dnf -y builddep "$SRPM"
echo "::endgroup::" 2>/dev/null || true

echo "::group::rpmbuild" 2>/dev/null || true
TOPDIR=$(mktemp -d /tmp/hkgot-rpmbuild-XXXXXX)
rpmbuild --define "_topdir $TOPDIR" --rebuild "$SRPM"
echo "::endgroup::" 2>/dev/null || true

find "$TOPDIR/RPMS" -name '*.rpm' -exec cp -v {} "$OUT/" \;

echo
echo "built:"
ls -la "$OUT"

# A package that lists no dependency on libvulkan or libdrm did not link what we
# think it linked. Cheap check, and it has to hold on every release.
MAIN=$(ls "$OUT"/honeykrisp-got-[0-9]*.rpm | grep -v debug | head -1)
rpm -qlp "$MAIN" | grep -q 'libvulkan_asahi.so' \
    || { echo "ci-build: the package contains no driver" >&2; exit 1; }
rpm -qp --requires "$MAIN" | grep -q 'libdrm' \
    || { echo "ci-build: the driver does not depend on libdrm -- suspect build" >&2; exit 1; }
echo "package checks passed"
