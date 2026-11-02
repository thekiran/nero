# Professional Makefile for "nero" project

# Compiler and flags
CC           ?= cc
CFLAGS       := -Wall -Wextra -Werror -pedantic -std=c11 \
                 -D_POSIX_C_SOURCE=199309L \
                 -g \
                 -I include \
                 -I /usr/include/ldns \
                 -DUNICODE_SUPPORT
LDFLAGS      := \
                 -lcurl \
                 -lcjson \
                 -lldns \
                 -pthread \
                 -lncurses 

# Project files
SRCS         := \
                 main.c \
                 syn_flood_powered.c \
                 cJSON.c \
                 ascii.c \
                 terminal.c \
                 select_interface.c \
                 portscanner.c

# Darwin builds only the development CLI and read-only interface adapter.
# Linux keeps its original source list and link dependencies.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SRCS := main.c cJSON.c ascii.c terminal.c platform/select_interface_macos.c platform/network_unavailable.c
CFLAGS := -Wall -Wextra -Werror -pedantic -std=c11 -g -I include -DUNICODE_SUPPORT
LDFLAGS :=
LDLIBS += -pthread
else ifneq ($(UNAME_S),Linux)
$(error Unsupported platform: $(UNAME_S))
endif

OBJS         := $(SRCS:.c=.o)
TARGET       := nero
INSTALL_DIR  := /usr/local/bin

# Phony targets
.PHONY: all clean install run run_default

# Default target: build the executable
all: $(TARGET)
	@echo "[INFO] Built $(TARGET)"

# Link object files into the final binary
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)
	@echo "[INFO] Linked $(TARGET) successfully"

# Compile .c sources to .o object files
%.o: %.c
	@echo "[CC] Compiling $<"
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

# Run the binary with a default domain
run_default: $(TARGET)
	@echo "[RUN] Executing with example.com"
	./$(TARGET) example.com

# Run with a custom domain: make run DOMAIN=target.com
run: $(TARGET)
	@if [ -z "$(DOMAIN)" ]; then \
		echo "[ERROR] DOMAIN variable not set. Usage: make run DOMAIN=example.com"; \
		exit 1; \
	fi; \
	@echo "[RUN] Executing with $(DOMAIN)"
	./$(TARGET) $(DOMAIN)

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

# Tests always use the no-network adapter, including on Linux.
SAFE_FLAGS = -Wall -Wextra -Werror -pedantic -std=c11 -g -Iinclude -DUNICODE_SUPPORT -DNERO_SAFE_TEST
ifeq ($(UNAME_S),Linux)
SAFE_FLAGS += -D_POSIX_C_SOURCE=200809L
INTERFACE_SOURCE = select_interface.c
else
INTERFACE_SOURCE = platform/select_interface_macos.c
endif
SAFE_SOURCES = main.c terminal.c ascii.c platform/network_unavailable.c
TEST_DIR ?= .build/tests
SANITIZERS ?=
.PHONY: test test-sanitize analyze
$(TEST_DIR):
	mkdir -p $@

$(TEST_DIR)/nero-safe: $(SAFE_SOURCES) $(wildcard include/*.h) Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) $(SAFE_SOURCES) -o $@ -pthread -lm

$(TEST_DIR)/interfaces: tests/interfaces.c $(INTERFACE_SOURCE) include/select_interface.h Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) tests/interfaces.c $(INTERFACE_SOURCE) -o $@

test: $(TEST_DIR)/nero-safe $(TEST_DIR)/interfaces
	python3 tests/test_safe.py $(TEST_DIR)

test-sanitize:
	$(MAKE) test TEST_DIR=.build/sanitize SANITIZERS="-fsanitize=address,undefined -fno-omit-frame-pointer"

analyze:
	@mkdir -p .build/analysis
	@set -e; for source in $(SAFE_SOURCES) $(filter platform/%,$(INTERFACE_SOURCE)); do \
		clang --analyze $(SAFE_FLAGS) -Xanalyzer -analyzer-werror $$source -o .build/analysis/$$(basename $$source).plist; \
	done

ifeq ($(UNAME_S),Darwin)
$(TEST_DIR)/interfaces-mock: tests/interfaces_mock_macos.c platform/select_interface_macos.c Makefile | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(SAFE_FLAGS) $(SANITIZERS) $< -o $@
test: $(TEST_DIR)/interfaces-mock
endif
