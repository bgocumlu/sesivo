#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"

read_project_version() {
    local version
    version="$(sed -nE 's/^[[:space:]]*set[[:space:]]*\([[:space:]]*SESIVO_VERSION[[:space:]]+"([^"]+)"[[:space:]]*\).*/\1/p' "${repo_dir}/CMakeLists.txt" | head -n 1)"
    if [[ -z "$version" ]]; then
        echo "could not read SESIVO_VERSION from CMakeLists.txt" >&2
        exit 2
    fi
    printf '%s\n' "$version"
}

require_env() {
    local name="$1"
    if [[ -z "${!name:-}" ]]; then
        echo "missing required env var: $name" >&2
        exit 2
    fi
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "missing required command: $1" >&2
        exit 2
    fi
}

require_env CSC_LINK
require_env CSC_KEY_PASSWORD
require_env APPLE_API_KEY
require_env APPLE_API_KEY_ID
require_env APPLE_API_ISSUER

require_command cmake
require_command cpack
require_command codesign
require_command openssl
require_command security
require_command xcrun
require_command spctl

if [[ ! -f "$CSC_LINK" ]]; then
    echo "CSC_LINK does not point to a file: $CSC_LINK" >&2
    exit 2
fi
if [[ ! -f "$APPLE_API_KEY" ]]; then
    echo "APPLE_API_KEY does not point to a file: $APPLE_API_KEY" >&2
    exit 2
fi

build_dir="${BUILD_DIR:-build}"
config="${CONFIG:-Release}"
package_dir="${PACKAGE_DIR:-${build_dir}/package}"
app_path="${build_dir}/Sesivo.app"
app_version="${APP_VERSION:-$(read_project_version)}"
dmg_path="${package_dir}/sesivo-${app_version}-macos-arm64.dmg"
entitlements="packaging/macos/entitlements.plist"

keychain="$(mktemp -u "/tmp/sesivo-signing.XXXXXX.keychain-db")"
keychain_password="$(openssl rand -hex 24)"
original_keychains="$(security list-keychains -d user | tr -d ' "')"

cleanup() {
    if [[ -n "$original_keychains" ]]; then
        # shellcheck disable=SC2086
        security list-keychains -d user -s $original_keychains >/dev/null 2>&1 || true
    fi
    security delete-keychain "$keychain" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cmake -S . -B "$build_dir" -DCMAKE_BUILD_TYPE="$config" -DJAM_BUILD_CLIENT=ON
cmake --build "$build_dir" --target client --config "$config"

security create-keychain -p "$keychain_password" "$keychain"
security list-keychains -d user -s "$keychain" $original_keychains
security set-keychain-settings -lut 21600 "$keychain"
security unlock-keychain -p "$keychain_password" "$keychain"
security import "$CSC_LINK" -k "$keychain" -P "$CSC_KEY_PASSWORD" -T /usr/bin/codesign
security set-key-partition-list -S apple-tool:,apple:,codesign: \
    -s -k "$keychain_password" "$keychain" >/dev/null

identity="$(security find-identity -v -p codesigning "$keychain" |
    awk '/Developer ID Application/ { print $2; exit }')"
if [[ -z "$identity" ]]; then
    echo "no Developer ID Application identity found in temporary keychain" >&2
    exit 2
fi

codesign --force --timestamp --options runtime \
    --entitlements "$entitlements" \
    --keychain "$keychain" \
    --sign "$identity" \
    "$app_path"

codesign --verify --deep --strict --verbose=4 "$app_path"

rm -rf "$package_dir"
cpack --config "$build_dir/CPackConfig.cmake" -G DragNDrop -C "$config" -B "$package_dir"

codesign --force --timestamp \
    --keychain "$keychain" \
    --sign "$identity" \
    "$dmg_path"
codesign --verify --verbose=4 "$dmg_path"

xcrun notarytool submit "$dmg_path" \
    --key "$APPLE_API_KEY" \
    --key-id "$APPLE_API_KEY_ID" \
    --issuer "$APPLE_API_ISSUER" \
    --wait

xcrun stapler staple "$dmg_path"
xcrun stapler validate "$dmg_path"
spctl -a -vv -t install "$dmg_path"
spctl -a -vv --type execute "$app_path"

echo "Released $dmg_path"
