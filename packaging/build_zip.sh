#!/bin/bash
#
# Builds a no-installer distribution of Less PA: a plain .zip containing the
# built AU and VST3 bundles (and the AAX, when a PACE-signed one is present)
# plus manual install instructions.
#
# This is the drag-and-drop alternative to build_installer.sh, for people who
# would rather copy two bundles into place than run an installer. The bundles
# are signed with a Developer ID, notarized and stapled exactly as the ones
# inside the .pkg are, so they load with no quarantine-clearing step and no
# Gatekeeper prompt.
#
# Produces packaging/build/Less-PA-<version>.zip from an existing Release
# build in plugin/build (see BUILDING.md).
#
# Requires the Developer ID Application certificate and a notarytool keychain
# profile -- see the "Code signing" section of BUILDING.md.
#
# Usage: ./packaging/build_zip.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

# shellcheck source=packaging/signing.sh
source "$HERE/signing.sh"

ARTEFACTS="$REPO_ROOT/plugin/build/PAEchoCanceller_artefacts/Release"
AU_BUNDLE="$ARTEFACTS/AU/Less PA.component"
VST3_BUNDLE="$ARTEFACTS/VST3/Less PA.vst3"
AAX_BUNDLE="$ARTEFACTS/AAX/Less PA.aaxplugin"

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
FOLDER_NAME="Less-PA-$VERSION"
STAGE_DIR="$BUILD_DIR/$FOLDER_NAME"

for bundle in "$AU_BUNDLE" "$VST3_BUNDLE"; do
    if [[ ! -d "$bundle" ]]; then
        echo "error: missing $bundle" >&2
        echo "       Build the Release targets first -- see BUILDING.md." >&2
        exit 1
    fi
done

echo "Less PA $VERSION -- building zip"

lesspa_require_signing_identities
MIN_MACOS="$(lesspa_deployment_target "$REPO_ROOT/plugin/CMakeLists.txt")"
lesspa_check_no_stray_dylibs "$AU_BUNDLE" "$VST3_BUNDLE"
lesspa_check_binary_targets "$MIN_MACOS" "$AU_BUNDLE" "$VST3_BUNDLE"

# Idempotent: if build_installer.sh already ran against this build, the
# bundles are stapled and this is a no-op rather than a second trip to
# Apple's notary service.
lesspa_prepare_bundles "$AU_BUNDLE" "$VST3_BUNDLE"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

cp -R "$AU_BUNDLE" "$STAGE_DIR/"
cp -R "$VST3_BUNDLE" "$STAGE_DIR/"

# The AAX is never signed here (see build_installer.sh): it is included only
# if it is already PACE-signed, notarized and stapled.
AAX_COPY_LINE=""
AAX_NOTE=""
if [[ -d "$AAX_BUNDLE/Contents/__Pace_Eden.bundle" ]] \
        && codesign --verify --deep --strict "$AAX_BUNDLE" 2>/dev/null \
        && lesspa_bundles_are_stapled "$AAX_BUNDLE"; then
    lesspa_check_no_stray_dylibs "$AAX_BUNDLE"
    cp -R "$AAX_BUNDLE" "$STAGE_DIR/"
    echo "  including the AAX (PACE-signed and stapled)"
    AAX_COPY_LINE="
    Less PA.aaxplugin  ->  /Library/Application Support/Avid/Audio/Plug-Ins/"
    AAX_NOTE="
The AAX is for Pro Tools, which only looks in that system folder, so macOS
asks for an administrator password when you copy it there.
"
fi

cat > "$STAGE_DIR/INSTALL.txt" <<TXT
Less PA $VERSION -- manual install
===================================

This is the no-installer copy, for anyone who would rather drag the files
into place than run an installer. If you would prefer the installer, grab
the .pkg from the releases page instead -- it does exactly the same thing.

Copy the plugin(s) you want into place:

    Less PA.component  ->  ~/Library/Audio/Plug-Ins/Components/
    Less PA.vst3       ->  ~/Library/Audio/Plug-Ins/VST3/$AAX_COPY_LINE

Only the format your DAW uses is needed -- AU for Logic/GarageBand, VST3 for
Reaper/Ableton/Cubase/etc. Create the destination folder first if it
doesn't already exist. The AU and VST3 folders above are your own user
plug-in folders, so no administrator password is needed for those. If an
installer put Less PA in /Library/Audio/Plug-Ins before, remove that copy
so your DAW doesn't see two.
$AAX_NOTE
That's the whole install. Restart your DAW, or trigger a plugin rescan, so
it picks up the new plugin.

The plugins are signed with an Apple Developer ID and notarized by Apple,
with the notarization ticket stapled to each bundle -- so there is no
quarantine flag to clear, no Terminal command to run, and no Gatekeeper
prompt, even offline.

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
echo "Zip:  $FINAL_ZIP"
echo "Size: $(du -h "$FINAL_ZIP" | cut -f1)"
