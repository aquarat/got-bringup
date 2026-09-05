#!/bin/bash
# Turn a pile of built RPMs into a dnf repository that can be served as a static
# site, so installing and *updating* the driver is "dnf install honeykrisp-got"
# rather than "go and find the newest file on the releases page".
#
#   ./make-dnf-repo.sh RPMDIR SITEDIR [BASE_URL]
#
# RPMDIR    a directory of .rpm files, any mix of Fedora releases
# SITEDIR   where to write the site (created; existing contents are replaced)
# BASE_URL  the URL SITEDIR will be served from, default the GitHub Pages one
#
# Packages are filed by the dist tag in their name, so one run can serve several
# Fedora releases from one tree and the .repo file resolves $releasever itself.
#
# If RPMDIR holds RPM-GPG-KEY-honeykrisp-got the repository is published as
# signed: gpgcheck on, and repomd.xml gets a detached signature if a private key
# is available in the environment (see sign-rpms.sh for the variables).
# Otherwise it is published as unsigned and says so on the index page, because a
# repository that claims to be verified and is not is worse than one that admits
# it isn't.

set -euo pipefail

PROG=make-dnf-repo
RPMDIR="${1:?usage: $0 RPMDIR SITEDIR [BASE_URL]}"
SITEDIR="${2:?usage: $0 RPMDIR SITEDIR [BASE_URL]}"
BASE_URL="${3:-https://aquarat.github.io/got-bringup}"
BASE_URL="${BASE_URL%/}"

command -v createrepo_c >/dev/null || { echo "$PROG: createrepo_c not installed" >&2; exit 1; }

rm -rf "$SITEDIR"
mkdir -p "$SITEDIR"

SIGNED=0
if [ -f "$RPMDIR/RPM-GPG-KEY-honeykrisp-got" ]; then
    cp "$RPMDIR/RPM-GPG-KEY-honeykrisp-got" "$SITEDIR/"
    SIGNED=1
fi

shopt -s nullglob
found=0
declare -A releases=()
for rpm in "$RPMDIR"/*.rpm; do
    case "$rpm" in *.src.rpm) continue ;; esac
    # The dist tag is the only thing that says which Fedora a package is for.
    fc=$(basename "$rpm" | grep -o 'fc[0-9]\+' | tail -1 || true)
    if [ -z "$fc" ]; then
        echo "$PROG: skipping $(basename "$rpm") -- no dist tag, so no release to file it under" >&2
        continue
    fi
    rel=${fc#fc}
    # The arch is the last dot-field before .rpm. Done with parameter expansion
    # rather than "rev | cut": rev lives in util-linux, which a minimal Fedora
    # container does not have, and its absence took this script out with
    # "command not found" after everything else had already worked.
    base=$(basename "$rpm"); base=${base%.rpm}
    arch=${base##*.}
    mkdir -p "$SITEDIR/fedora/$rel/$arch"
    cp "$rpm" "$SITEDIR/fedora/$rel/$arch/"
    releases[$rel]=1
    found=$((found + 1))
done

[ "$found" -gt 0 ] || { echo "$PROG: no binary packages found in $RPMDIR" >&2; exit 1; }
echo "$PROG: $found packages across releases: ${!releases[*]}"

for dir in "$SITEDIR"/fedora/*/*/; do
    echo "$PROG: createrepo_c $dir"
    createrepo_c --quiet --update "$dir"

    # Sign the metadata too, not just the packages. Without this an attacker who
    # can rewrite the repository can still remove a package or pin you to an old
    # one, which package signatures alone do not prevent.
    if [ "$SIGNED" = 1 ] && [ -n "${RPM_GPG_PRIVATE_KEY:-}" ]; then
        GNUPGHOME=$(mktemp -d /tmp/hkgot-repo-gnupg-XXXXXX)
        chmod 700 "$GNUPGHOME"
        export GNUPGHOME
        printf '%s\n' "$RPM_GPG_PRIVATE_KEY" | gpg --batch --quiet --import
        gpg_args=(--batch --yes --pinentry-mode loopback --detach-sign --armor)
        if [ -n "${RPM_GPG_PASSPHRASE:-}" ]; then
            pf="$GNUPGHOME/pp"; printf '%s' "$RPM_GPG_PASSPHRASE" > "$pf"; chmod 600 "$pf"
            gpg_args+=(--passphrase-file "$pf")
        fi
        gpg "${gpg_args[@]}" --output "$dir/repodata/repomd.xml.asc" "$dir/repodata/repomd.xml"
        rm -rf "$GNUPGHOME"; unset GNUPGHOME
        echo "$PROG:   signed repomd.xml"
    fi
done

# ------------------------------------------------------------------ .repo file

if [ "$SIGNED" = 1 ]; then
    gpgcheck=1
    keyline="gpgkey=$BASE_URL/RPM-GPG-KEY-honeykrisp-got"
    repo_gpgcheck=$([ -n "${RPM_GPG_PRIVATE_KEY:-}" ] && echo 1 || echo 0)
else
    gpgcheck=0
    keyline="# no packaging key: these packages are unsigned"
    repo_gpgcheck=0
fi

cat > "$SITEDIR/honeykrisp-got.repo" <<REPO
[honeykrisp-got]
name=honeykrisp-got -- patched Honeykrisp Vulkan driver (Fedora \$releasever)
baseurl=$BASE_URL/fedora/\$releasever/\$basearch/
enabled=1
gpgcheck=$gpgcheck
repo_gpgcheck=$repo_gpgcheck
$keyline
metadata_expire=1h
# Only the Fedora releases listed at
#   $BASE_URL
# are built. On any other release this baseurl 404s; skip_if_unavailable turns
# that into a warning rather than breaking every dnf command you run.
skip_if_unavailable=1
REPO

# --------------------------------------------------------------------- index

{
cat <<'HTML'
<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>honeykrisp-got</title>
<style>
  :root { color-scheme: light dark; }
  body { max-width: 46rem; margin: 3rem auto; padding: 0 1.2rem;
         font: 16px/1.6 system-ui, sans-serif; }
  pre  { background: rgba(127,127,127,.12); padding: .8rem 1rem;
         border-radius: 6px; overflow-x: auto; }
  code { font-size: .92em; }
  table { border-collapse: collapse; }
  td, th { text-align: left; padding: .2rem 1.2rem .2rem 0; }
  .warn { border-left: 3px solid #c86; padding-left: 1rem; }
</style>
<h1>honeykrisp-got</h1>
<p>A dnf repository for the Honeykrisp (Asahi) Vulkan driver built from
<a href="https://github.com/aquarat/got-bringup">aquarat/got-bringup</a>, carrying
two changes measured to cut <em>Ghost of Tsushima DIRECTOR'S CUT</em>'s per-frame
GPU compute time on an M1 Max from 102.8 ms to 18.6 ms on the heaviest scene
measured, and by less on lighter ones.</p>

<h2>Install</h2>
<pre><code>sudo dnf config-manager addrepo --from-repofile=BASEURL/honeykrisp-got.repo
sudo dnf install honeykrisp-got
sudo honeykrisp-got enable
honeykrisp-got user-config     # as your normal user, not root</code></pre>
<p>On Fedora 40 and older, <code>dnf config-manager --add-repo</code> instead.</p>

<div class="warn">
<p><strong>Installing changes nothing on its own.</strong> <code>enable</code> is a
separate step because it replaces <code>/usr/lib64/libvulkan_asahi.so</code> — the
Vulkan driver your desktop compositor is also using. That path is the only one a
Steam game reaches. If the desktop does not come back:
<strong>Ctrl-Alt-F3</strong>, log in, <code>sudo honeykrisp-got disable</code>.</p>
<p>This is a development build of Mesa, not a distro package. Read
<a href="https://github.com/aquarat/got-bringup/blob/main/INSTALL.md">INSTALL.md</a>
first — in particular the two steps that are easy to skip and silently produce no
improvement at all.</p>
</div>

<h2>What is here</h2>
HTML
echo "<table><tr><th>Fedora</th><th>packages</th></tr>"
for dir in "$SITEDIR"/fedora/*/; do
    rel=$(basename "$dir")
    n=$(find "$dir" -name '*.rpm' | wc -l)
    echo "<tr><td>$rel</td><td>$n</td></tr>"
done
echo "</table>"

if [ "$SIGNED" = 1 ]; then
    echo "<p>Packages are signed. The public key is"
    echo "<a href=\"RPM-GPG-KEY-honeykrisp-got\">RPM-GPG-KEY-honeykrisp-got</a>,"
    echo "and the .repo file above points dnf at it.</p>"
else
    echo '<p class="warn"><strong>These packages are unsigned</strong>, so the'
    echo '.repo file sets <code>gpgcheck=0</code>. Nothing here proves a package'
    echo 'came from this pipeline rather than from whoever served it to you.</p>'
fi
echo "<p style=\"opacity:.7\">Rebuilt $(date -u '+%Y-%m-%d %H:%M UTC') from the releases of aquarat/got-bringup.</p>"
} | sed "s|BASEURL|$BASE_URL|g" > "$SITEDIR/index.html"

echo "$PROG: site written to $SITEDIR"
find "$SITEDIR" -maxdepth 3 -type d | sed "s|^|$PROG:   |"
