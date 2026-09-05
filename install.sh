#!/bin/bash
# Install honeykrisp-got.
#
# It installs a package and nothing else. It does NOT deploy the driver -- that
# is "sudo honeykrisp-got enable", a separate step on purpose, because it
# replaces the Vulkan driver your desktop is using. Read INSTALL.md.
#
#   bash install.sh            add the dnf repository and install from it
#   bash install.sh --rpm      download one RPM from the releases page instead
#
# The repository is the better path: dnf then carries the driver forward on
# every "dnf upgrade" instead of leaving you on whatever you downloaded once.

set -euo pipefail

REPO=aquarat/got-bringup
REPO_URL="https://aquarat.github.io/got-bringup"
API="https://api.github.com/repos/$REPO/releases"
MODE=repo

for a in "${@:-}"; do
    case "$a" in
    --rpm)  MODE=rpm ;;
    ""|--repo) ;;
    *) echo "install: unknown argument: $a" >&2; exit 2 ;;
    esac
done

[ "$(uname -m)" = aarch64 ] || {
    echo "install: this driver is for Apple Silicon (aarch64); this machine is $(uname -m)." >&2
    exit 1
}
command -v dnf >/dev/null || {
    echo "install: no dnf. This package is built for Fedora Asahi Remix." >&2
    exit 1
}

next_steps() {
    cat <<'NEXT'

Installed. Nothing on your system has changed yet.

    honeykrisp-got test           check the driver, changing nothing
    sudo honeykrisp-got enable    deploy it as the system Vulkan driver
    honeykrisp-got user-config    as your normal user, not root
    honeykrisp-got doctor         check everything the game needs

Recovery, if the desktop does not come back: Ctrl-Alt-F3, log in,
"sudo honeykrisp-got disable".
NEXT
}

# ------------------------------------------------------------- the dnf repo

if [ "$MODE" = repo ]; then
    if ! curl -fsS -o /dev/null "$REPO_URL/honeykrisp-got.repo"; then
        echo "install: no repository published at $REPO_URL yet; falling back to a direct download."
        MODE=rpm
    fi
fi

if [ "$MODE" = repo ]; then
    echo "==> adding the honeykrisp-got repository"
    # dnf5 (Fedora 41+) renamed this. Try the new spelling, fall back to the old.
    sudo dnf -y install 'dnf5-command(config-manager)' 2>/dev/null ||
        sudo dnf -y install dnf-plugins-core
    sudo dnf config-manager addrepo --overwrite --from-repofile="$REPO_URL/honeykrisp-got.repo" 2>/dev/null ||
        sudo dnf config-manager --add-repo "$REPO_URL/honeykrisp-got.repo"

    echo "==> installing"
    sudo dnf install -y honeykrisp-got
    next_steps
    echo
    echo "Updates now arrive with 'dnf upgrade'. Remove the repository with:"
    echo "    sudo rm /etc/yum.repos.d/honeykrisp-got.repo"
    exit 0
fi

# --------------------------------------------------- one RPM from a release

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
next_steps
