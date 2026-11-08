#!/bin/sh
# Install the verified archive's native CLI into the system command directory.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${PREFIX:-/usr/local}
destination=${DESTDIR:-}${prefix}/bin
if [ ! -f "$script_dir/bin/nero" ]; then
    echo "Missing bin/nero beside installer" >&2
    exit 1
fi
install -d "$destination"
temporary=$(mktemp "$destination/.nero-install.XXXXXX")
trap 'rm -f "$temporary"' EXIT HUP INT TERM
install -m 755 "$script_dir/bin/nero" "$temporary"
mv -f "$temporary" "$destination/nero"
printf 'Installed %s/nero\nRun nero, or sudo nero when administrator access is needed.\n' "$destination"
