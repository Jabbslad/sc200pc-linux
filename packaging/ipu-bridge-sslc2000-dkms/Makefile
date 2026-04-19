KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	install -Dm644 ipu-bridge.ko \
	  $(DESTDIR)/lib/modules/$(shell uname -r)/updates/ipu-bridge.ko
	depmod -a $(shell uname -r)

.PHONY: all clean install
