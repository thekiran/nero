#!/bin/sh
set -u

NERO_TMP=$(mktemp -d /tmp/nero-test.XXXXXX) || exit 1
trap 'rm -rf "$NERO_TMP"' EXIT

cp -R /source/src /source/include /source/third_party /source/tests /source/Makefile "$NERO_TMP"/
cd "$NERO_TMP" || exit 1

FAILURES=0
run() {
    echo "COMMAND: $*"
    "$@"
    code=$?
    echo "EXIT: $code"
    if [ "$code" -ne 0 ]; then
        echo "FAILURE: $* (exit $code)"
        FAILURES=$((FAILURES + 1))
    fi
    return "$code"
}

run uname -a
run cat /etc/os-release
run gcc --version
run clang --version
run cmake --version
run ip addr
run make -B CC=gcc BUILD_DIR="$NERO_TMP/gcc-build" TARGET="$NERO_TMP/nero-gcc"
run make -B CC=clang BUILD_DIR="$NERO_TMP/clang-build" TARGET="$NERO_TMP/nero-clang"
run gcc -Wall -Wextra -Werror -pedantic -std=c11 -pthread tests/linux/scanner_runtime.c -o "$NERO_TMP/scanner-runtime"
run timeout 15 strace -f -e trace=socket,connect,getsockopt,close,select,pselect6 "$NERO_TMP/scanner-runtime"
run gcc -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread tests/linux/scanner_runtime.c -o "$NERO_TMP/scanner-sanitize"
run timeout 15 "$NERO_TMP/scanner-sanitize"
run gcc -Wall -Wextra -Werror -pedantic -std=c11 -Iinclude tests/interfaces.c src/platform/linux/select_interface.c -o "$NERO_TMP/interfaces"

printf '1junk\n' | "$NERO_TMP/interfaces"; code=$?; echo "INVALID PREFIX EXIT: $code"
[ "$code" -eq 1 ] || FAILURES=$((FAILURES + 1))
printf '1\n' | "$NERO_TMP/interfaces"; code=$?; echo "VALID EXIT: $code"
[ "$code" -eq 0 ] || FAILURES=$((FAILURES + 1))
run ss -tanup
run python3 tests/linux/interface_regression.py "$NERO_TMP/interfaces"
printf 'version\nexit\n' | "$NERO_TMP/nero-gcc"; code=$?; echo "PRODUCTION CLI EXIT: $code"
[ "$code" -eq 0 ] || FAILURES=$((FAILURES + 1))
run python3 tests/linux/cli_signals.py "$NERO_TMP/nero-gcc"

echo "TOTAL FAILURES: $FAILURES"
[ "$FAILURES" -eq 0 ]
