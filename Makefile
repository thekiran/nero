# Nero: production build, offline tests and analysis

# Compiler and flags
CC           ?= cc
CFLAGS       := -Wall -Wextra -Werror -pedantic -std=c11 \
                 -D_POSIX_C_SOURCE=199309L \
                 -g \
                 -I include -I third_party/cjson \
                 -I /usr/include/ldns \
                 -DUNICODE_SUPPORT
LDFLAGS      := \
                 -lcurl \
                 -lcjson \
                 -lldns \
                 -pthread \
                 -lncurses

# Source groups
CLI_SUPPORT_SOURCES := \
    src/cli/parser/parser.c \
    src/cli/input.c \
    src/cli/render.c \
    src/cli/operation_ui.c \
    src/cli/commands.c \
    src/cli/ascii.c
CLI_SOURCES := src/cli/terminal.c $(CLI_SUPPORT_SOURCES)
CLI_HEADERS := $(wildcard include/nero/*.h src/cli/*.h)

# Production sources
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SRCS := src/main.c third_party/cjson/cJSON.c $(CLI_SOURCES) \
        src/platform/macos/select_interface_macos.c \
        src/modules/portscanner.c
CFLAGS := -Wall -Wextra -Werror -pedantic -std=c11 \
          -g -I include -I third_party/cjson -DUNICODE_SUPPORT
LDFLAGS :=
MACOS_FRAMEWORKS := -framework CoreFoundation -framework SystemConfiguration
LDLIBS += -pthread $(MACOS_FRAMEWORKS)
else ifeq ($(UNAME_S),Linux)
SRCS := src/main.c src/modules/syn_flood_powered.c \
        third_party/cjson/cJSON.c $(CLI_SOURCES) \
        src/platform/linux/select_interface.c \
        src/modules/portscanner.c
else
$(error Unsupported platform: $(UNAME_S))
endif

BUILD_DIR    := .build
OBJS         := $(addprefix $(BUILD_DIR)/obj/,$(SRCS:.c=.o))

TARGET       := nero
INSTALL_DIR  := /usr/local/bin

# Phony targets
.PHONY: all clean install run help
.DEFAULT_GOAL := all

# Default target: build the executable
all: $(TARGET)
	@echo "[INFO] Built $(TARGET)"

# Link object files into the final binary
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "[INFO] Linked $(TARGET) successfully"

# Compile .c sources to .o object files
$(BUILD_DIR)/obj/%.o: %.c Makefile
	@mkdir -p $(dir $@)
	@echo "[CC] Compiling $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Run the binary. The development CLI is interactive; it does not process a
# positional DOMAIN argument (legacy `make run DOMAIN=...` was removed).
run: $(TARGET)
	@echo "[RUN] Launching interactive CLI"
	./$(TARGET)

# Install the binary system-wide
install: $(TARGET)
	@echo "[INSTALL] Installing $(TARGET) to $(INSTALL_DIR)"
	sudo install -m 755 $(TARGET) $(INSTALL_DIR)/$(TARGET)

# Clean build artifacts
clean:
	@echo "[CLEAN] Removing object files and $(TARGET)"
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
	rm -rf .build


-include $(OBJS:.o=.d)

# Tests disable network operations or use mocks, including on Linux.
SAFE_FLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -g -Iinclude -DUNICODE_SUPPORT -DNERO_SAFE_TEST
ifeq ($(UNAME_S),Linux)
SAFE_FLAGS += -D_POSIX_C_SOURCE=200809L
INTERFACE_SOURCE = src/platform/linux/select_interface.c
else
INTERFACE_SOURCE = src/platform/macos/select_interface_macos.c
endif
SAFE_SOURCES = src/main.c $(CLI_SOURCES)
MOCK_SOURCES = src/main.c $(CLI_SOURCES) tests/mocks/network_handler.c
FAULT_SOURCES = src/main.c tests/mocks/terminal_faults.c $(CLI_SUPPORT_SOURCES) tests/mocks/network_handler.c
TEST_DIR ?= .build/tests
SANITIZERS ?=
.PHONY: test test-sanitize analyze
$(TEST_DIR):
	mkdir -p $@

$(TEST_DIR)/nero-safe: $(SAFE_SOURCES) $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) -DNERO_NETWORK_UNAVAILABLE $(SANITIZERS) $(SAFE_SOURCES) -o $@ -pthread -lm

$(TEST_DIR)/interfaces: tests/interfaces.c $(INTERFACE_SOURCE) include/nero/select_interface.h Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/interfaces.c $(INTERFACE_SOURCE) -o $@ $(MACOS_FRAMEWORKS)

$(TEST_DIR)/nero-mock: $(MOCK_SOURCES) $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) $(MOCK_SOURCES) -o $@ -pthread

$(TEST_DIR)/nero-faults: $(FAULT_SOURCES) src/cli/terminal.c $(CLI_HEADERS) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) $(FAULT_SOURCES) -o $@ -pthread

test: $(TEST_DIR)/nero-safe $(TEST_DIR)/interfaces $(TEST_DIR)/nero-mock $(TEST_DIR)/nero-faults
	python3 tests/test_safe.py $(TEST_DIR)
	python3 tests/test_terminal_quality.py $(TEST_DIR)/nero-mock

test-sanitize:
	$(MAKE) test TEST_DIR=.build/sanitize SANITIZERS="-fsanitize=address,undefined -fno-omit-frame-pointer"

analyze:
	@mkdir -p .build/analysis
	@set -e; for source in $(SAFE_SOURCES) $(filter src/platform/macos/%,$(INTERFACE_SOURCE)); do \
		clang --analyze $(SAFE_FLAGS) -DNERO_NETWORK_UNAVAILABLE -Xanalyzer -analyzer-werror $$source -o .build/analysis/$$(basename $$source).plist; \
	done

ifeq ($(UNAME_S),Darwin)
$(TEST_DIR)/interfaces-mock: tests/interfaces_mock_macos.c src/platform/macos/select_interface_macos.c Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) $< -o $@ $(MACOS_FRAMEWORKS)
test: $(TEST_DIR)/interfaces-mock
endif

# Discoverable entry point for local development.
help:
	@echo "make                 Build the platform-specific executable"
	@echo "make test            Run offline CLI regression tests"
	@echo "make test-sanitize   Run offline tests with ASan/UBSan"
	@echo "make analyze         Run Clang static analysis"
	@echo "make clean           Remove generated build files"
	@echo "Code map: docs/code-map.md"
