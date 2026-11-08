#!/bin/sh
# Install the verified archive's native CLI into the system command directory.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${PREFIX:-/usr/local}
destination=${DESTDIR:-}${INSTALL_DIR:-${prefix}/bin}
binary=${NERO_INSTALL_BINARY:-$script_dir/bin/nero}
if [ ! -f "$binary" ]; then
    echo "Missing bin/nero beside installer" >&2
    exit 1
fi
if ! install -d "$destination"; then
    echo 'System installation requires administrator access: use sudo make install for a source checkout, or sudo sh install.sh for an extracted release.' >&2
    exit 1
fi
if [ -d "$destination/nero" ]; then
    echo "Refusing to replace a directory at $destination/nero" >&2
    exit 1
fi
temporary=$(mktemp "$destination/.nero-install.XXXXXX")
trap 'rm -f "$temporary"' EXIT HUP INT TERM
install -m 755 "$binary" "$temporary"
mv -f "$temporary" "$destination/nero"
printf 'Installed %s/nero\nRun nero, or sudo nero when administrator access is needed.\n' "$destination"
