#!/bin/sh
# Exercise Apple Installer install, upgrade and complete removal of the NERO
# .pkg on a disposable CI runner. Every mode uses sudo and changes /usr/local.
#
#   sh tests/macos/pkg_lifecycle.sh remove
#   sh tests/macos/pkg_lifecycle.sh upgrade OLDER.pkg OLDER_TAG NEWER.pkg NEWER_TAG
#
# Package identifier and payload layout come from scripts/release/package_macos.py.
set -eu

PKG_ID=com.thekiran.nero
COMMAND=usr/local/bin/nero
# The only directory NERO owns; shared prefixes such as /usr/local/bin stay.
DOC_DIR=usr/local/share/doc/nero

fail() {
    printf 'FAIL %s\n' "$*" >&2
    exit 1
}

[ "$(uname -s)" = Darwin ] || fail 'requires macOS'
[ "${GITHUB_ACTIONS:-}" = true ] || fail 'refusing to modify /usr/local outside a disposable CI runner'

export NERO_LOG_FILE=:memory: NERO_HISTORY_FILE=:memory: NO_COLOR=1 NERO_ASCII=1
work=$(mktemp -d "${TMPDIR:-/tmp}/nero-pkg.XXXXXX")
trap 'rm -rf "$work"' EXIT

has_receipt() {
    pkgutil --pkg-info "$PKG_ID" >/dev/null 2>&1
}

# Receipt paths are relative to the recorded volume and install location.
receipt_root() {
    pkgutil --pkg-info "$PKG_ID" > "$work/info"
    volume=$(sed -n 's/^volume: //p' "$work/info")
    location=$(sed -n 's/^location: //p' "$work/info")
    root="${volume%/}/${location#/}"
    printf '%s\n' "${root%/}"
}

check_installed() {
    tag=$1
    numeric=${tag#v}
    numeric=${numeric%%-*}
    actual=$(pkgutil --pkg-info "$PKG_ID" | sed -n 's/^version: //p')
    [ "$actual" = "$numeric" ] || fail "receipt version $actual, expected $numeric"
    resolved=$(env PATH="$PATH" sh -c 'command -v nero') || fail 'nero is not on PATH'
    [ "$resolved" = "/$COMMAND" ] || fail "nero resolved to $resolved"
    (cd "$work" && printf 'version\nexit\n' | nero) > "$work/version"
    grep -F "version $tag" "$work/version" >/dev/null || fail "nero version does not report $tag"
    grep -F "\"version\": \"$tag\"" "/$DOC_DIR/BUILD-INFO.json" >/dev/null ||
        fail "BUILD-INFO.json does not record $tag"
    printf 'PASS installed %s (receipt %s)\n' "$tag" "$actual"
}

remove_pkg() {
    has_receipt || fail "no $PKG_ID receipt to remove"
    root=$(receipt_root)
    # Capture the payload before the receipt is forgotten.
    pkgutil --only-files --files "$PKG_ID" > "$work/files"
    pkgutil --only-dirs --files "$PKG_ID" > "$work/dirs"
    grep -Fx "$COMMAND" "$work/files" >/dev/null || fail "receipt does not list /$COMMAND"
    grep -Fx "$DOC_DIR" "$work/dirs" >/dev/null || fail "receipt does not list /$DOC_DIR"
    while IFS= read -r path; do
        sudo rm -f "$root/$path"
    done < "$work/files"
    # Remove NERO-owned directories deepest first; rmdir fails on unknown leftovers.
    grep -x -e "$DOC_DIR" -e "$DOC_DIR/.*" "$work/dirs" | sort -r > "$work/owned"
    while IFS= read -r path; do
        sudo rmdir "$root/$path"
    done < "$work/owned"
    sudo pkgutil --forget "$PKG_ID"

    cat "$work/files" "$work/owned" > "$work/payload"
    while IFS= read -r path; do
        if [ -e "$root/$path" ] || [ -L "$root/$path" ]; then
            fail "removal left $root/$path behind"
        fi
    done < "$work/payload"
    ! has_receipt || fail "$PKG_ID receipt remains"
    ! env PATH="$PATH" sh -c 'command -v nero' >/dev/null || fail 'nero still resolves after removal'
    ! sudo -n sh -c 'command -v nero' >/dev/null || fail 'nero still resolves under sudo after removal'
    printf 'PASS removed %s payload and receipt\n' "$PKG_ID"
}

upgrade() {
    older=$1 older_tag=$2 newer=$3 newer_tag=$4
    [ "$older_tag" != "$newer_tag" ] || fail 'upgrade fixture must use a different version'
    ! has_receipt || fail "disposable runner already has a $PKG_ID receipt"
    [ ! -e "/$COMMAND" ] || fail "disposable runner already has /$COMMAND"
    sudo installer -pkg "$older" -target /
    check_installed "$older_tag"
    sudo installer -pkg "$newer" -target /
    check_installed "$newer_tag"
    printf 'PASS upgraded %s to %s\n' "$older_tag" "$newer_tag"
    remove_pkg
}

case "${1:-}" in
    remove)
        [ "$#" -eq 1 ] || fail 'usage: pkg_lifecycle.sh remove'
        remove_pkg
        ;;
    upgrade)
        [ "$#" -eq 5 ] || fail 'usage: pkg_lifecycle.sh upgrade OLDER.pkg OLDER_TAG NEWER.pkg NEWER_TAG'
        upgrade "$2" "$3" "$4" "$5"
        ;;
    *)
        fail 'usage: pkg_lifecycle.sh remove | upgrade OLDER.pkg OLDER_TAG NEWER.pkg NEWER_TAG'
        ;;
esac
