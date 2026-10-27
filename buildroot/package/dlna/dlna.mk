################################################################################
#
# dlna -- Moggie UPnP AV MediaRenderer
#
################################################################################

DLNA_VERSION = 0.1.0
DLNA_SITE = $(BR2_EXTERNAL_MOGGIE_PATH)/../firmware/dlna
DLNA_SITE_METHOD = local
DLNA_LICENSE = GPL-2.0
DLNA_LICENSE_FILES =

# The renderer's own Makefile only honours CROSS_COMPILE from the command line,
# deliberately, so that a stray environment variable cannot turn a host test run
# into a silently-cross-compiled binary. Buildroot therefore passes it
# explicitly, exactly as it does for sonosd.
DLNA_CFLAGS = $(TARGET_CFLAGS) -std=c99 -Wall -Wextra -Wno-unused-parameter

define DLNA_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		CFLAGS="$(DLNA_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define DLNA_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/dlna-renderer $(TARGET_DIR)/usr/sbin/dlna-renderer
endef

$(eval $(generic-package))
