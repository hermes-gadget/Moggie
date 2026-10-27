# sonos_common.mk — reconstruction
#
# linux-2.6.35/Makefile does:  UPTO_ROOT := $(srctree)/../.. ; include $(UPTO_ROOT)/sonos_common.mk
#
# Sonos did not publish this file in their GPL drop (the drop ships the kernel
# source and the drivers, but not the build glue). This is a minimal
# reconstruction providing only what the kernel Makefile actually consumes.
#
# If a build fails on a missing variable, add it here rather than patching the
# kernel Makefile — that keeps the vendor tree pristine and the delta visible.

SONOS_PLATFORMS_INCLUDES := -I$(srctree)/../platforms/includes

# Sonos's original build stamped a platform identifier into the kernel. We do
# not know its exact format, so leave it empty rather than invent one.
SONOS_PLATFORM ?=
