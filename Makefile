 KDIR ?= /lib/modules/`uname -r`/build

modules modules_install clean:
	make -C $(KDIR) M=$$PWD $@