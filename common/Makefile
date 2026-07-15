# UPL Common — shared AST, interpreter, JIT compilation, and runtime
#
# Builds libupl_common.a, a static library linked into each language driver.
# Source: postgres.env must be sourced before building.

PG_CONFIG ?= pg_config
PG_INCLUDEDIR_SERVER := $(shell $(PG_CONFIG) --includedir-server)

# Prefer an llvm-config on PATH (Linux, and macOS with LLVM on PATH); fall
# back to the Homebrew location.  Override with UPL_LLVM_CONFIG=... if needed.
UPL_LLVM_CONFIG ?= $(shell command -v llvm-config 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-config)
UPL_LLVM_CFLAGS := $(shell $(UPL_LLVM_CONFIG) --cflags)

CORE_DIR = ../core

CC ?= cc
# -fPIC: these objects are archived into a static lib that is linked into the
# driver's shared object; on ELF platforms that requires position-independent
# code.  (No-op on macOS, where all code is already PIC.)
CFLAGS = -Wall -Wmissing-prototypes -Wpointer-arith \
	-Wdeclaration-after-statement -Werror=vla \
	-Wendif-labels -Wmissing-format-attribute \
	-Wformat-security -fno-strict-aliasing -fwrapv \
	-fvisibility=hidden -fPIC -O2 \
	-I$(PG_INCLUDEDIR_SERVER) \
	-I$(CORE_DIR) \
	$(UPL_LLVM_CFLAGS)

OBJS = upl_comp.o upl_exec.o upl_funcs.o \
       upl_compile_stmts.o upl_compile_expr.o upl_runtime.o

LIB = libupl_common.a

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJS)
	ar rcs $@ $^

%.o: %.c upl_common.h upl_plpgsql.h upl_errcodes.h $(CORE_DIR)/upl.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIB)
