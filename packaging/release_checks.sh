#!/bin/bash
#
# Release-build checks for the Less PA packaging scripts. Sourced by
# build_installer.sh and build_zip.sh -- not meant to be run directly.
#
# The packaging scripts take the version from plugin/CMakeLists.txt and the
# bundles from plugin/build, and nothing else ties the two together. These
# checks refuse a build folder that would ship the wrong thing under the
# release's name: one configured for a labelled test build (the panel would
# read "v1.1.5 RC"), one not built as Release, or bundles built before a
# version bump.

# $1: the CMake build directory. Reads what the last configure set.
lesspa_check_release_configuration() {
    local build_dir="$1" label_file build_type
    label_file="$build_dir/lesspa_build_label.txt"
    if [[ ! -f "$label_file" ]]; then
        echo "error: $label_file is missing; the build folder predates the label check." >&2
        echo "       Reconfigure: cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DLESSPA_BUILD_LABEL=" >&2
        return 1
    fi
    if [[ -s "$label_file" ]]; then
        echo "error: the build is labelled '$(cat "$label_file")', a test build." >&2
        echo "       Reconfigure with -DLESSPA_BUILD_LABEL= (empty) and rebuild. See BUILDING.md." >&2
        return 1
    fi
    build_type="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$build_dir/CMakeCache.txt" 2>/dev/null)"
    if [[ "$build_type" != "Release" ]]; then
        echo "error: the build type is '${build_type:-unknown}', expected Release." >&2
        echo "       Reconfigure with -DCMAKE_BUILD_TYPE=Release and rebuild. See BUILDING.md." >&2
        return 1
    fi
    echo "  checked: Release build, no test label"
}

# $1: the version the release is named after; the rest: bundles. Each
# bundle's own version must be it (a build from before a version bump
# carries the old one).
lesspa_check_bundle_versions() {
    local version="$1" bundle bundle_version
    shift
    for bundle in "$@"; do
        bundle_version="$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" \
            "$bundle/Contents/Info.plist" 2>/dev/null || true)"
        if [[ "$bundle_version" != "$version" ]]; then
            echo "error: $(basename "$bundle") is version '${bundle_version:-unknown}', expected $version." >&2
            echo "       Rebuild after the version bump. See BUILDING.md." >&2
            return 1
        fi
    done
    echo "  checked: bundles are version $version"
}
