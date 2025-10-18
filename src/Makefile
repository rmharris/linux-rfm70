obj-m += rfm70.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

-include version.mk
DRIVER_VERSION ?= $(shell git describe --tags --dirty --always 2>/dev/null || echo unknown)
EXTRA_CFLAGS += -DDRIVER_VERSION=\\\"\"$(DRIVER_VERSION)\"\\\"

all:
	make -C $(KERNELDIR) EXTRA_CFLAGS="$(EXTRA_CFLAGS)" M=$(PWD) modules

clean:
	make -C $(KERNELDIR) M=$(PWD) clean