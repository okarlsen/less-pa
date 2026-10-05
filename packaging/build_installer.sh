#!/bin/bash
#
# Builds the Less PA macOS installer package: signed with a Developer ID,
# notarized by Apple, and stapled.
#
# Produces packaging/build/Less-PA-<version>.pkg from an existing Release
# build in plugin/build (see BUILDING.md). Two pkgbuild components -- the AU
# and the VST3 -- are combined with productbuild into a single installer that
# writes into the current user's ~/Library/Audio/Plug-Ins, so it needs no
# administrator password.
#
# The plugin bundles are signed, notarized and stapled *before* being staged
# into the package, so plugins installed from this .pkg carry their own
# notarization ticket and load without any network lookup. The finished .pkg
# is then signed with the Developer ID Installer certificate and notarized in
# its own right.
#
# Requires the Developer ID certificates and a notarytool keychain profile --
# see the "Code signing" section of BUILDING.md.
#
# Usage: ./packaging/build_installer.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

# shellcheck source=packaging/signing.sh
source "$HERE/signing.sh"

ARTEFACTS="$REPO_ROOT/plugin/build/PAEchoCanceller_artefacts/Release"
AU_BUNDLE="$ARTEFACTS/AU/Less PA.component"
VST3_BUNDLE="$ARTEFACTS/VST3/Less PA.vst3"

BUILD_DIR="$HERE/build"
STAGE_DIR="$BUILD_DIR/stage"

PKG_ID_BASE="com.sgtm.LessPA"

# Single source of truth for the version: the project() line in CMakeLists.
VERSION="$(sed -n 's/^project(PAEchoCanceller VERSION \([0-9.]*\)).*/\1/p' \
    "$REPO_ROOT/plugin/CMakeLists.txt")"

if [[ -z "$VERSION" ]]; then
    echo "error: could not read the version from plugin/CMakeLists.txt" >&2
    exit 1
fi

for bundle in "$AU_BUNDLE" "$VST3_BUNDLE"; do
    if [[ ! -d "$bundle" ]]; then
        echo "error: missing $bundle" >&2
        echo "       Build the Release targets first -- see BUILDING.md." >&2
        exit 1
    fi
done

echo "Less PA $VERSION -- building installer"

lesspa_require_signing_identities --with-installer
lesspa_check_no_stray_dylibs "$AU_BUNDLE" "$VST3_BUNDLE"

# Sign, notarize and staple the plugins before they go into the package.
lesspa_prepare_bundles "$AU_BUNDLE" "$VST3_BUNDLE"

# Remove only this script's own outputs, not the whole build directory --
# build_zip.sh writes its zip here too.
rm -rf "$STAGE_DIR" "$BUILD_DIR/resources" "$BUILD_DIR/distribution.xml" \
    "$BUILD_DIR/LessPA-AU.pkg" "$BUILD_DIR/LessPA-VST3.pkg"
mkdir -p "$STAGE_DIR/au" "$STAGE_DIR/vst3"

# pkgbuild wants a directory whose contents get copied into --install-location,
# so stage each bundle on its own.
cp -R "$AU_BUNDLE" "$STAGE_DIR/au/"
cp -R "$VST3_BUNDLE" "$STAGE_DIR/vst3/"

# --install-location paths are relative to the install domain, and the
# distribution below enables only enable_currentUserHome -- so these resolve
# under the installing user's home directory, not /.
pkgbuild \
    --quiet \
    --root "$STAGE_DIR/au" \
    --identifier "$PKG_ID_BASE.au" \
    --version "$VERSION" \
    --install-location "/Library/Audio/Plug-Ins/Components" \
    "$BUILD_DIR/LessPA-AU.pkg"

pkgbuild \
    --quiet \
    --root "$STAGE_DIR/vst3" \
    --identifier "$PKG_ID_BASE.vst3" \
    --version "$VERSION" \
    --install-location "/Library/Audio/Plug-Ins/VST3" \
    "$BUILD_DIR/LessPA-VST3.pkg"

echo "  built component packages"

# Installer resources: the welcome text, plus the SGTM wordmark as background.
RESOURCES="$BUILD_DIR/resources"
mkdir -p "$RESOURCES"
cp "$HERE/welcome.txt" "$RESOURCES/welcome.txt"

BACKGROUND_XML=""
LOGO="$REPO_ROOT/plugin/Resources/sgtm_logo.png"
if [[ -f "$LOGO" ]]; then
    cp "$LOGO" "$RESOURCES/background.png"
    BACKGROUND_XML='<background file="background.png" alignment="bottomleft" scaling="proportional"/>'
fi

# customize="always" opens the Installation Type step on the AU/VST3 choice
# list itself. With "allow" the list hid behind a Customize button that
# was easy to miss, so the welcome text's promise of a choice went unmet.
cat > "$BUILD_DIR/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Less PA $VERSION</title>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    $BACKGROUND_XML
    <options customize="always" require-scripts="false" hostArchitectures="arm64"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
    <choices-outline>
        <line choice="au"/>
        <line choice="vst3"/>
    </choices-outline>
    <choice id="au" title="Audio Unit (AU)"
            description="Installs Less PA.component into ~/Library/Audio/Plug-Ins/Components. For Logic Pro, GarageBand, and other AU hosts.">
        <pkg-ref id="$PKG_ID_BASE.au"/>
    </choice>
    <choice id="vst3" title="VST3"
            description="Installs Less PA.vst3 into ~/Library/Audio/Plug-Ins/VST3. For Reaper, Ableton Live, Cubase, and other VST3 hosts.">
        <pkg-ref id="$PKG_ID_BASE.vst3"/>
    </choice>
    <pkg-ref id="$PKG_ID_BASE.au" version="$VERSION" onConclusion="none">LessPA-AU.pkg</pkg-ref>
    <pkg-ref id="$PKG_ID_BASE.vst3" version="$VERSION" onConclusion="none">LessPA-VST3.pkg</pkg-ref>
</installer-gui-script>
XML

FINAL_PKG="$BUILD_DIR/Less-PA-$VERSION.pkg"

# Only the final combined product is signed; the two component packages above
# are intermediates that get embedded into it, so signing them buys nothing.
productbuild \
    --quiet \
    --distribution "$BUILD_DIR/distribution.xml" \
    --package-path "$BUILD_DIR" \
    --resources "$RESOURCES" \
    --sign "$LESSPA_INSTALLER_IDENTITY" \
    "$FINAL_PKG"

echo "  signed the installer package"

lesspa_notarize_artifact "$FINAL_PKG"
xcrun stapler staple "$FINAL_PKG"

# Tidy up the intermediates so only the shippable .pkg is left behind.
rm -rf "$STAGE_DIR" "$RESOURCES" \
    "$BUILD_DIR/LessPA-AU.pkg" "$BUILD_DIR/LessPA-VST3.pkg" \
    "$BUILD_DIR/distribution.xml"

echo
echo "Verification:"
pkgutil --check-signature "$FINAL_PKG" | sed 's/^/  /'
xcrun stapler validate "$FINAL_PKG" | sed 's/^/  /'
spctl -a -vvv -t install "$FINAL_PKG" 2>&1 | sed 's/^/  /'

echo
echo "Installer: $FINAL_PKG"
echo "Size:      $(du -h "$FINAL_PKG" | cut -f1)"
