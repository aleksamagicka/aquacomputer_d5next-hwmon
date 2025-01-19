.PHONY: all modules install modules_install clean

# external KDIR specification is supported
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD ?= $(shell pwd)

SOURCES := aquacomputer_d5next.c docs/aquacomputer_d5next.rst

.PHONY: all modules modules clean checkpatch dev

all: modules

install: modules_install

modules modules_install clean:
	$(MAKE) -C $(KDIR) M=$(PWD) $@

checkpatch:
	$(KDIR)/scripts/checkpatch.pl --strict --no-tree --ignore LINUX_VERSION_CODE $(SOURCES)

dev:
	$(MAKE) clean
	$(MAKE)
	sudo rmmod aquacomputer_d5next || true
	sudo insmod aquacomputer_d5next.ko
