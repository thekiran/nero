# Nero - platform builds, offline tests, and static analysis

CC ?= cc

COMMON_CFLAGS := -Wall -Wextra -Werror -pedantic -std=c11 \
                 -g -Iinclude -Ithird_party/cjson -DUNICODE_SUPPORT

CFLAGS := $(COMMON_CFLAGS)
# Stamp the tag only into a release build. Source builds remain "development".
ifneq ($(strip $(NERO_RELEASE_VERSION)),)
    CPPFLAGS += -DNERO_BUILD_VERSION='"$(NERO_RELEASE_VERSION)"'
endif
LDFLAGS ?=
LDLIBS  += -pthread

UNAME_S := $(shell uname -s)

CLI_SUPPORT_SOURCES := \
    src/core/log.c \
    src/core/doctor.c \
    src/core/platform.c \
    src/core/hardware.c \
    src/cli/parser/parser.c \
    src/cli/input.c \
    src/cli/history.c \
    src/cli/render.c \
    src/cli/operation_ui.c \
    src/cli/commands.c \
    src/cli/workload.c \
    src/cli/ascii.c

CLI_SOURCES := src/cli/terminal.c $(CLI_SUPPORT_SOURCES)
CLI_HEADERS := $(wildcard include/nero/*.h src/cli/*.h src/modules/*internal.h src/modules/syn/*.h)
SCANNER_SOURCES := src/modules/portscanner.c src/modules/scan_session.c
SYN_SOURCES := \
    src/modules/syn_flood_powered.c \
    src/modules/syn/syn_settings.c \
    src/modules/syn/syn_ui.c \
    src/modules/syn/syn_preflight.c \
    src/modules/syn/syn_report.c

COMMON_SOURCES := \
    src/main.c \
    third_party/cjson/cJSON.c \
    $(CLI_SOURCES) \
    $(SCANNER_SOURCES) \
    $(SYN_SOURCES)

ifeq ($(UNAME_S),Darwin)
    INTERFACE_SOURCE := src/platform/macos/select_interface_macos.c
    PLATFORM_SOURCES := $(INTERFACE_SOURCE)
    MACOS_FRAMEWORKS := -framework CoreFoundation -framework SystemConfiguration
    LDLIBS += $(MACOS_FRAMEWORKS)
else ifeq ($(UNAME_S),Linux)
    INTERFACE_SOURCE := src/platform/linux/select_interface.c
    PLATFORM_SOURCES := $(INTERFACE_SOURCE)
    CFLAGS += -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
else
    $(error Unsupported platform: $(UNAME_S))
endif

SRCS := $(COMMON_SOURCES) $(PLATFORM_SOURCES)

BUILD_DIR := .build
OBJS := $(addprefix $(BUILD_DIR)/obj/,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)

TARGET := nero
PREFIX ?= /usr/local
DESTDIR ?=
INSTALL_DIR ?= $(PREFIX)/bin

.PHONY: all clean install uninstall run help test test-sanitize test-tsan analyze
.DEFAULT_GOAL := all

all: $(TARGET)
	@echo "[INFO] Built $(TARGET)"

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "[INFO] Linked $(TARGET) successfully"

$(BUILD_DIR)/obj/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	@echo "[CC] Compiling $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

run: $(TARGET)
	@echo "[RUN] Launching interactive CLI"
	./$(TARGET)

install: $(TARGET)
	@echo "[INSTALL] Installing $(TARGET) to $(INSTALL_DIR)"
	NERO_INSTALL_BINARY="$(abspath $(TARGET))" INSTALL_DIR="$(INSTALL_DIR)" DESTDIR="$(DESTDIR)" sh scripts/install.sh

uninstall:
	rm -f "$(DESTDIR)$(INSTALL_DIR)/nero"

clean:
	@echo "[CLEAN] Removing build artifacts and $(TARGET)"
	rm -rf $(BUILD_DIR) $(TARGET)

-include $(DEPS)

# Offline regression tests. These targets must not initiate network traffic.
SAFE_FLAGS := $(COMMON_CFLAGS) -DNERO_SAFE_TEST
ifeq ($(UNAME_S),Linux)
    SAFE_FLAGS += -D_POSIX_C_SOURCE=200809L
endif

SAFE_SOURCES := src/main.c $(CLI_SOURCES)
MOCK_SOURCES := $(SAFE_SOURCES) tests/mocks/network_handler.c
FAULT_SOURCES := src/main.c tests/mocks/terminal_faults.c tests/mocks/input_faults.c \
                 $(filter-out src/cli/input.c,$(CLI_SUPPORT_SOURCES)) tests/mocks/network_handler.c

TEST_DIR ?= $(BUILD_DIR)/tests
SANITIZERS ?=

$(TEST_DIR):
	@mkdir -p $@

$(TEST_DIR)/nero-safe: $(SAFE_SOURCES) $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) -DNERO_NETWORK_UNAVAILABLE $(SANITIZERS) $(SAFE_SOURCES) -o $@ -pthread

$(TEST_DIR)/interfaces: tests/interfaces.c $(INTERFACE_SOURCE) include/nero/select_interface.h Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/interfaces.c $(INTERFACE_SOURCE) -o $@ $(MACOS_FRAMEWORKS)

$(TEST_DIR)/nero-mock: $(MOCK_SOURCES) $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) $(MOCK_SOURCES) -o $@ -pthread

$(TEST_DIR)/nero-faults: $(FAULT_SOURCES) src/cli/input.c src/cli/terminal.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) $(FAULT_SOURCES) -o $@ -pthread

$(TEST_DIR)/operation-layout: tests/operation_layout_fixture.c src/cli/operation_ui.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/operation_layout_fixture.c src/cli/operation_ui.c -o $@

$(TEST_DIR)/portscanner-reliability: tests/portscanner_reliability.c $(SCANNER_SOURCES) src/cli/operation_ui.c src/core/log.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) -DNERO_SCAN_TEST_HOOKS tests/portscanner_reliability.c $(SCANNER_SOURCES) src/cli/operation_ui.c src/core/log.c -o $@ -pthread

# Production scanner against real 127.0.0.1 sockets and ptys (Linux, macOS).
# A connect() guard aborts the fixture on any non-loopback destination.
$(TEST_DIR)/scanner-runtime: tests/linux/scanner_runtime.c $(SCANNER_SOURCES) src/cli/operation_ui.c src/core/log.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) -DNERO_SCAN_TEST_HOOKS tests/linux/scanner_runtime.c $(SCANNER_SOURCES) src/cli/operation_ui.c src/core/log.c -o $@ -pthread

$(TEST_DIR)/scan-session: tests/scan_session.c src/modules/scan_session.c src/core/log.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/scan_session.c src/modules/scan_session.c src/core/log.c -o $@ -pthread

$(TEST_DIR)/logging-fixture: tests/logging_fixture.c src/core/log.c include/nero/log.h include/nero/private_file.h Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/logging_fixture.c src/core/log.c -o $@ -pthread

$(TEST_DIR)/parser-boundaries: tests/parser_boundaries.c src/cli/parser/parser.c include/nero/cli/parser.h Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/parser_boundaries.c src/cli/parser/parser.c -o $@

# Deterministic hardware recommendation and simulation-bound parser tests.
# Fixtures inject fake CPU/RAM data; the real machine probe is never asserted.
$(TEST_DIR)/workload-profile: tests/workload_profile.c src/core/hardware.c src/core/platform.c include/nero/hardware.h include/nero/platform.h Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) -DNERO_HARDWARE_TEST_HOOKS tests/workload_profile.c src/core/hardware.c src/core/platform.c -o $@

# Deterministic offline test for the extracted -Syn settings prompt. stdin is a
# pipe and stdout is /dev/null, so no terminal device or network is involved.
$(TEST_DIR)/syn-settings: tests/syn_settings.c src/modules/syn/syn_settings.c src/modules/syn/syn_internal.h src/core/hardware.c src/core/platform.c src/core/log.c include/nero/hardware.h Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) -DNERO_HARDWARE_TEST_HOOKS tests/syn_settings.c src/modules/syn/syn_settings.c src/core/hardware.c src/core/platform.c src/core/log.c -o $@ -pthread

# Deterministic PTY smoke test for live early port selection (macOS/Linux). Mock
# sockets only, no real traffic; defines NERO_SCAN_TEST_HOOKS itself for the
# scanner's test-only, timing-free synchronization signals.
$(TEST_DIR)/live-select-pty: tests/linux/live_select_pty.c $(SCANNER_SOURCES) src/cli/operation_ui.c src/core/log.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) -DNERO_SCAN_TEST_HOOKS tests/linux/live_select_pty.c $(SCANNER_SOURCES) src/cli/operation_ui.c src/core/log.c -o $@ -pthread

TEST_BINS := $(TEST_DIR)/nero-safe $(TEST_DIR)/interfaces \
             $(TEST_DIR)/nero-mock $(TEST_DIR)/nero-faults \
             $(TEST_DIR)/operation-layout $(TEST_DIR)/portscanner-reliability $(TEST_DIR)/logging-fixture \
             $(TEST_DIR)/scanner-runtime $(TEST_DIR)/live-select-pty $(TEST_DIR)/parser-boundaries $(TEST_DIR)/scan-session \
             $(TEST_DIR)/workload-profile $(TEST_DIR)/syn-settings

ifeq ($(UNAME_S),Darwin)
$(TEST_DIR)/interfaces-mock: tests/interfaces_mock_macos.c src/platform/macos/select_interface_macos.c Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) $< -o $@ $(MACOS_FRAMEWORKS)
TEST_BINS += $(TEST_DIR)/interfaces-mock
endif

ifeq ($(UNAME_S),Linux)
$(TEST_DIR)/interfaces-mock: tests/interfaces.c tests/mocks/interfaces_linux.c src/platform/linux/select_interface.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/interfaces.c tests/mocks/interfaces_linux.c src/platform/linux/select_interface.c -Wl,--wrap=opendir,--wrap=getifaddrs,--wrap=freeifaddrs -o $@
TEST_BINS += $(TEST_DIR)/interfaces-mock

$(TEST_DIR)/interface-fallback: tests/linux/interface_fallback.c src/platform/linux/select_interface.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/linux/interface_fallback.c src/platform/linux/select_interface.c -Wl,--wrap=opendir,--wrap=getifaddrs,--wrap=freeifaddrs -o $@
TEST_BINS += $(TEST_DIR)/interface-fallback
endif

$(TEST_DIR)/diagnostic-process: tests/diagnostic_process.c include/nero/diagnostic_process.h Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/diagnostic_process.c -o $@

TEST_BINS += $(TEST_DIR)/diagnostic-process

test: $(TEST_BINS)
ifeq ($(UNAME_S),Linux)
	$(TEST_DIR)/interface-fallback
endif
	NERO_LOG_FILE=:memory: $(TEST_DIR)/scan-session
	python3 tests/test_installation.py
	$(TEST_DIR)/parser-boundaries
	$(TEST_DIR)/workload-profile
	NERO_LOG_FILE=:memory: $(TEST_DIR)/syn-settings
	$(TEST_DIR)/diagnostic-process
	python3 tests/test_private_state.py $(TEST_DIR)/nero-safe
	python3 tests/test_workflow_security.py
	python3 tests/test_history.py $(TEST_DIR)/nero-safe
	python3 tests/test_logging.py $(TEST_DIR)/nero-safe $(TEST_DIR)/logging-fixture
	NERO_LOG_FILE=:memory: $(TEST_DIR)/portscanner-reliability
	NERO_LOG_FILE=:memory: $(TEST_DIR)/scanner-runtime
	NERO_LOG_FILE=:memory: $(TEST_DIR)/live-select-pty
	NERO_LOG_FILE=:memory: python3 tests/test_safe.py $(TEST_DIR)
	NERO_LOG_FILE=:memory: python3 tests/test_terminal_quality.py $(TEST_DIR)/nero-mock
	NERO_LOG_FILE=:memory: python3 tests/test_operation_layout.py $(TEST_DIR)/operation-layout

test-sanitize:
	$(MAKE) test TEST_DIR=$(BUILD_DIR)/sanitize SANITIZERS="-fsanitize=address,undefined -fno-omit-frame-pointer"

test-tsan:
	$(MAKE) $(BUILD_DIR)/tsan/scan-session TEST_DIR=$(BUILD_DIR)/tsan SANITIZERS="-fsanitize=thread -fno-omit-frame-pointer"
	NERO_LOG_FILE=:memory: $(BUILD_DIR)/tsan/scan-session
	$(MAKE) $(BUILD_DIR)/tsan/portscanner-reliability TEST_DIR=$(BUILD_DIR)/tsan SANITIZERS="-fsanitize=thread -fno-omit-frame-pointer"
	NERO_LOG_FILE=:memory: $(BUILD_DIR)/tsan/portscanner-reliability
	$(MAKE) $(BUILD_DIR)/tsan/scanner-runtime TEST_DIR=$(BUILD_DIR)/tsan SANITIZERS="-fsanitize=thread -fno-omit-frame-pointer"
	NERO_LOG_FILE=:memory: $(BUILD_DIR)/tsan/scanner-runtime
	$(MAKE) $(BUILD_DIR)/tsan/live-select-pty TEST_DIR=$(BUILD_DIR)/tsan SANITIZERS="-fsanitize=thread -fno-omit-frame-pointer"
	NERO_LOG_FILE=:memory: $(BUILD_DIR)/tsan/live-select-pty
	$(MAKE) $(BUILD_DIR)/tsan/logging-fixture TEST_DIR=$(BUILD_DIR)/tsan SANITIZERS="-fsanitize=thread -fno-omit-frame-pointer"
	NERO_LOG_FILE=:memory: $(BUILD_DIR)/tsan/logging-fixture stress > /dev/null

analyze:
	@mkdir -p $(BUILD_DIR)/analysis
	@set -e; for source in $(SAFE_SOURCES) $(SCANNER_SOURCES) $(SYN_SOURCES) $(INTERFACE_SOURCE); do \
		clang --analyze $(SAFE_FLAGS) -DNERO_NETWORK_UNAVAILABLE -Xanalyzer -analyzer-werror $$source -o $(BUILD_DIR)/analysis/$$(basename $$source).plist; \
	done

help:
	@echo "make                 Build the platform-specific executable"
	@echo "sudo make install    Install nero to /usr/local/bin for all directories"
	@echo "make test            Run offline CLI regression tests"
	@echo "make test-sanitize   Run offline tests with ASan/UBSan"
	@echo "make analyze         Run Clang static analysis"
	@echo "make clean           Remove generated build files"
	@echo "Code map: docs/code-map.md"
