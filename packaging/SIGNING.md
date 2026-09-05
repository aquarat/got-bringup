# Signing the packages

By default this pipeline builds **unsigned** RPMs. `dnf` says so on install, and
the dnf repository publishes `gpgcheck=0` and admits it on its index page. That
is the honest default for a project with no key: a repository that claims to be
verified and is not is worse than one that says it isn't.

Once a key exists, CI signs every package and the repository turns `gpgcheck`
on by itself. Nothing else changes.

## What signing gets you

A signature says a package came from this pipeline and has not been altered
since. That matters here more than for most projects, because the thing being
distributed replaces the Vulkan driver the user's compositor runs. It says
nothing about whether the driver is *correct* — see the conformance section of
`STATE.md` for what does.

Repository metadata is signed too (`repomd.xml.asc`). Package signatures alone
do not stop somebody who can rewrite the repository from removing a package or
holding you on an old one.

## Making a key

    packaging/make-gpg-key.sh

It writes `packaging/gpg-key/` (gitignored) and prints exactly what to paste
where. The key it makes is deliberately a **CI signing key**, not your personal
one: its own identity, a 3 year expiry, and no passphrase by default — a
passphrase stored in the same secret store as the key it protects buys nothing.
Set `GPG_PASSPHRASE` if you disagree; the signing script handles both.

## Putting it in GitHub

Settings → Secrets and variables → Actions → New repository secret:

| secret | value |
|---|---|
| `RPM_GPG_PRIVATE_KEY` | the whole of `packaging/gpg-key/private.asc`, BEGIN/END lines included |
| `RPM_GPG_PASSPHRASE` | only if you set one |

Or:

    gh secret set RPM_GPG_PRIVATE_KEY --repo aquarat/got-bringup \
        < packaging/gpg-key/private.asc

Then back `private.asc` up somewhere you will still have it in three years, and
delete it from the working tree. The public half needs no backup: it is attached
to every release and published in the repository.

## After that

Re-run **build driver package** with `publish: true`. The packages are signed,
`RPM-GPG-KEY-honeykrisp-got` is attached to the release, and the **dnf
repository** workflow picks both up.

Check it worked:

    rpm -qpi honeykrisp-got-*.rpm | grep Signature      # not "(none)"
    rpm -Kv honeykrisp-got-*.rpm                        # after importing the key

## Rotating or revoking

Packages signed with the old key keep verifying only while users still trust it.
To rotate: make a new key, replace the secret, and re-run a publish so the
release assets and the repository carry the new public key. Users who added the
repository get the new key through the `gpgkey=` URL; users who imported the old
one by hand need `sudo rpm -e --allmatches gpg-pubkey-<old-id>` first.

## Why the workflow does not fail when there is no key

The secret cannot exist until somebody creates it, and refusing to build until
then would just mean nobody could build. So `sign-rpms.sh` exits 0 and says the
packages are unsigned.

It does **not** extend that leniency to a key that is present and does not work.
If `RPM_GPG_PRIVATE_KEY` is set and signing fails — bad key, wrong half, wrong
passphrase, or `rpmsign` returning success having signed nothing, which it can —
the job fails. Shipping unsigned packages after being asked to sign them is the
one outcome worth stopping for.
