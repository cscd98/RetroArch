include $(DEVKITPRO)/libounce/switch2_cpu.mk
CC := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc
CFLAGS := $(SWITCH2_CPU) -mtp=soft -fPIC -O2 -g -std=gnu11 -Wall -Wextra -Werror -D__SWITCH__ -D__SWITCH2__ -I$(DEVKITPRO)/libounce/include
LDFLAGS := -specs=$(DEVKITPRO)/libounce/switch.specs $(SWITCH2_CPU) -mtp=soft -fPIC -L$(DEVKITPRO)/libounce/lib
all: fastmem_probe.nro
fastmem_probe.elf: main.o exception.o
	$(CC) $^ $(LDFLAGS) -lnx -o $@
fastmem_probe.nro: fastmem_probe.elf
	$(DEVKITPRO)/tools/bin/elf2nro $< $@
main.o: main.c
	$(CC) $(CFLAGS) -c $< -o $@
exception.o: exception.S
	$(CC) $(CFLAGS) -c $< -o $@
.PHONY: all clean
clean:
	$(RM) main.o exception.o fastmem_probe.elf fastmem_probe.nro
