# uplpgsql — LLVM JIT-compiled PL/pgSQL extension (UPL driver)
#
# This Makefile uses PGXS (PostgreSQL Extension Building Infrastructure).
# Source: postgres.env must be sourced before building.
#
# An incremental "make install" is expected to ship what is in the tree.  See
# the dependency rules below the PGXS include for what it takes to make that
# true: PGXS on its own links the shared library against ../../common and
# ../../core without depending on them, and tracks no header dependencies, so
# editing shared code and running "make install" would quietly reinstall the
# previous build.

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
# -MMD -MP: record each object's real header dependencies (see the -include
# below).  PGXS only tracks them if PostgreSQL itself was configured with
# --enable-depend, which is not something an extension can rely on.
PG_CFLAGS += $(UPL_LLVM_CFLAGS) -MMD -MP
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

UPL_LIBS = $(CORE_DIR)/libupl_core.a $(COMMON_DIR)/libupl_common.a

#
# Recurse into core/ and common/ whenever the archives are asked for, and let
# their own Makefiles decide whether anything needs rebuilding.  FORCE rather
# than listing their sources here: duplicating that list is how it goes out of
# date.
#
# This has to be a rule on the archive files themselves, not an "all: libs"
# prerequisite.  Both are prerequisites of all, and PGXS declares its own
# first, so make is free to link the shared library before the recursion has
# rebuilt the archives it links in.
#
.PHONY: FORCE
FORCE:

$(CORE_DIR)/libupl_core.a: FORCE
	@$(MAKE) -C $(CORE_DIR)

$(COMMON_DIR)/libupl_common.a: FORCE
	@$(MAKE) -C $(COMMON_DIR)

#
# SHLIB_LINK pulls both archives into the shared library, but PGXS makes it
# depend only on $(OBJS).  Without this, changing shared code rebuilds the
# archive and leaves the .so alone, and "make install" ships the old build.
#
# Caveat for scripted builds: /usr/bin/make on macOS is GNU make 3.81, which
# compares timestamps at one-second granularity.  A rebuild finishing in the
# same second as the previous link cannot be seen as newer, so an edit-build
# cycle driven faster than that still needs a "make clean" to be trustworthy.
# Hand editing is never that fast; a test script can be.
#
$(shlib): $(UPL_LIBS)

# Header dependencies for this directory's own objects, recorded by -MMD.
-include $(OBJS:.o=.d)

clean: upl-clean
.PHONY: upl-clean
upl-clean:
	rm -f $(OBJS:.o=.d)
