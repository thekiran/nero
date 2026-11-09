#!/bin/sh
# Remove only the installed executable; retain user history and application logs.
set -eu
prefix=${PREFIX:-/usr/local}
# Resolve the same destination install.sh used, so a custom INSTALL_DIR install
# is actually removed instead of silently left behind.
destination=${DESTDIR:-}${INSTALL_DIR:-${prefix}/bin}
if [ -e "$destination/nero" ]; then
    rm -f "$destination/nero"
    printf 'Removed %s/nero (user data retained).\n' "$destination"
else
    printf 'No nero binary found at %s/nero (nothing removed).\n' "$destination"
fi
