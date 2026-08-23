# Build the core as a shared library for the Python driver.
#
#   make            build for this platform
#   make clean      remove build output
#
# Windows users: run build.bat instead (MSVC), or use this from MSYS2/MinGW.
#
# The same petkit_core.c also compiles into the ESP32 firmware unchanged. It is
# freestanding C99: no malloc, no files, no sockets, no time().

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -fPIC
SRC      = petkit_core.c

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  TARGET  = libpetkit_core.dylib
  LDFLAGS = -dynamiclib
else
  TARGET  = libpetkit_core.so
  LDFLAGS = -shared
endif

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC) petkit_core.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC)
	@echo "[+] core/$(TARGET) built."

# Convenience: build, then run the suite from the repository root.
test: $(TARGET)
	cd .. && python3 test_core.py

clean:
	rm -f libpetkit_core.so libpetkit_core.dylib petkit_core.dll \
	      petkit_core.obj petkit_core.exp petkit_core.lib *.o
