#!/bin/bash
# Sign the built RPMs with the repository's packaging key.
#
# Reads the key from the environment, never from a file in the repository and
# never from an argument:
#
#   RPM_GPG_PRIVATE_KEY   ASCII-armoured private key    (required)
#   RPM_GPG_PASSPHRASE    passphrase, if the key has one (optional)
#
# In CI these come from repository secrets. packaging/make-gpg-key.sh generates
# a key and tells you exactly what to paste where.
#
#   ./sign-rpms.sh out/            # signs every *.rpm in that directory
#
# Also writes out/RPM-GPG-KEY-honeykrisp-got -- the PUBLIC half. Without it
# published somewhere users can fetch, a signature proves nothing to them, so
# the two always travel together.
#
# EXIT STATUS matters here. If RPM_GPG_PRIVATE_KEY is unset this exits 0 and
# signs nothing, because unsigned packages are the honest default for a project
# with no key. If a key IS present and signing fails, it exits non-zero: a
# pipeline that was asked to sign and then quietly shipped unsigned packages is
# worse than one that never tried.

set -euo pipefail

DIR="${1:-out}"
PROG=sign-rpms

if [ -z "${RPM_GPG_PRIVATE_KEY:-}" ]; then
    echo "$PROG: no RPM_GPG_PRIVATE_KEY -- leaving these packages unsigned."
    echo "$PROG: see packaging/SIGNING.md to set one up."
    exit 0
fi

command -v rpmsign >/dev/null || { echo "$PROG: rpmsign not installed (dnf install rpm-sign)" >&2; exit 1; }
command -v gpg     >/dev/null || { echo "$PROG: gpg not installed (dnf install gnupg2)" >&2; exit 1; }

# A private key in a world-readable directory is a private key you have given
# away. GNUPGHOME must be 0700 or gpg refuses anyway.
GNUPGHOME=$(mktemp -d /tmp/hkgot-gnupg-XXXXXX)
chmod 700 "$GNUPGHOME"
export GNUPGHOME
trap 'rm -rf "$GNUPGHOME"' EXIT

printf '%s\n' "$RPM_GPG_PRIVATE_KEY" | gpg --batch --quiet --import 2>&1 |
    sed "s/^/$PROG: /"

KEYID=$(gpg --list-secret-keys --with-colons | awk -F: '/^sec:/ {print $5; exit}')
[ -n "$KEYID" ] || { echo "$PROG: no secret key after import -- is RPM_GPG_PRIVATE_KEY the private half?" >&2; exit 1; }
UID_LINE=$(gpg --list-secret-keys --with-colons | awk -F: '/^uid:/ {print $10; exit}')
echo "$PROG: signing with $KEYID  ($UID_LINE)"

SIGN_ARGS=(--define "_gpg_name $KEYID")
if [ -n "${RPM_GPG_PASSPHRASE:-}" ]; then
    # --pinentry-mode loopback is what lets gpg take a passphrase without a tty.
    # There is no terminal in a container and no agent to ask.
    pf="$GNUPGHOME/pp"
    printf '%s' "$RPM_GPG_PASSPHRASE" > "$pf"
    chmod 600 "$pf"
    SIGN_ARGS+=(--define "_gpg_sign_cmd_extra_args --batch --pinentry-mode loopback --passphrase-file $pf")
else
    SIGN_ARGS+=(--define "_gpg_sign_cmd_extra_args --batch --pinentry-mode loopback")
fi

shopt -s nullglob
rpms=("$DIR"/*.rpm)
[ ${#rpms[@]} -gt 0 ] || { echo "$PROG: no packages in $DIR" >&2; exit 1; }

rpmsign "${SIGN_ARGS[@]}" --addsign "${rpms[@]}"

# rpmsign can return 0 having signed nothing at all, depending on version and
# how the key was rejected. Verify, package by package, that a signature is
# actually there.
fail=0
for r in "${rpms[@]}"; do
    if rpm -qp --qf '%{SIGPGP:pgpsig}%{SIGGPG:pgpsig}\n' "$r" 2>/dev/null | grep -q 'Key ID'; then
        echo "$PROG: signed $(basename "$r")"
    else
        echo "$PROG: NOT SIGNED: $(basename "$r")" >&2
        fail=1
    fi
done
[ "$fail" = 0 ] || exit 1

gpg --batch --armor --export "$KEYID" > "$DIR/RPM-GPG-KEY-honeykrisp-got"
echo "$PROG: public key -> $DIR/RPM-GPG-KEY-honeykrisp-got"
