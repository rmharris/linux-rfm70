obj-m += rfm70.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)

all:
	make -C $(KERNELDIR) M=$(PWD) modules

clean:
	make -C $(KERNELDIR) M=$(PWD) clean
