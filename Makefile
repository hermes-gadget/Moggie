.PHONY: all check toolchain sources kernel rootfs ota ota-test clean

SHELL := /bin/bash
OUT   := out

all: check kernel rootfs

check:
	@./scripts/check-deps.sh

toolchain:
	@./scripts/fetch-toolchain.sh

sources:
	@./scripts/fetch-sources.sh

kernel:
	@./scripts/build-kernel.sh

rootfs:
	@./scripts/build-rootfs.sh

# The OTA toolchain builds on the host and needs no cross compiler.
ota:
	@$(MAKE) -C firmware/updater

ota-test: ota
	@./tests/ota/run.sh

clean:
	rm -rf $(OUT)
	@$(MAKE) -C firmware/updater clean
