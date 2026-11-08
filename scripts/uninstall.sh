#!/bin/sh
# Remove only the installed executable; retain user history and application logs.
set -eu
prefix=${PREFIX:-/usr/local}
destination=${DESTDIR:-}${prefix}/bin
rm -f "$destination/nero"
printf 'Removed %s/nero (user data retained).\n' "$destination"
