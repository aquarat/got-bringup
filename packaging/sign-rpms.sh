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
KEYFPR=$(gpg --list-secret-keys --with-colons | awk -F: '/^fpr:/ {print $10; exit}')
UID_LINE=$(gpg --list-secret-keys --with-colons | awk -F: '/^uid:/ {print $10; exit}')
echo "$PROG: signing with $KEYFPR  ($UID_LINE)"

# Getting a passphrase-protected key to sign unattended under rpm 6.
#
# rpm 6 made the low-level signing macros parametric, so the
# %_gpg_sign_cmd_extra_args override that used to carry "--pinentry-mode
# loopback --passphrase-file" is ignored outright. rpm runs gpg its own way,
# gets nothing back, and exits 0 having signed nothing -- which is how this
# failed silently to begin with. So the passphrase has to be dealt with before
# rpm is involved at all, and rpm's gpg call must then need no passphrase.
#
# Two ways to get there, tried in that order:
#   1. prime gpg-agent. One loopback signature unlocks the key and the agent
#      caches it, so rpm's later plain gpg calls just work. The key is not
#      modified.
#   2. failing that, take the passphrase off this copy of the key. That is a
#      throwaway keyring in an ephemeral container and this process was handed
#      the plaintext key in an environment variable a moment ago, so it gives
#      nothing away -- but it is still the bigger hammer, so it is second.

cat > "$GNUPGHOME/gpg-agent.conf" <<CONF
allow-loopback-pinentry
default-cache-ttl 7200
max-cache-ttl 7200
CONF
gpgconf --kill gpg-agent 2>/dev/null || :

# Signs one byte and throws the signature away. With a passphrase argument it
# unlocks the key and primes the agent; without one it answers the only question
# that matters -- can gpg sign with no help, which is all rpm will give it.
try_sign() {
    if [ $# -eq 1 ]; then
        printf preflight | gpg --batch --yes --pinentry-mode loopback \
            --passphrase "$1" --local-user "$KEYFPR" \
            --detach-sign --output /dev/null 2>&1
    else
        printf preflight | gpg --batch --yes --local-user "$KEYFPR" \
            --detach-sign --output /dev/null 2>&1
    fi
}

if [ -n "${RPM_GPG_PASSPHRASE:-}" ]; then
    # A secret pasted into GitHub keeps whatever trailing newline came with it,
    # and that newline is part of the passphrase as far as gpg is concerned.
    PP_TRIMMED=$(printf '%s' "$RPM_GPG_PASSPHRASE" | tr -d '\r\n')
    if out=$(try_sign "$RPM_GPG_PASSPHRASE"); then
        PP="$RPM_GPG_PASSPHRASE"
    elif out=$(try_sign "$PP_TRIMMED"); then
        echo "$PROG: note: the passphrase worked only after trimming trailing whitespace"
        PP="$PP_TRIMMED"
    else
        echo "$PROG: gpg will not unlock $KEYFPR with RPM_GPG_PASSPHRASE." >&2
        printf '%s\n' "$out" | sed "s/^/$PROG:   /" >&2
        echo "$PROG: check the secret matches the passphrase on the EXPORTED key." >&2
        exit 1
    fi
fi

if ! out=$(try_sign); then
    if [ -z "${PP:-}" ]; then
        echo "$PROG: gpg cannot sign with $KEYFPR and no passphrase was given." >&2
        printf '%s\n' "$out" | sed "s/^/$PROG:   /" >&2
        exit 1
    fi
    echo "$PROG: gpg-agent did not retain the passphrase; removing it from this copy"
    if ! out=$(gpg --batch --pinentry-mode loopback --passphrase "$PP" \
                   --new-passphrase '' --passphrase-repeat 0 \
                   --passwd "$KEYFPR" 2>&1); then
        echo "$PROG: could not remove the passphrase either:" >&2
        printf '%s\n' "$out" | sed "s/^/$PROG:   /" >&2
        exit 1
    fi
    if ! out=$(try_sign); then
        echo "$PROG: gpg still cannot sign unattended with $KEYFPR" >&2
        printf '%s\n' "$out" | sed "s/^/$PROG:   /" >&2
        exit 1
    fi
fi
echo "$PROG: gpg signs unattended; handing over to rpmsign"

SIGN_ARGS=(
    # rpm 6 names the signer with %_openpgp_sign_id and picks the backend with
    # %_openpgp_sign; %_gpg_name is the rpm 4/5 spelling and is kept so this
    # script works on both.
    --define "_openpgp_sign gpg"
    --define "_openpgp_sign_id $KEYFPR"
    --define "_gpg_name $KEYFPR"
)

shopt -s nullglob
rpms=("$DIR"/*.rpm)
[ ${#rpms[@]} -gt 0 ] || { echo "$PROG: no packages in $DIR" >&2; exit 1; }

rpmsign "${SIGN_ARGS[@]}" --addsign "${rpms[@]}"

gpg --batch --armor --export "$KEYID" > "$DIR/RPM-GPG-KEY-honeykrisp-got"
echo "$PROG: public key -> $DIR/RPM-GPG-KEY-honeykrisp-got"

# rpmsign can return 0 having signed nothing at all, depending on version and
# how the key was rejected, so every package is checked individually.
#
# Read the Signature line out of "rpm -qi" rather than querying signature tags
# directly. The tags are not stable across rpm versions -- rpm 6 (Fedora 44) no
# longer fills in the legacy V3 %{SIGPGP}/%{SIGGPG} pair that rpm 4 used, so
# querying those reports a perfectly good package as unsigned. The Signature
# line is derived from whichever tag the running rpm actually populates, and is
# what a user inspecting the package would look at.
#
# Where the keyring can be written -- i.e. as root, which is the case in CI --
# go further and check the signature VERIFIES against the key we just exported,
# not merely that some signature is present.
verify=0
if rpm --import "$DIR/RPM-GPG-KEY-honeykrisp-got" 2>/dev/null; then
    verify=1
else
    echo "$PROG: cannot write the rpm keyring; checking signatures are present, not that they verify"
fi

fail=0
for r in "${rpms[@]}"; do
    sig=$(rpm -qpi "$r" 2>/dev/null | sed -n 's/^Signature *: *//p')
    if [ -z "$sig" ] || [ "$sig" = "(none)" ]; then
        echo "$PROG: NOT SIGNED: $(basename "$r")" >&2
        fail=1
        continue
    fi
    if [ "$verify" = 1 ] && ! rpm --checksig "$r" 2>&1 | grep -q 'signatures OK'; then
        echo "$PROG: SIGNATURE DOES NOT VERIFY: $(basename "$r")" >&2
        rpm --checksig -v "$r" 2>&1 | sed "s/^/$PROG:   /" >&2
        fail=1
        continue
    fi
    echo "$PROG: signed $(basename "$r")  [$sig]"
done
[ "$fail" = 0 ] || exit 1
