#!/bin/sh
set -u
mkdir /tmp/nero
cp -R /source/src /source/include /source/third_party /source/tests /source/Makefile /tmp/nero/
cd /tmp/nero
run() { echo "COMMAND: $*"; "$@"; code=$?; echo "EXIT: $code"; return 0; }
run uname -a
run cat /etc/os-release
run gcc --version
run clang --version
run cmake --version
run ip addr
run make -B CC=gcc BUILD_DIR=/tmp/gcc-build TARGET=/tmp/nero-gcc
run make -B CC=clang BUILD_DIR=/tmp/clang-build TARGET=/tmp/nero-clang
run gcc -Wall -Wextra -Werror -pedantic -std=c11 -pthread tests/linux/scanner_runtime.c -o /tmp/scanner-runtime
run timeout 15 strace -f -e trace=socket,connect,getsockopt,close,select,pselect6 /tmp/scanner-runtime
run gcc -g -fsanitize=address,undefined -fno-omit-frame-pointer -pthread tests/linux/scanner_runtime.c -o /tmp/scanner-sanitize
run timeout 15 /tmp/scanner-sanitize
run gcc -Wall -Wextra -Werror -pedantic -std=c11 -Iinclude tests/interfaces.c src/platform/linux/select_interface.c -o /tmp/interfaces
printf '1junk\n' | /tmp/interfaces; echo "INVALID PREFIX EXIT: $?"
printf '1\n' | /tmp/interfaces; echo "VALID EXIT: $?"
run ss -tanup
run python3 tests/linux/interface_regression.py
printf 'version\nexit\n' | /tmp/nero-gcc; echo "PRODUCTION CLI EXIT: $?"
run python3 tests/linux/cli_signals.py
