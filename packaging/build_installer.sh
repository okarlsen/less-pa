#!/bin/bash
#
# Builds the Less PA macOS installer package: signed with a Developer ID,
# notarized by Apple, and stapled.
#
# Produces packaging/build/Less-PA-<version>.pkg from an existing Release
# build in plugin/build (see BUILDING.md). One pkgbuild component per format
# -- the AU, the VST3 and, when a signed one is present, the AAX -- is
# combined with productbuild into a single installer.
#
# Everything installs system-wide, under /Library, so the installer asks for
# an administrator password. Pro Tools only scans
# /Library/Application Support/Avid/Audio/Plug-Ins, so the AAX has no
# per-user location to go to, and one installer with one destination is
# simpler than a per-user/all-users choice where a format silently drops out.
#
# The plugin bundles are signed, notarized and stapled *before* being staged
# into the package, so plugins installed from this .pkg carry their own
# notarization ticket and load without any network lookup. The finished .pkg
# is then signed with the Developer ID Installer certificate and notarized in
# its own right.
#
# The AAX is optional. This script never signs it: Pro Tools only loads an
# AAX signed with PACE's tools, which are not part of this repository, and a
# second codesign pass would strip that signature. An AAX bundle in the build
# is included if it is already PACE-signed and stapled, and left out with a
# note otherwise.
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
AAX_BUNDLE="$ARTEFACTS/AAX/Less PA.aaxplugin"

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

INCLUDE_AAX=0
if [[ -d "$AAX_BUNDLE" ]]; then
    if [[ -d "$AAX_BUNDLE/Contents/__Pace_Eden.bundle" ]] \
            && codesign --verify --deep --strict "$AAX_BUNDLE" 2>/dev/null \
            && lesspa_bundles_are_stapled "$AAX_BUNDLE"; then
        lesspa_check_no_stray_dylibs "$AAX_BUNDLE"
        INCLUDE_AAX=1
        echo "  including the AAX (PACE-signed and stapled)"
    else
        echo "  leaving out the AAX: it is not PACE-signed, notarized and stapled"
    fi
fi

# Remove only this script's own outputs, not the whole build directory --
# build_zip.sh writes its zip here too.
rm -rf "$STAGE_DIR" "$BUILD_DIR/resources" "$BUILD_DIR/distribution.xml" \
    "$BUILD_DIR/LessPA-AU.pkg" "$BUILD_DIR/LessPA-VST3.pkg" "$BUILD_DIR/LessPA-AAX.pkg"

# Up to 1.1.2 the installer wrote into the installing user's own
# ~/Library/Audio/Plug-Ins. A copy left there would sit beside the new
# system-wide one, and hosts that prefer the user folder would keep loading
# the old version, so each format's package removes its old per-user bundle
# first. Installer scripts run as root, so the user is taken from the console
# session rather than from \$HOME.
write_preinstall() {
    local scripts_dir="$1" old_relative_path="$2"
    mkdir -p "$scripts_dir"
    cat > "$scripts_dir/preinstall" <<SH
#!/bin/bash
user="\$(stat -f%Su /dev/console 2>/dev/null)"
if [[ -z "\$user" || "\$user" == "root" ]]; then
    exit 0
fi
home="\$(dscl . -read "/Users/\$user" NFSHomeDirectory 2>/dev/null | sed 's/^NFSHomeDirectory: //')"
old="\${LESSPA_TEST_HOME:-\$home}/$old_relative_path"
if [[ -n "\$home" && -d "\$old" ]]; then
    rm -rf "\$old"
fi
exit 0
SH
    chmod +x "$scripts_dir/preinstall"
}

# One component package per format. pkgbuild wants a directory whose contents
# get copied into --install-location, so each bundle is staged on its own.
#
# Where pkgbuild marks a bundle relocatable, the installer may "upgrade"
# another copy of it found on disk (a build folder, or the old per-user
# install) instead of writing to the install location, so that is pinned off.
build_component() {
    local key="$1" bundle="$2" install_location="$3" old_relative_path="${4:-}"
    local stage="$STAGE_DIR/$key" out="$5"
    local scripts_args=()

    mkdir -p "$stage/root"
    cp -R "$bundle" "$stage/root/"

    # pkgbuild only marks some bundle types relocatable (plugin bundles
    # usually are not), so the key is pinned only where it is present.
    pkgbuild --analyze --root "$stage/root" "$stage/component.plist" > /dev/null
    if /usr/libexec/PlistBuddy -c "Print :0:BundleIsRelocatable" "$stage/component.plist" > /dev/null 2>&1; then
        /usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" "$stage/component.plist"
    fi

    if [[ -n "$old_relative_path" ]]; then
        write_preinstall "$stage/scripts" "$old_relative_path"
        scripts_args=(--scripts "$stage/scripts")
    fi

    pkgbuild \
        --quiet \
        --root "$stage/root" \
        --component-plist "$stage/component.plist" \
        ${scripts_args[@]+"${scripts_args[@]}"} \
        --identifier "$PKG_ID_BASE.$key" \
        --version "$VERSION" \
        --install-location "$install_location" \
        "$out"
}

build_component au "$AU_BUNDLE" "/Library/Audio/Plug-Ins/Components" \
    "Library/Audio/Plug-Ins/Components/Less PA.component" "$BUILD_DIR/LessPA-AU.pkg"
build_component vst3 "$VST3_BUNDLE" "/Library/Audio/Plug-Ins/VST3" \
    "Library/Audio/Plug-Ins/VST3/Less PA.vst3" "$BUILD_DIR/LessPA-VST3.pkg"

AAX_LINE_XML=""
AAX_CHOICE_XML=""
if [[ $INCLUDE_AAX -eq 1 ]]; then
    build_component aax "$AAX_BUNDLE" "/Library/Application Support/Avid/Audio/Plug-Ins" \
        "" "$BUILD_DIR/LessPA-AAX.pkg"
    AAX_LINE_XML='<line choice="aax"/>'
    AAX_CHOICE_XML="<choice id=\"aax\" title=\"AAX (Pro Tools)\"
            description=\"Installs Less PA.aaxplugin into /Library/Application Support/Avid/Audio/Plug-Ins. For Pro Tools.\">
        <pkg-ref id=\"$PKG_ID_BASE.aax\"/>
    </choice>
    <pkg-ref id=\"$PKG_ID_BASE.aax\" version=\"$VERSION\" onConclusion=\"none\">LessPA-AAX.pkg</pkg-ref>"
fi

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

# customize="always" opens the Installation Type step on the format choice
# list itself. With "allow" the list hid behind a Customize button that
# was easy to miss, so the welcome text's promise of a choice went unmet.
cat > "$BUILD_DIR/distribution.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Less PA $VERSION</title>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    $BACKGROUND_XML
    <options customize="always" require-scripts="false" hostArchitectures="arm64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <choices-outline>
        <line choice="au"/>
        <line choice="vst3"/>
        $AAX_LINE_XML
    </choices-outline>
    <choice id="au" title="Audio Unit (AU)"
            description="Installs Less PA.component into /Library/Audio/Plug-Ins/Components. For Logic Pro, GarageBand, and other AU hosts.">
        <pkg-ref id="$PKG_ID_BASE.au"/>
    </choice>
    <choice id="vst3" title="VST3"
            description="Installs Less PA.vst3 into /Library/Audio/Plug-Ins/VST3. For Reaper, Ableton Live, Cubase, and other VST3 hosts.">
        <pkg-ref id="$PKG_ID_BASE.vst3"/>
    </choice>
    <pkg-ref id="$PKG_ID_BASE.au" version="$VERSION" onConclusion="none">LessPA-AU.pkg</pkg-ref>
    <pkg-ref id="$PKG_ID_BASE.vst3" version="$VERSION" onConclusion="none">LessPA-VST3.pkg</pkg-ref>
    $AAX_CHOICE_XML
</installer-gui-script>
XML

FINAL_PKG="$BUILD_DIR/Less-PA-$VERSION.pkg"

# Only the final combined product is signed; the component packages above
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
    "$BUILD_DIR/LessPA-AU.pkg" "$BUILD_DIR/LessPA-VST3.pkg" "$BUILD_DIR/LessPA-AAX.pkg" \
    "$BUILD_DIR/distribution.xml"

echo
echo "Verification:"
pkgutil --check-signature "$FINAL_PKG" | sed 's/^/  /'
xcrun stapler validate "$FINAL_PKG" | sed 's/^/  /'
spctl -a -vvv -t install "$FINAL_PKG" 2>&1 | sed 's/^/  /'

echo
echo "Installer: $FINAL_PKG"
echo "Size:      $(du -h "$FINAL_PKG" | cut -f1)"
