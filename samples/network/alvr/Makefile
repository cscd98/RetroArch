CC ?= cc
RA := ../../..
TARGET := alvr_protocol_test$(EXE)
SOURCES := protocol_test.c $(RA)/network/alvr_protocol.c
CFLAGS += -D_POSIX_C_SOURCE=200809L -std=c89 -O1 -g -Wall -Wextra -Werror \
          -Wdeclaration-after-statement -I$(RA)/libretro-common/include
ifneq ($(SANITIZER),)
CFLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZER)
endif

CONNECTION := alvr_connection_test$(EXE)
CONNECTION_SOURCES := connection_test.c $(RA)/network/alvr_connection.c \
                     $(RA)/network/alvr_protocol.c \
                     $(RA)/libretro-common/net/net_socket.c \
                     $(RA)/libretro-common/net/net_compat.c \
                     $(RA)/libretro-common/features/features_cpu.c \
                     $(RA)/libretro-common/time/rtime.c
ifneq ($(findstring mingw,$(CC)),)
LDFLAGS += -lws2_32
endif

all: $(TARGET) $(CONNECTION)

$(TARGET): $(SOURCES) $(RA)/network/alvr_protocol.h
	$(CC) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

$(CONNECTION): $(CONNECTION_SOURCES) $(RA)/network/alvr_connection.h
	$(CC) $(CFLAGS) -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-cast-function-type -ffunction-sections -fdata-sections \
		$(CONNECTION_SOURCES) -Wl,--gc-sections -o $@ $(LDFLAGS)

check: all
	$(RUNNER) ./$(TARGET)
	$(RUNNER) ./$(CONNECTION)

clean:
	rm -f alvr_protocol_test alvr_protocol_test.exe alvr_connection_test alvr_connection_test.exe

.PHONY: all check clean
