obj-m := ni6674t.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

ifeq ($(KERNELRELEASE),)

.DEFAULT_GOAL := modules

%:
	@echo Building PXIe-6674T driver against kernel @ $(KERNELDIR)
	@$(MAKE) --no-print-directory -C $(KERNELDIR) M=$(PWD) $@

.PHONY: firmware_install
firmware_install:
	@install -m 644 $(wildcard firmware/*.bin) $(INSTALL_MOD_PATH)/lib/firmware

modules_install : modules firmware_install

install : modules_install ;

endif
