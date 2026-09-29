#!/usr/bin/env bash
#
# Prepare the LibreOffice build tree for XSNium, from this repository:
#   - LibreOffice at the commit pinned in office/LIBREOFFICE_COMMIT,
#   - XSNium's changes to LibreOffice (office/patches) on a branch named "xsnium",
#   - this repository's office/xsnium module linked into the tree (a directory junction),
#   - XSNium's product settings (office/autogen.xsnium) in autogen.input.
#
# Run it in Git Bash. The build tools come from LibreOffice's own winget setup; see
# docs/building-xsnium-office.md. The tree defaults to ~/lo/libo-core (LO_DIR overrides it).

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
lo="${LO_DIR:-$HOME/lo/libo-core}"
commit="$(tr -d '[:space:]' < "$here/LIBREOFFICE_COMMIT")"

if [ ! -d "$lo/.git" ]; then
    echo "Fetching LibreOffice $commit into $lo"
    mkdir -p "$lo"
    git -C "$lo" init -q
    git -C "$lo" config core.autocrlf false
    git -C "$lo" config protocol.version 2
    git -C "$lo" remote add origin https://git.libreoffice.org/core
    git -C "$lo" fetch -q --depth 1 origin "$commit"
    git -C "$lo" checkout -q -b xsnium FETCH_HEAD
    git -C "$lo" am -q "$here"/patches/*.patch
else
    # An existing tree is never reset: it may hold work that is not exported as patches yet.
    base="$(git -C "$lo" merge-base xsnium "$commit" 2>/dev/null || true)"
    if [ "$base" != "$commit" ]; then
        echo "error: $lo has no branch 'xsnium' based on $commit; fix it by hand or use a new LO_DIR" >&2
        exit 1
    fi
    expected="$(ls "$here"/patches/*.patch | wc -l)"
    actual="$(git -C "$lo" rev-list --count "$commit..xsnium")"
    if [ "$expected" != "$actual" ]; then
        echo "warning: branch xsnium has $actual commits, office/patches has $expected; run office/export-patches.sh or apply the missing patches" >&2
    fi
fi

# Link office/xsnium into the tree, so the module is built from this repository.
if [ ! -e "$lo/xsnium" ]; then
    powershell.exe -NoProfile -Command \
        "New-Item -ItemType Junction -Path '$(cygpath -w "$lo/xsnium")' -Target '$(cygpath -w "$here/xsnium")' | Out-Null"
fi

# XSNium's product settings.
input="$lo/autogen.input"
if [ -f "$HOME/lo/autogen.input" ] && [ ! -f "$input" ]; then
    cp "$HOME/lo/autogen.input" "$input"
fi
touch "$input"
grep -v '^#' "$here/autogen.xsnium" | while IFS= read -r line; do
    [ -z "$line" ] && continue
    grep -qxF -- "$line" "$input" || echo "$line" >> "$input"
done

echo "Ready: $lo (branch xsnium). Next, in that directory: wsl ./autogen.sh && make"
