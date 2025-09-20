# ==== Project ====
APP       := autod

# Source files (core)
SRC_CORE  := autod.c parson.c civetweb.c
# GUI module (optional)
SRC_GUI   := autod_gui_sdl.c

# Build dir
BUILD     := build
OBJ_CORE  := $(addprefix $(BUILD)/,$(SRC_CORE:.c=.o))
OBJ_GUI   := $(addprefix $(BUILD)/,$(SRC_GUI:.c=.o))

# ==== Config toggles ====
# Default: no GUI. Enable with `make GUI=1`
GUI       ?= 0

# ==== Toolchain (override when cross-compiling later) ====
CC        ?= gcc
PKGCONFIG ?= pkg-config

# ==== Flags ====
CPPFLAGS  += -D_POSIX_C_SOURCE=200809L -DNO_SSL -DNO_CGI -DNO_FILES
CFLAGS    ?= -Os -std=c11 -Wall -Wextra
LDFLAGS   += -pthread

# Auto depgen
CFLAGS    += -MMD -MP

# When GUI is enabled, add SDL2/TTF flags and defines
ifeq ($(GUI),1)
CPPFLAGS  += -DUSE_SDL2_GUI -DUSE_SDL2_TTF
CFLAGS    += $(shell $(PKGCONFIG) --cflags sdl2 SDL2_ttf 2>/dev/null)
LDLIBS    += $(shell $(PKGCONFIG) --libs sdl2 SDL2_ttf 2>/dev/null)
OBJ       := $(OBJ_CORE) $(OBJ_GUI)
else
OBJ       := $(OBJ_CORE)
endif

# Per-file flag tweak: CivetWeb noisy unused local 'uri_len' in some revs
# Only for civetweb.c object, tack on -Wno-unused-variable
CFLAGS_CIVETWEB := $(CFLAGS) -Wno-unused-variable

# ==== Targets ====
.PHONY: all gui nogui clean run strip install help

all: nogui

gui:
	@$(MAKE) GUI=1 $(APP)

nogui:
	@$(MAKE) GUI=0 $(APP)

$(APP): $(OBJ)
	$(CC) $(OBJ) $(LDFLAGS) $(LDLIBS) -o $@

# Core objects (generic rule)
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# CivetWeb object with adjusted flags
$(BUILD)/civetweb.o: civetweb.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS_CIVETWEB) -c $< -o $@

# Ensure build dir exists
$(BUILD):
	mkdir -p $(BUILD)

# Dev helpers
run: $(APP)
	./$(APP) --gui

strip: $(APP)
	strip $(APP)

install: $(APP)
	install -Dm755 $(APP) /usr/local/bin/$(APP)

clean:
	rm -rf $(BUILD) $(APP)

help:
	@echo "Targets:"
	@echo "  make            # build (no GUI)"
	@echo "  make gui        # build with SDL2 GUI"
	@echo "  make nogui      # build without GUI"
	@echo "  make run        # run with --gui (if built with GUI)"
	@echo "  make strip      # strip binary"
	@echo "  make install    # install to /usr/local/bin"
	@echo "  make clean      # remove build outputs"

# Include auto-generated dependency files
-include $(OBJ:.o=.d)
