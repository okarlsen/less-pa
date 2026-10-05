#!/bin/bash
#
# Shared Developer ID signing / notarization helpers for the Less PA
# packaging scripts. Sourced by build_installer.sh and build_beta_zip.sh --
# not meant to be run directly.
#
# Both scripts need the identical "check the bundles, sign them, get them
# notarized and stapled" sequence, so it lives here once. The notarization
# step is idempotent: if the bundles already carry a valid stapled ticket it
# does nothing, so running both scripts back to back costs one trip to
# Apple's notary service, not two.
#
# Credentials: none are stored here or anywhere else in the repo. Signing
# uses the Developer ID certificates in the login keychain, and notarization
# uses a notarytool keychain profile (default name: "notarytool") created
# once with:
#
#     xcrun notarytool store-credentials notarytool \
#         --apple-id <apple-id> --team-id <team-id> --password <app-specific-password>
#
# See the "Code signing" section of BUILDING.md.

# Overridable so a fork can sign with its own identities without editing
# the scripts. Defaults are SGTM's (Team ID ZVP9U3LWAJ).
: "${LESSPA_SIGN_IDENTITY:=Developer ID Application: Sounds Good To Me AS (ZVP9U3LWAJ)}"
: "${LESSPA_INSTALLER_IDENTITY:=Developer ID Installer: Sounds Good To Me AS (ZVP9U3LWAJ)}"
: "${LESSPA_NOTARY_PROFILE:=notarytool}"

# Refuse to ship a plugin that drags in dylibs the end user will not have
# (as a locally-installed Homebrew library once did, back when the build
# linked an external DSP library).
lesspa_check_no_stray_dylibs() {
    local bundle binary strays
    for bundle in "$@"; do
        binary="$bundle/Contents/MacOS/Less PA"
        strays="$(otool -L "$binary" | tail -n +2 \
            | grep -v -e '/System/Library/' -e '/usr/lib/' || true)"
        if [[ -n "$strays" ]]; then
            echo "error: $(basename "$bundle") links against non-system libraries:" >&2
            echo "$strays" >&2
            echo "       These will not exist on an end user's Mac. See BUILDING.md." >&2
            return 1
        fi
    done
    echo "  checked: no non-system dynamic dependencies"
}

# Fail early and clearly if the machine can't sign, rather than part way
# through a build or -- worse -- silently shipping an ad-hoc signature.
lesspa_require_signing_identities() {
    local missing=0
    if ! security find-identity -v -p basic | grep -qF "$LESSPA_SIGN_IDENTITY"; then
        echo "error: no such signing identity in the keychain:" >&2
        echo "       $LESSPA_SIGN_IDENTITY" >&2
        missing=1
    fi
    if [[ "${1:-}" == "--with-installer" ]]; then
        if ! security find-identity -v -p basic | grep -qF "$LESSPA_INSTALLER_IDENTITY"; then
            echo "error: no such installer identity in the keychain:" >&2
            echo "       $LESSPA_INSTALLER_IDENTITY" >&2
            missing=1
        fi
    fi
    if [[ $missing -ne 0 ]]; then
        echo "       See the 'Code signing' section of BUILDING.md." >&2
        return 1
    fi
}

# Developer ID + hardened runtime, timestamped.
#
# No entitlements file is passed, and none is needed: each bundle contains a
# single Mach-O with no nested code, no JIT, no dyld interposing and no
# special capabilities. The microphone access these plugins operate on is
# owned and declared by the host application, not by the plugin.
lesspa_sign_bundles() {
    local bundle
    for bundle in "$@"; do
        codesign --force --options runtime --timestamp \
            --sign "$LESSPA_SIGN_IDENTITY" "$bundle"
        codesign --verify --deep --strict --verbose=2 "$bundle"
        echo "  signed: $(basename "$bundle")"
    done
}

# The one entry point the packaging scripts should call: leaves both bundles
# Developer ID signed, hardened, notarized and stapled.
#
# Idempotent, and deliberately so. Stapling *adds* a Contents/CodeResources
# ticket to the bundle, so a second `codesign --force` pass would re-seal the
# bundle's resources and invalidate the ticket that was just stapled to it --
# quietly un-notarizing a plugin that had been fine. Checking staple state
# first means running build_installer.sh and build_zip.sh back to back is
# safe in either order.
lesspa_prepare_bundles() {
    if lesspa_bundles_are_stapled "$@"; then
        echo "  already signed, notarized and stapled -- nothing to do"
        return 0
    fi
    lesspa_sign_bundles "$@"
    lesspa_notarize_and_staple_bundles "$@"
}

# True when every given bundle already carries a valid stapled ticket.
lesspa_bundles_are_stapled() {
    local bundle
    for bundle in "$@"; do
        xcrun stapler validate "$bundle" >/dev/null 2>&1 || return 1
    done
}

# Submit to Apple, wait, and report honestly on failure. $1 is the artifact
# (zip/pkg/dmg -- notarytool does not accept a raw bundle); the rest of the
# output is left to the caller.
lesspa_notarize_artifact() {
    local artifact="$1"
    # Deliberately not named "status": that is a read-only special variable
    # in zsh, so the plain name makes this file a footgun if anyone sources
    # it from an interactive zsh rather than running it under bash.
    local json notary_status submission_id

    echo "  submitting $(basename "$artifact") to the Apple notary service (this takes a few minutes)..."

    if ! json="$(xcrun notarytool submit "$artifact" \
            --keychain-profile "$LESSPA_NOTARY_PROFILE" \
            --output-format json --wait 2>&1)"; then
        echo "error: notarytool submit failed:" >&2
        echo "$json" >&2
        return 1
    fi

    notary_status="$(printf '%s' "$json" | jq -r '.status // "unknown"' 2>/dev/null || echo unknown)"
    submission_id="$(printf '%s' "$json" | jq -r '.id // empty' 2>/dev/null || true)"

    if [[ "$notary_status" != "Accepted" ]]; then
        echo "error: notarization did not succeed (status: $notary_status)" >&2
        if [[ -n "$submission_id" ]]; then
            echo "       fetching the notary log for $submission_id:" >&2
            xcrun notarytool log "$submission_id" \
                --keychain-profile "$LESSPA_NOTARY_PROFILE" >&2 || true
        fi
        return 1
    fi

    echo "  notarized: $(basename "$artifact") (submission $submission_id)"
}

# Notarize both plugin bundles in a single submission and staple each.
#
# notarytool wants an archive, and a notarization ticket is keyed by the
# cdhash of each binary it covers -- so one zip containing both bundles
# yields a ticket that staples to either bundle individually. That halves
# the round trips compared with submitting them separately.
lesspa_notarize_and_staple_bundles() {
    local bundles=("$@")
    local tmp zip bundle rc=0

    if lesspa_bundles_are_stapled "${bundles[@]}"; then
        echo "  already notarized and stapled -- skipping submission"
        return 0
    fi

    tmp="$(mktemp -d)"

    mkdir -p "$tmp/bundles"
    for bundle in "${bundles[@]}"; do
        cp -R "$bundle" "$tmp/bundles/"
    done

    zip="$tmp/LessPA-bundles.zip"
    (cd "$tmp" && ditto -c -k --sequesterRsrc --keepParent bundles "$zip")

    # Explicit cleanup rather than a RETURN trap: that trap is a bash-only
    # construct and silently breaks under other shells.
    lesspa_notarize_artifact "$zip" || rc=$?
    rm -rf "$tmp"
    [[ $rc -eq 0 ]] || return $rc

    for bundle in "${bundles[@]}"; do
        xcrun stapler staple "$bundle"
        xcrun stapler validate "$bundle"
        echo "  stapled: $(basename "$bundle")"
    done
}
