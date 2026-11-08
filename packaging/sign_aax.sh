#!/bin/bash
#
# PACE-signs the built Less PA AAX plugin in place so Pro Tools will load
# it, then has Apple notarize and staple it.
#
#     ./packaging/sign_aax.sh [path/to/Less PA.aaxplugin]
#
# Defaults to the AAX from a Release build configured with
# -DLESSPA_BUILD_AAX=ON. Set LESSPA_SKIP_NOTARIZE=1 for a local test build
# that only needs to load in Pro Tools on this Mac.
#
# Requires PACE's wraptool plus the iLok account ID and wrap config GUID --
# see lesspa_pace_sign_aax in signing.sh.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"

# shellcheck source=packaging/signing.sh
source "$HERE/signing.sh"

AAX_BUNDLE="${1:-$REPO_ROOT/plugin/build/PAEchoCanceller_artefacts/Release/AAX/Less PA.aaxplugin}"

if [[ ! -d "$AAX_BUNDLE" ]]; then
    echo "error: missing $AAX_BUNDLE" >&2
    echo "       Build the AAX target first -- see BUILDING.md." >&2
    exit 1
fi

echo "Less PA -- signing $(basename "$AAX_BUNDLE")"

lesspa_require_signing_identities
lesspa_check_no_stray_dylibs "$AAX_BUNDLE"
lesspa_pace_sign_aax "$AAX_BUNDLE"

if [[ -z "${LESSPA_SKIP_NOTARIZE:-}" ]]; then
    lesspa_notarize_and_staple_bundles "$AAX_BUNDLE"
fi
