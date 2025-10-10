CS0 ?= rfm70

TARGET :=
CPPFLAGS = -nostdinc -undef -x assembler-with-cpp 

COMPATIBLE_rfm70 = "\"hoperf,rfm70\""
COMPATIBLE_nRF24L01 = "\"nordic,nRF24L01\""

ifeq ($(COMPATIBLE_$(CS0)),)
$(error CS0 must be rfm70 or nRF24L01)
else
TARGET := $(CS0)
CPPFLAGS += -DCOMPATIBLE_CS0=$(COMPATIBLE_$(CS0))
endif

ifneq ($(CS1),)
ifeq ($(COMPATIBLE_$(CS1)),)
$(error CS1 must be rfm70 or nRF24L01)
else
TARGET := $(TARGET)_$(CS1)
CPPFLAGS += -DCOMPATIBLE_CS1=$(COMPATIBLE_$(CS1))
endif
endif

overlay-$(TARGET).dtbo:	overlay.dts
	cpp $(CPPFLAGS) $< | dtc -@ -I dts -O dtb -o $@

clean:
	rm overlay-*.dtbo
