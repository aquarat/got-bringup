#!/bin/bash
# Generate a packaging key for signing honeykrisp-got RPMs, and print what to
# put in GitHub so CI can use it.
#
#   ./make-gpg-key.sh [OUTDIR]      # default: ./gpg-key (gitignored)
#
# This is a CI SIGNING KEY, not your personal one. It exists to let a user
# verify that a package came from this pipeline and was not modified in
# transit. Deliberately, by default, it has:
#
#   * no passphrase -- a passphrase in the same secret store as the key it
#     protects buys nothing, and it is one more thing to lose. Set
#     GPG_PASSPHRASE if you disagree; sign-rpms.sh handles both.
#   * a 3 year expiry -- so an abandoned key stops being usable rather than
#     staying valid forever.
#   * its own identity, not yours, so it is obvious what it is for and
#     revoking it costs you nothing else.
#
# Keep the private key out of the repository. This script writes it to a file
# so it never passes through a shell history or a terminal scrollback, and the
# output directory is gitignored.

set -euo pipefail

OUTDIR="${1:-$(dirname "${BASH_SOURCE[0]}")/gpg-key}"
NAME="${GPG_NAME:-honeykrisp-got packaging}"
EMAIL="${GPG_EMAIL:-honeykrisp-got@users.noreply.github.com}"

command -v gpg >/dev/null || { echo "gpg is not installed" >&2; exit 1; }

mkdir -p "$OUTDIR"
chmod 700 "$OUTDIR"

export GNUPGHOME="$OUTDIR/gnupg"
mkdir -p "$GNUPGHOME"
chmod 700 "$GNUPGHOME"

if gpg --list-secret-keys --with-colons 2>/dev/null | grep -q '^sec:'; then
    echo "a key already exists in $GNUPGHOME -- refusing to make a second one." >&2
    echo "Delete that directory if you really want a new key; anything signed" >&2
    echo "with the old one will stop verifying." >&2
    exit 1
fi

echo "==> generating a 4096-bit RSA signing key for \"$NAME <$EMAIL>\""
cat > "$GNUPGHOME/params" <<PARAMS
%echo generating
Key-Type: RSA
Key-Length: 4096
Key-Usage: sign
Name-Real: $NAME
Name-Email: $EMAIL
Name-Comment: RPM signing key
Expire-Date: 3y
$( [ -n "${GPG_PASSPHRASE:-}" ] && echo "Passphrase: $GPG_PASSPHRASE" || echo "%no-protection" )
%commit
%echo done
PARAMS
gpg --batch --gen-key "$GNUPGHOME/params"
rm -f "$GNUPGHOME/params"

KEYID=$(gpg --list-secret-keys --with-colons | awk -F: '/^sec:/ {print $5; exit}')

gpg --batch --armor --export-secret-keys "$KEYID" > "$OUTDIR/private.asc"
gpg --batch --armor --export             "$KEYID" > "$OUTDIR/RPM-GPG-KEY-honeykrisp-got"
chmod 600 "$OUTDIR/private.asc"

cat <<DONE

Key $KEYID created.

  $OUTDIR/private.asc                        the private half -- NEVER commit this
  $OUTDIR/RPM-GPG-KEY-honeykrisp-got         the public half

Add the private half to GitHub as a repository secret. The value is the whole
file including the BEGIN/END lines:

  Settings -> Secrets and variables -> Actions -> New repository secret
    Name:  RPM_GPG_PRIVATE_KEY
    Value: the contents of $OUTDIR/private.asc
DONE

if [ -n "${GPG_PASSPHRASE:-}" ]; then
    cat <<DONE
    ...and a second secret:
    Name:  RPM_GPG_PASSPHRASE
    Value: the passphrase you set
DONE
fi

cat <<DONE

Or, with the gh CLI:

  gh secret set RPM_GPG_PRIVATE_KEY --repo aquarat/got-bringup < $OUTDIR/private.asc

Then back up private.asc somewhere you will still have it in three years, and
delete it from here. The public half does NOT need backing up -- it is
published with every release and in the dnf repository, and this script can
re-export it from $GNUPGHOME.

Until the secret exists, CI keeps building unsigned packages and says so.
DONE
