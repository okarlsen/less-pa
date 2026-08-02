#!/bin/bash
#
# Builds the Less PA macOS installer package.
#
# Produces packaging/build/Less-PA-<version>.pkg from an existing Release
# build in plugin/build (see BUILDING.md). Two pkgbuild components -- the AU
# and the VST3 -- are combined with productbuild into a single installer that
# writes into the current user's ~/Library/Audio/Plug-Ins, so it needs no
# administrator password.
#
# Usage: ./packaging/build_installer.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

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

# Refuse to ship a plugin that drags in dylibs the end user will not have.
# This is the exact failure mode a locally-installed Homebrew Abseil causes;
# see the 'A note on Abseil' section of BUILDING.md.
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

rm -rf "$BUILD_DIR"
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

cat > "$BUILD_DIR/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Less PA $VERSION</title>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    $BACKGROUND_XML
    <options customize="allow" require-scripts="false" hostArchitectures="arm64"/>
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

productbuild \
    --quiet \
    --distribution "$BUILD_DIR/distribution.xml" \
    --package-path "$BUILD_DIR" \
    --resources "$RESOURCES" \
    "$FINAL_PKG"

# Tidy up the intermediates so only the shippable .pkg is left behind.
rm -rf "$STAGE_DIR" "$RESOURCES" \
    "$BUILD_DIR/LessPA-AU.pkg" "$BUILD_DIR/LessPA-VST3.pkg" \
    "$BUILD_DIR/distribution.xml"

echo
echo "Installer: $FINAL_PKG"
echo "Size:      $(du -h "$FINAL_PKG" | cut -f1)"
