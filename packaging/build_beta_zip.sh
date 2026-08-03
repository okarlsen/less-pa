#!/bin/bash
#
# Builds a no-installer beta distribution of Less PA: a plain .zip containing
# the built AU and VST3 bundles plus manual install instructions.
#
# This exists as a stopgap alongside build_installer.sh: the .pkg installer
# is unsigned (ad-hoc only) and macOS now blocks it behind System Settings ->
# Privacy & Security with no reliable right-click bypass. A signed,
# notarized release is planned, but until then this gives people a copy that
# doesn't route through Installer.app's stricter Gatekeeper check at all --
# see INSTALL.txt below for the one-time quarantine-clearing step this still
# needs.
#
# Produces packaging/build/Less-PA-<version>-beta.zip from an existing
# Release build in plugin/build (see BUILDING.md).
#
# Usage: ./packaging/build_beta_zip.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

ARTEFACTS="$REPO_ROOT/plugin/build/PAEchoCanceller_artefacts/Release"
AU_BUNDLE="$ARTEFACTS/AU/Less PA.component"
VST3_BUNDLE="$ARTEFACTS/VST3/Less PA.vst3"

BUILD_DIR="$HERE/build"

# Single source of truth for the version: the project() line in CMakeLists.
VERSION="$(sed -n 's/^project(PAEchoCanceller VERSION \([0-9.]*\)).*/\1/p' \
    "$REPO_ROOT/plugin/CMakeLists.txt")"

if [[ -z "$VERSION" ]]; then
    echo "error: could not read the version from plugin/CMakeLists.txt" >&2
    exit 1
fi

# Named after the shipped zip's contents (not a generic "stage" name) so
# --keepParent below gives the unzipped folder a sensible name instead of a
# build-internal one.
FOLDER_NAME="Less-PA-$VERSION-beta"
STAGE_DIR="$BUILD_DIR/$FOLDER_NAME"

for bundle in "$AU_BUNDLE" "$VST3_BUNDLE"; do
    if [[ ! -d "$bundle" ]]; then
        echo "error: missing $bundle" >&2
        echo "       Build the Release targets first -- see BUILDING.md." >&2
        exit 1
    fi
done

echo "Less PA $VERSION -- building beta zip"

# Same safety check as build_installer.sh: refuse to ship a plugin that
# drags in dylibs the end user will not have (the Homebrew-Abseil failure
# mode -- see BUILDING.md's 'A note on Abseil').
for bundle in "$AU_BUNDLE" "$VST3_BUNDLE"; do
    binary="$bundle/Contents/MacOS/Less PA"
    strays="$(otool -L "$binary" | tail -n +2 \
        | grep -v -e '/System/Library/' -e '/usr/lib/' || true)"
    if [[ -n "$strays" ]]; then
        echo "error: $(basename "$bundle") links against non-system libraries:" >&2
        echo "$strays" >&2
        echo "       These will not exist on an end user's Mac. See BUILDING.md." >&2
        exit 1
    fi
done
echo "  checked: no non-system dynamic dependencies"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

cp -R "$AU_BUNDLE" "$STAGE_DIR/"
cp -R "$VST3_BUNDLE" "$STAGE_DIR/"

cat > "$STAGE_DIR/INSTALL.txt" <<TXT
Less PA $VERSION -- beta (manual install)
==========================================

This is an interim, no-installer copy while a signed & notarized .pkg
release is being finished -- it works exactly the same, it's just not
signed with an Apple Developer ID yet, which means macOS needs one extra
one-time step below.

1. Copy the plugin(s) you want into place:

     Less PA.component  ->  ~/Library/Audio/Plug-Ins/Components/
     Less PA.vst3       ->  ~/Library/Audio/Plug-Ins/VST3/

   (Only need one format, not both -- AU for Logic/GarageBand, VST3 for
   Reaper/Ableton/Cubase/etc. Create the destination folder first if it
   doesn't already exist.)

2. Clear the quarantine flag macOS attaches to anything downloaded, so
   your DAW can load it without a Gatekeeper prompt. Open Terminal and run:

     xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/Components/"Less PA.component"
     xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"Less PA.vst3"

   (Run whichever line matches what you copied in step 1 -- both is fine
   too.) This is the one-time step; nothing else is needed after this.

   If you skip this and your DAW's plugin scan blocks it anyway, the
   fallback is System Settings -> Privacy & Security -> scroll down to the
   blocked-item notice -> Open Anyway.

Questions or issues: https://github.com/okarlsen/less-pa
TXT

FINAL_ZIP="$BUILD_DIR/$FOLDER_NAME.zip"

rm -f "$FINAL_ZIP"

# ditto (not zip) preserves the bundles' resource forks/extended attributes
# correctly -- the standard way to zip a .app/.component/.vst3 on macOS.
# Run from BUILD_DIR naming the folder explicitly (rather than cd-ing into it
# and passing ".") so --keepParent wraps the zip in "$FOLDER_NAME/" instead of
# whatever the current directory happens to be called.
(cd "$BUILD_DIR" && ditto -c -k --sequesterRsrc --keepParent "$FOLDER_NAME" "$FINAL_ZIP")

rm -rf "$STAGE_DIR"

echo
echo "Beta zip: $FINAL_ZIP"
echo "Size:     $(du -h "$FINAL_ZIP" | cut -f1)"
