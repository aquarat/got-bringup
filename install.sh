#!/bin/bash
# Fetch and install the newest honeykrisp-got release.
#
# It installs a package and nothing else. It does NOT deploy the driver -- that
# is "sudo honeykrisp-got enable", which is a separate step on purpose, because
# it replaces the Vulkan driver your desktop is using. Read INSTALL.md.
#
#   bash install.sh
#
# No network trust beyond GitHub's release API and dnf's own signature handling
# (these packages are unsigned; dnf will say so and ask).

set -euo pipefail

REPO=aquarat/got-bringup
API="https://api.github.com/repos/$REPO/releases"

[ "$(uname -m)" = aarch64 ] || {
    echo "install: this driver is for Apple Silicon (aarch64); this machine is $(uname -m)." >&2
    exit 1
}
command -v dnf >/dev/null || {
    echo "install: no dnf. This package is built for Fedora Asahi Remix." >&2
    exit 1
}

echo "==> finding the newest release with an aarch64 package"
url=$(curl -fsSL "$API" |
      grep -o '"browser_download_url": *"[^"]*honeykrisp-got-[0-9][^"]*\.aarch64\.rpm"' |
      sed 's/.*"\(https[^"]*\)"/\1/' |
      grep -v debug |
      head -1)

[ -n "$url" ] || {
    echo "install: no release package found at https://github.com/$REPO/releases" >&2
    echo "         Build one yourself: see INSTALL.md, 'Building it yourself instead'." >&2
    exit 1
}

tmp=$(mktemp -d /tmp/honeykrisp-got-XXXXXX)
trap 'rm -rf "$tmp"' EXIT
echo "==> downloading $(basename "$url")"
curl -fsSL -o "$tmp/$(basename "$url")" "$url"

echo "==> installing"
sudo dnf install -y "$tmp/$(basename "$url")"

cat <<'NEXT'

Installed. Nothing on your system has changed yet.

    honeykrisp-got test           check the driver, changing nothing
    sudo honeykrisp-got enable    deploy it as the system Vulkan driver
    honeykrisp-got user-config    as your normal user, not root
    honeykrisp-got doctor         check everything the game needs

Recovery, if the desktop does not come back: Ctrl-Alt-F3, log in,
"sudo honeykrisp-got disable".
NEXT
