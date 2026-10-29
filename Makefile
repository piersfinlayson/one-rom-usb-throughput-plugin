# Copyright (C) 2026 Piers Finlayson <piers@piers.rocks>
#
# MIT License
#
# Builds the One ROM USB throughput test user plugin.  At
# plugins/user/usb-throughput in a One ROM checkout, the build uses that
# checkout's firmware/ora.  Elsewhere it uses One ROM release ONEROM_REF, and the
# first build or make one-rom fetches ONEROM_REF release firmware/ora and ci
# directories into build/one-rom-<ONEROM_REF>.
#
# Usage:
#   make
#   make TOOLCHAIN=/opt/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi/bin
#   make DRIVE=2 SLEW=slow
#   make one-rom
#   make clean
#   make distclean
#
# DRIVE sets the data pins' drive strength in mA (2, 4, 8 or 12) and SLEW their
# slew rate (slow or fast) at start-up.  Without DRIVE the firmware's own
# setting is left in place.  A DRIVE build is written to
# build/drive-<DRIVE>-<SLEW>.

ONEROM_REF ?= v0.8.0
ONEROM_URL := https://github.com/piersfinlayson/one-rom
ONEROM_DIR := $(abspath build/one-rom-$(ONEROM_REF))

LOCAL_ORA := $(abspath ../../../firmware/ora)
FETCHED_ORA := $(ONEROM_DIR)/firmware/ora

ifneq ($(wildcard $(LOCAL_ORA)/plugin.mk),)
ORA := $(LOCAL_ORA)
else
ORA := $(FETCHED_ORA)
endif

# check_loop.py reads the relocations kept by --emit-relocs.  The RAM functions
# are loaded from flash, which puts a writable and executable segment in the
# ELF.
LINK_FLAGS := -Wl,--emit-relocs -Wl,--no-warn-rwx-segments
OUT_DIR := build

ifneq ($(DRIVE),)
SLEW ?= fast
DRIVE_FLAGS := -DDATA_DRIVE_MA=$(DRIVE) -DDATA_SLEW_FAST=$(if $(filter fast,$(SLEW)),1,0)
OUT_DIR := build/drive-$(DRIVE)-$(SLEW)
endif

.PHONY: all one-rom clean distclean

all: $(ORA)/plugin.mk
	@echo "Building against $(ORA)"
	@$(MAKE) -f $(ORA)/plugin.mk \
		SRC=plugin_main.c \
		ORA_INCLUDE=$(ORA) \
		PLUGIN_TYPE=USER \
		EXTRA_C_FLAGS="$(LINK_FLAGS) $(DRIVE_FLAGS)" \
		BUILD_DIR=$(OUT_DIR) \
		$(if $(TOOLCHAIN),TOOLCHAIN=$(TOOLCHAIN),)
	@python3 check_loop.py $(OUT_DIR)/plugin_user.elf

one-rom: $(FETCHED_ORA)/plugin.mk

$(FETCHED_ORA)/plugin.mk:
	@echo "Fetching One ROM $(ONEROM_REF) plugin API..."
	@rm -rf $(ONEROM_DIR) $(ONEROM_DIR).partial
	@git init --quiet $(ONEROM_DIR).partial
	@git -C $(ONEROM_DIR).partial sparse-checkout set firmware/ora ci
	@git -C $(ONEROM_DIR).partial fetch --quiet --depth 1 --filter=blob:none \
		$(ONEROM_URL) refs/tags/$(ONEROM_REF)
	@git -C $(ONEROM_DIR).partial -c advice.detachedHead=false checkout \
		--quiet FETCH_HEAD
	@mv $(ONEROM_DIR).partial $(ONEROM_DIR)

clean:
	@rm -rf build

distclean: clean
