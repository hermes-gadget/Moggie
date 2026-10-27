.PHONY: all check toolchain sources kernel rootfs ota-test clean

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

ota-test:
	@./tests/ota/run.sh

clean:
	rm -rf $(OUT)
