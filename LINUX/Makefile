# To build external modules, you must have a prebuilt kernel available
# that contains the configuration and header files used in the build.
# go in the kernel directory and do a
#       make oldconfig; make scripts; make prepare
#
# The list of targets is derived from obj-m
# and then the corresponding foo-objs

CONFIG_NETMAP:=m

netmap_lin-objs := netmap.o

obj-$(CONFIG_NETMAP) = netmap_lin.o
obj-m += $(DRIVERS)

# names of the driver sources. In old linuxes are under
# KSRC/drivers/net, but depending on where you build they
# can be in $(KSRC)/source/drivers/net/ethernet/$(manufacturer)
DRIVER_SRCS = $(KSRC)/arch/arm/plat-armada/mv_drivers_lsp/$(DRV_NAME)

# The following commands are needed to build the modules as out-of-tree, in
# fact the kernel sources path must be specified.

# Additional compile flags (e.g. header location)
M:=$(PWD)
EXTRA_CFLAGS := -I$(M) -I$(M)/../sys -I$(M)/../sys/dev -I$(M)/mv_neta -DCONFIG_NETMAP

# We use KSRC for the kernel configuration and sources.s
# If the sources are elsewhere, then use SRC to point to them.
KSRC ?= /lib/modules/$(shell uname -r)/build
SRC ?= $(KSRC)

include $(KSRC)/include/config/auto.conf
ifeq ($(CONFIG_CPU_BIG_ENDIAN), y)
EXTRA_CFLAGS += -DMV_CPU_BE
else
EXTRA_CFLAGS += -DMV_CPU_LE
endif

# extract version number and filter with the available patches.
LIN_VER = $(shell grep LINUX_VERSION_CODE $(KSRC)/include/linux/version.h | \
	awk '{printf "%03x%02d", $$3/256, $$3%256} ')

all:	build build_drv

build:
	make -C $(KSRC) M=$(PWD) CONFIG_NETMAP=m	\
		EXTRA_CFLAGS='$(EXTRA_CFLAGS)' \
		modules

build_drv:
	@cp ./Module.symvers $(DRIVER_SRCS)
	make -C $(KSRC) M=$(DRIVER_SRCS) NETMAP=y NETMAP_DIR=$(PWD) modules ; mv $(DRIVER_SRCS)/*.ko $(PWD);
	@ls -l `find . -name \*.ko`

test:
	@echo "ksrc $(KSRC)"

clean:
	(cd ../examples; make clean)
	@ make -C $(KSRC) M=$(PWD) clean
	@ make -C $(KSRC) M=$(DRIVER_SRCS) clean

# the source is not here so we need to specify a dependency
$(obj)/netmap.o: $(M)/../sys/dev/netmap/netmap.c
	$(call cmd,cc_o_c)
	$(call cmd,modversions)

apps:
	(cd ../examples; make KSRC=$(KSRC) pkt-gen; make KSRC=$(KSRC) bridge)
