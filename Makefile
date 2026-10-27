.PHONY: all check toolchain sources kernel rootfs ota ota-test ha-payloads ha-test ha-rig mqtt-lib mqtt-test clean

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

# Home Assistant control plane — see docs/HA.md
ha-payloads:
	@python3 tools/ha/gen_discovery.py

ha-test:
	@python3 tests/ha/test_payloads.py

ha-rig:
	@cd tools/ha && docker compose up -d && ./sync-config.sh

# The MQTT integration test needs a real libmosquitto on the host. There is no
# system-wide copy on the build box (and no root to install one), so it builds
# into dl/ instead.
mqtt-lib:
	@./scripts/build-mosquitto.sh host

mqtt-test:
	@MOSQ_PREFIX=$(CURDIR)/dl/mosq-host/usr ./tests/mqtt/run.sh

clean:
	rm -rf $(OUT)
	@$(MAKE) -C firmware/updater clean
