# Professional Makefile for "nero" project

# Compiler and flags
CC           := gcc
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
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "[INFO] Linked $(TARGET) successfully"

# Compile .c sources to .o object files
%.o: %.c
	@echo "[CC] Compiling $<"
	$(CC) $(CFLAGS) -c $< -o $@

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
	rm -f $(OBJS) $(TARGET)

