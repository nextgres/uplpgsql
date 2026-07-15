# UPL — Universal Procedural Language JIT Compiler
#
# Top-level Makefile. Builds core and common shared libraries, then
# selected language drivers.
# Source: postgres.env must be sourced before building.

DRIVERS ?= plpgsql

.PHONY: all install installcheck clean core common

core:
	$(MAKE) -C core

common: core
	$(MAKE) -C common

all: common
	@for d in $(DRIVERS); do $(MAKE) -C drivers/$$d; done

install: common
	@for d in $(DRIVERS); do $(MAKE) -C drivers/$$d install; done

installcheck:
	@for d in $(DRIVERS); do $(MAKE) -C drivers/$$d installcheck PGPORT=$(PGPORT); done

clean:
	$(MAKE) -C core clean
	$(MAKE) -C common clean
	@for d in $(DRIVERS); do $(MAKE) -C drivers/$$d clean; done
