# Include after defining TARGET (.elf), OBJECTS and the core's build flags,
# but before rules using OBJECTS. The core links TARGET with LDFLAGS and LIBS.
ifndef LIBOUNCE_MODULE_INCLUDED
LIBOUNCE_MODULE_INCLUDED := 1

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO must point to the Switch 2 SDK)
endif
ifneq ($(suffix $(TARGET)),.elf)
$(error Set TARGET to the core's .elf filename before including libounce-module.mk)
endif

LIBOUNCE_MODULE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
LIBOUNCE_MODULE_BUILD ?= libretro/libounce
LIBOUNCE_MODULE_NRO := $(TARGET:.elf=.nro)
LIBOUNCE_MODULE_OBJECTS := $(LIBOUNCE_MODULE_BUILD)/start.o $(LIBOUNCE_MODULE_BUILD)/runtime.o

include $(DEVKITPRO)/libounce/switch2_cpu.mk
CC := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
CXX := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++
LIBOUNCE_MODULE_FLAGS := $(SWITCH2_CPU) -mtp=soft -fPIC -D__SWITCH__ -D__SWITCH2__ -DSWITCH -DHAVE_LIBNX
LIBOUNCE_MODULE_FLAGS += -mtls-dialect=trad -ftls-model=global-dynamic -DLIBOUNCE_MODULE_TLS
LIBOUNCE_MODULE_FLAGS += -I$(LIBOUNCE_MODULE_DIR)/../../include
LIBOUNCE_MODULE_FLAGS += -I$(DEVKITPRO)/libounce/include -I$(DEVKITPRO)/portlibs/switch/include
CFLAGS += $(LIBOUNCE_MODULE_FLAGS)
CXXFLAGS += $(LIBOUNCE_MODULE_FLAGS) -fno-exceptions -fno-rtti -fuse-cxa-atexit
LDFLAGS += $(SWITCH2_CPU) -mtp=soft -nostdlib -shared -Wl,-Bsymbolic,--hash-style=sysv,-z,text
LDFLAGS += -Wl,-T,$(DEVKITPRO)/libounce/switch.ld
LDFLAGS += -Wl,-T,$(LIBOUNCE_MODULE_DIR)/module-tls.ld
LIBS += -lgcc
OBJECTS += $(LIBOUNCE_MODULE_OBJECTS)

ifeq ($(strip $(.DEFAULT_GOAL)),)
.DEFAULT_GOAL := all
endif

all: $(LIBOUNCE_MODULE_NRO)

$(LIBOUNCE_MODULE_NRO): $(TARGET)
	$(DEVKITPRO)/tools/bin/elf2nro $< $@

$(LIBOUNCE_MODULE_BUILD)/start.o: $(LIBOUNCE_MODULE_DIR)/start.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBOUNCE_MODULE_BUILD)/runtime.o: $(LIBOUNCE_MODULE_DIR)/runtime.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(LIBOUNCE_MODULE_OBJECTS) $(LIBOUNCE_MODULE_DIR)/libounce-module.mk $(LIBOUNCE_MODULE_DIR)/module-tls.ld $(DEVKITPRO)/libounce/switch.ld

.PHONY: libounce-module-clean

clean: libounce-module-clean

libounce-module-clean:
	$(RM) $(LIBOUNCE_MODULE_OBJECTS) $(LIBOUNCE_MODULE_NRO)
endif
