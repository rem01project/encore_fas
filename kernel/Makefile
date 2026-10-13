# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_ENCORE_FAS) += encore_fas.o
encore_fas-y := fas_lkm.o

ifeq ($(KERNELRELEASE),)

KDIR ?= /lib/modules/$(shell uname -r)/build
HOSTCC ?= cc

all: tools/check_symbol
	$(MAKE) -C $(KDIR) M=$(CURDIR) CONFIG_ENCORE_FAS=m modules
	@if [ -f $(KDIR)/vmlinux ]; then \
		tools/check_symbol encore_fas.ko $(KDIR)/vmlinux; \
	else \
		echo "-- no vmlinux in KDIR, skipping symbol check"; \
	fi

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f tools/check_symbol

tools/check_symbol: tools/check_symbol.c
	$(HOSTCC) $< -o $@

.PHONY: all clean

else

fas_srcdir := $(patsubst %/,%,$(dir $(realpath $(lastword $(MAKEFILE_LIST)))))
fas_git := git -C $(fas_srcdir)
fas_git_version := $(shell $(fas_git) rev-list --count HEAD 2>/dev/null)
fas_git_branch := $(shell $(fas_git) rev-parse --abbrev-ref HEAD 2>/dev/null)

ifneq ($(fas_git_version),)
ccflags-y += -DFAS_VERSION=$(fas_git_version)
ccflags-y += -DFAS_BRANCH=\"$(fas_git_branch)\"
endif

ccflags-y += -I$(fas_srcdir) -I$(fas_srcdir)/../src -I$(fas_srcdir)/../include

endif
