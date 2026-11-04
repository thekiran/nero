# Nero - platform builds, offline tests, and static analysis

CC ?= cc

COMMON_CFLAGS := -Wall -Wextra -Werror -pedantic -std=c11 \
                 -g -Iinclude -Ithird_party/cjson -DUNICODE_SUPPORT

CFLAGS := $(COMMON_CFLAGS)
LDFLAGS ?=
LDLIBS  += -pthread

UNAME_S := $(shell uname -s)

CLI_SUPPORT_SOURCES := \
    src/cli/parser/parser.c \
    src/cli/input.c \
    src/cli/render.c \
    src/cli/operation_ui.c \
    src/cli/commands.c \
    src/cli/ascii.c

CLI_SOURCES := src/cli/terminal.c $(CLI_SUPPORT_SOURCES)
CLI_HEADERS := $(wildcard include/nero/*.h src/cli/*.h)

COMMON_SOURCES := \
    src/main.c \
    third_party/cjson/cJSON.c \
    $(CLI_SOURCES) \
    src/modules/portscanner.c

ifeq ($(UNAME_S),Darwin)
    PLATFORM_SOURCES := src/platform/macos/select_interface_macos.c \
                        src/modules/syn_flood_powered.c
    MACOS_FRAMEWORKS := -framework CoreFoundation -framework SystemConfiguration
    LDLIBS += $(MACOS_FRAMEWORKS)
else ifeq ($(UNAME_S),Linux)
    PLATFORM_SOURCES := src/platform/linux/select_interface.c
    CFLAGS += -D_POSIX_C_SOURCE=199309L -I/usr/include/ldns
    LDLIBS += -lcurl -lcjson -lldns -lncurses
else
    $(error Unsupported platform: $(UNAME_S))
endif

SRCS := $(COMMON_SOURCES) $(PLATFORM_SOURCES)

BUILD_DIR := .build
OBJS := $(addprefix $(BUILD_DIR)/obj/,$(SRCS:.c=.o))
DEPS := $(OBJS:.o=.d)

TARGET := nero
INSTALL_DIR ?= /usr/local/bin

.PHONY: all clean install run help test test-sanitize analyze
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
	sudo install -m 755 $(TARGET) $(INSTALL_DIR)/$(TARGET)

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
FAULT_SOURCES := src/main.c tests/mocks/terminal_faults.c \
                 $(CLI_SUPPORT_SOURCES) tests/mocks/network_handler.c

TEST_DIR ?= $(BUILD_DIR)/tests