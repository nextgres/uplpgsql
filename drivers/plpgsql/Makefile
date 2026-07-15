# uplpgsql — LLVM JIT-compiled PL/pgSQL extension (UPL driver)
#
# This Makefile uses PGXS (PostgreSQL Extension Building Infrastructure).
# Source: postgres.env must be sourced before building.
#
# IMPORTANT: PGXS does not track header dependencies.  After modifying
# headers in common/ or core/, always run "make clean && make install".

CORE_DIR = ../../core
COMMON_DIR = ../../common

MODULE_big = uplpgsql
EXTENSION = uplpgsql
DATA = uplpgsql--1.0.sql

# Regression tests — sql/ and expected/ are in ../../test/plpgsql/
REGRESS = plpgsql_array plpgsql_cache plpgsql_call plpgsql_control \
	plpgsql_copy plpgsql_coverage plpgsql_domain plpgsql_jit plpgsql_misc \
	plpgsql_record plpgsql_simple plpgsql_transaction \
	plpgsql_trap plpgsql_trigger plpgsql_varprops
REGRESS_OPTS = --load-extension=uplpgsql \
	--inputdir=../../test/plpgsql \
	--outputdir=../../test/plpgsql

# Driver-specific sources only (grammar, scanner, handler).
# Shared code is in libupl_common.a and libupl_core.a.
OBJS = uplpgsql_handler.o upl_scanner.o upl_gram.o

# LLVM configuration
# Prefer an llvm-config on PATH (Linux, and macOS with LLVM on PATH); fall
# back to the Homebrew location.  Override with UPL_LLVM_CONFIG=... if needed.
UPL_LLVM_CONFIG ?= $(shell command -v llvm-config 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-config)

UPL_LLVM_CFLAGS := $(shell $(UPL_LLVM_CONFIG) --cflags)
UPL_LLVM_LDFLAGS := $(shell $(UPL_LLVM_CONFIG) --ldflags)
UPL_LLVM_LIBS := $(shell $(UPL_LLVM_CONFIG) --libs core analysis orcjit native)
UPL_LLVM_SYSLIBS := $(shell $(UPL_LLVM_CONFIG) --system-libs)

PG_CPPFLAGS += $(UPL_LLVM_CFLAGS) -I$(CORE_DIR) -I$(COMMON_DIR)
PG_CFLAGS += $(UPL_LLVM_CFLAGS)
# The RT helper symbols in libupl_common.a are only reached from JIT'd code
# (never referenced from C), so the linker would otherwise drop them.  Force the
# whole common archive in.  The spelling differs by linker: ld64 (macOS) takes
# -force_load, GNU ld and lld (ELF) take --whole-archive.
ifeq ($(shell uname -s),Darwin)
UPL_FORCE_COMMON := -Wl,-force_load,$(COMMON_DIR)/libupl_common.a
else
UPL_FORCE_COMMON := -Wl,--whole-archive $(COMMON_DIR)/libupl_common.a -Wl,--no-whole-archive
endif

SHLIB_LINK += $(UPL_FORCE_COMMON) $(CORE_DIR)/libupl_core.a \
	$(UPL_LLVM_LDFLAGS) $(UPL_LLVM_LIBS) $(UPL_LLVM_SYSLIBS)

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Ensure libraries are built before the driver
all: libs

.PHONY: libs
libs:
	$(MAKE) -C $(CORE_DIR)
	$(MAKE) -C $(COMMON_DIR)
