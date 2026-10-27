.PHONY: all check toolchain sources kernel rootfs ota-test ha-payloads ha-test ha-rig clean

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

# Home Assistant control plane — see docs/HA.md
ha-payloads:
	@python3 tools/ha/gen_discovery.py

ha-test:
	@python3 tests/ha/test_payloads.py

ha-rig:
	@cd tools/ha && docker compose up -d && ./sync-config.sh

clean:
	rm -rf $(OUT)
