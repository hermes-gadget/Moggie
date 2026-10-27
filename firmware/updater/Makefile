# moggie-updater
#
# Two binaries from one source:
#   $(BUILD)/moggie-updater       the firmware build: NAND only, key compiled in
#   $(BUILD)/moggie-updater-test  the x86 build: regular files, --pubkey-txt
#
# Nothing here writes to a real MTD device unless you point it at one without
# --test-mode, and the production build refuses regular files outright.

CC      ?= cc
BUILD   ?= build
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c99 -Wall -Wextra -Werror -pedantic \
           -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64

# Point this at a generated pubkey header to embed a real (or throwaway) key:
#   make PUBKEY_HEADER=/abs/path/pubkey.h
PUBKEY_HEADER ?=
ifneq ($(PUBKEY_HEADER),)
CFLAGS += -DMOGGIE_OTA_PUBKEY_HEADER='"$(PUBKEY_HEADER)"'
endif
LDFLAGS ?=

# make SANITIZE=1 BUILD=build-asan — used when auditing the manifest parser
ifneq ($(SANITIZE),)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
LDFLAGS += -fsanitize=address,undefined
endif

SRCS := updater.c manifest.c slotdev.c rsa.c sha256.c b64.c
HDRS := manifest.h slotdev.h rsa.h sha256.h b64.h pubkey_dev.h

OBJS      := $(SRCS:%.c=$(BUILD)/%.o)
TEST_OBJS := $(SRCS:%.c=$(BUILD)/test/%.o)

.PHONY: all test-build clean

all: $(BUILD)/moggie-updater test-build

test-build: $(BUILD)/moggie-updater-test

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/test:
	@mkdir -p $(BUILD)/test

$(BUILD)/%.o: %.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/test/%.o: %.c $(HDRS) | $(BUILD)/test
	$(CC) $(CFLAGS) -DMOGGIE_TEST_MODE -c $< -o $@

$(BUILD)/moggie-updater: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BUILD)/moggie-updater-test: $(TEST_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(BUILD)
