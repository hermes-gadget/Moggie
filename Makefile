.PHONY: all check toolchain sources kernel rootfs ota ota-test ha-payloads ha-test ha-rig mqtt-lib mqtt-test emulated-test compat-probe clean

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

# Runs the cross-built PowerPC binaries under qemu-user. This is the only check
# that proves they execute on the target ABI rather than merely linking.
emulated-test:
	@./tests/emulated/run.sh

# Exercises heap, fs, threads, sockets, timing, entropy, mmap and signals on the
# target ABI. This is the probe that decides whether a toolchain's libc can run
# on the device's 2.6.35 kernel — run it on hardware day before anything else.
compat-probe:
	@./tests/compat/run.sh

clean:
	rm -rf $(OUT)
	@$(MAKE) -C firmware/updater clean
