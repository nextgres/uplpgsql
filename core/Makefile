# UPL Core Engine — language-agnostic LLVM JIT infrastructure
#
# Builds libupl_core.a, a static library linked into each language driver.
# Source: postgres.env must be sourced before building.

PG_CONFIG ?= pg_config
PG_INCLUDEDIR_SERVER := $(shell $(PG_CONFIG) --includedir-server)

# Prefer an llvm-config on PATH (Linux, and macOS with LLVM on PATH); fall
# back to the Homebrew location.  Override with UPL_LLVM_CONFIG=... if needed.
UPL_LLVM_CONFIG ?= $(shell command -v llvm-config 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-config)
UPL_LLVM_CFLAGS := $(shell $(UPL_LLVM_CONFIG) --cflags)

CC ?= cc

# -fPIC: these objects are archived into a static library that is linked
# into the driver's shared object; ELF platforms require
# position-independent code for that.  A no-op on macOS.
CFLAGS = -Wall -Wmissing-prototypes -Wpointer-arith \
	-Wdeclaration-after-statement -Werror=vla \
	-Wendif-labels -Wmissing-format-attribute \
	-Wformat-security -fno-strict-aliasing -fwrapv \
	-fvisibility=hidden -fPIC -O2 \
	-I$(PG_INCLUDEDIR_SERVER) \
	$(UPL_LLVM_CFLAGS)

OBJS = upl_llvm.o upl_cache.o upl_compile.o upl_datum.o

LIB = libupl_core.a

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

%.o: %.c upl.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB)
