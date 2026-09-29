#!/usr/bin/env bash
#
# Write the commits on the LibreOffice tree's "xsnium" branch back to office/patches, after changing a
# LibreOffice file (anything outside the xsnium module, which lives in this repository already).
# Commit the change in the LibreOffice tree first, then run this and commit the patches here.

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
lo="${LO_DIR:-$HOME/lo/libo-core}"
commit="$(tr -d '[:space:]' < "$here/LIBREOFFICE_COMMIT")"

rm -f "$here"/patches/*.patch
git -C "$lo" format-patch -q --no-signature --zero-commit "$commit..xsnium" -o "$here/patches"
ls "$here/patches"
