# firmware/dlna/Makefile
#
#   make                  host build                    -> ./dlna-renderer
#   make test             host build + protocol tests (tests/dlna)
#   make clean            remove build/ and the binary
#
# Cross build for the Play:5 (PowerPC MPC8315, e300c3):
#   PATH=/home/ben/Moggie/toolchain/powerpc-e300c3--glibc--stable-2026.08-1/bin:$PATH \
#   make CROSS_COMPILE=powerpc-buildroot-linux-gnu- CFLAGS="-Os -g"
#
# As in firmware/sonosd, an ambient CROSS_COMPILE in the environment is ignored;
# only the command-line form is honoured. That keeps `make test` on the host
# from silently producing a PowerPC binary.

SHELL := /bin/sh

ifeq ($(origin CROSS_COMPILE),command line)
TOOLCHAIN_PREFIX := $(CROSS_COMPILE)
BUILD_TAG        := cross
else
TOOLCHAIN_PREFIX :=
BUILD_TAG        := host
endif

ifeq ($(origin CC),command line)
# honour an explicit compiler
else
CC := $(TOOLCHAIN_PREFIX)gcc
endif
STRIP ?= $(TOOLCHAIN_PREFIX)strip

OPT     ?= -O2 -g
CFLAGS  += $(OPT) -std=c99 -Wall -Wextra -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wpointer-arith -Wno-unused-parameter \
           -MMD -MP
CPPFLAGS += -I.
LDLIBS   +=

BUILD := build/$(BUILD_TAG)
OBJ   := $(BUILD)/obj
BIN   := $(BUILD)/dlna-renderer

SRC := main.c util.c log.c config.c backend.c renderer.c scpd.c \
       description.c event.c soap.c http.c ssdp.c
OBJ_FILES := $(SRC:%.c=$(OBJ)/%.o)
DEPS      := $(OBJ_FILES:.o=.d)

.PHONY: all test clean
.DELETE_ON_ERROR:

all: $(BIN)

-include $(DEPS)

$(OBJ):
	@mkdir -p $@

$(OBJ)/%.o: %.c | $(OBJ)
	@echo "  CC     $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJ_FILES)
	@echo "  LD     $@ ($(BUILD_TAG))"
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ_FILES) $(LDLIBS)
	@echo "  NOTE   compiler $(CC)"

test: $(BIN)
	@python3 ../../tests/dlna/test_renderer.py --binary $(BIN)

clean:
	rm -rf build dlna-renderer
