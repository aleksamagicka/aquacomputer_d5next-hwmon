 KDIR ?= /lib/modules/`uname -r`/build

modules modules_install clean:
	make -C $(KDIR) M=$$PWD $@

checkpatch:
	$(KDIR)/scripts/checkpatch.pl --strict --no-tree $(SOURCES)
