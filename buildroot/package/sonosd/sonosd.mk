################################################################################
#
# sonosd -- Moggie audio daemon
#
################################################################################

SONOSD_VERSION = 0.1.0
SONOSD_SITE = $(BR2_EXTERNAL_MOGGIE_PATH)/../firmware/sonosd
SONOSD_SITE_METHOD = local
SONOSD_LICENSE = GPL-2.0
SONOSD_LICENSE_FILES =
SONOSD_DEPENDENCIES = mosquitto

# The daemon's own Makefile has an opt-in cross build and probes pkg-config
# for libmosquitto. Buildroot has both, so tell it explicitly instead of
# relying on the host's pkg-config finding the host's library.
SONOSD_CFLAGS = $(TARGET_CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter

define SONOSD_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(SONOSD_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)" \
		HAVE_MOSQ=1 \
		MOSQ_CFLAGS="-I$(STAGING_DIR)/usr/include" \
		MOSQ_LIBS="-lmosquitto" \
		LDLIBS="-lmosquitto -pthread"
endef

define SONOSD_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/sonosd $(TARGET_DIR)/usr/sbin/sonosd
endef

$(eval $(generic-package))
