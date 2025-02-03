include ../../make.mk

# Untested: if for some reason CMake cannot be used, below is the native Makefile equivalent
# Credit: https://github.com/mwpenny/portal64-still-alive
# Use tag name if the current commit is tagged, otherwise use commit hash
# If not in a git repo, fall back to exported version
#PROJ_VERSION := $(shell \
#  (git describe --tags HEAD 2>/dev/null || cat version.txt) | \
#  awk -F '-' '{print (NF >= 3 ? substr($$3, 2) : $$1)}' \
#)
#
# Allow targets to depend on the PROJ_VERSION variable via a file.
# Update the file only when it differs from the variable (triggers rebuild).
#.PHONY: gameversion
#build/version.txt: gameversion
#ifneq ($(shell cat build/version.txt 2>/dev/null), $(PROJ_VERSION))
#  echo -n $(PROJ_VERSION) > build/version.txt
#endif


INC += \
	src \
	$(TOP)/hw \

# Example source
EXAMPLE_SOURCE += \
  src/main.c \
  src/msc_disk.c \
  src/usb_descriptors.c \

SRC_C += $(addprefix $(CURRENT_PATH)/, $(EXAMPLE_SOURCE))

include ../../rules.mk
