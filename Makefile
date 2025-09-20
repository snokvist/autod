# ==== Project ====
APP          := autod

# Sources
SRC_CORE     := autod.c parson.c civetweb.c
SRC_GUI      := autod_gui_sdl.c

# Tool defaults (can be overridden from env)
CC_NATIVE    ?= gcc
CC_MUSL      ?= arm-linux-musleabihf-gcc
CC_GNU       ?= arm-linux-gnueabihf-gcc
PKGCONFIG    ?= pkg-config

# Common flags
CPPFLAGS_COM := -D_POSIX_C_SOURCE=200809L -DNO_SSL -DNO_CGI -DNO_FILES
CFLAGS_COM   ?= -Os -std=c11 -Wall -Wextra
LDFLAGS_COM  := -pthread
CFLAGS_COM   += -MMD -MP

# GUI helpers
GUI         ?= 0
GUI_DEFS     = -DUSE_SDL2_GUI -DUSE_SDL2_TTF
GUI_CFLAGS   = $(shell $(PKGCONFIG) --cflags sdl2 SDL2_ttf 2>/dev/null)
GUI_LDLIBS   = $(shell $(PKGCONFIG) --libs   sdl2 SDL2_ttf 2>/dev/null)

# ===== Helper macro to define a build "flavor" =====
# $(call DEFINE_FLAVOR,flavor_name,CC,build_subdir,output_binary_basename)
define DEFINE_FLAVOR

# Flavor variables
$1_CC        := $2
$1_BUILD     := build/$3
$1_OUTBASE   := $4
$1_CPPFLAGS  := $(CPPFLAGS_COM)
$1_CFLAGS    := $(CFLAGS_COM)
$1_LDFLAGS   := $(LDFLAGS_COM)
$1_LDLIBS    :=

# Files per flavor
$1_OBJ_CORE  := $(addprefix $$($1_BUILD)/,$(SRC_CORE:.c=.o))
$1_OBJ_GUI   := $(addprefix $$($1_BUILD)/,$(SRC_GUI:.c=.o))
$1_OBJ_NOGUI := $$($1_OBJ_CORE)
$1_OBJ_GUION := $$($1_OBJ_CORE) $$($1_OBJ_GUI)

# CivetWeb per-file tweak
$1_CFLAGS_CIVETWEB := $$($1_CFLAGS) -Wno-unused-variable

# Output names (with/without GUI)
$1_BIN_NOGUI := $$($1_OUTBASE)
$1_BIN_GUI   := $$($1_OUTBASE)-gui

# Phony targets for this flavor
.PHONY: $1 $1-gui

$1: $$($1_BIN_NOGUI)
$1-gui: GUI=1
$1-gui: $$($1_BIN_GUI)

# Link rules
$$($1_BIN_NOGUI): $$($1_OBJ_NOGUI)
	$$($1_CC) $$^ $$($1_LDFLAGS) $$($1_LDLIBS) -o $$@

$$($1_BIN_GUI): $$($1_OBJ_GUION)
	$$($1_CC) $$^ $$($1_LDFLAGS) $$($1_LDLIBS) $(GUI_LDLIBS) -o $$@

# Compile rules
$$($1_BUILD)/%.o: %.c | $$($1_BUILD)
	$$(if $$(filter 1,$$(GUI)), \
		$$($1_CC) $$($1_CPPFLAGS) $(GUI_DEFS) $(GUI_CFLAGS) $$($1_CFLAGS) -c $$< -o $$@, \
		$$($1_CC) $$($1_CPPFLAGS) $$($1_CFLAGS) -c $$< -o $$@)

# CivetWeb with extra flag
$$($1_BUILD)/civetweb.o: civetweb.c | $$($1_BUILD)
	$$(if $$(filter 1,$$(GUI)), \
		$$($1_CC) $$($1_CPPFLAGS) $(GUI_DEFS) $(GUI_CFLAGS) $$($1_CFLAGS_CIVETWEB) -c $$< -o $$@, \
		$$($1_CC) $$($1_CPPFLAGS) $$($1_CFLAGS_CIVETWEB) -c $$< -o $$@)

# Build dir
$$($1_BUILD):
	mkdir -p $$@

# Include deps
-include $$($1_OBJ_NOGUI:.o=.d)
-include $$($1_OBJ_GUION:.o=.d)

endef

# ===== Define the three flavors =====
# native -> ./autod / ./autod-gui
$(eval $(call DEFINE_FLAVOR,native,$(CC_NATIVE),native,$(APP)))
# musl   -> ./autod-musl / ./autod-musl-gui
$(eval $(call DEFINE_FLAVOR,musl,$(CC_MUSL),musl,$(APP)-musl))
# gnu    -> ./autod-gnu / ./autod-gnu-gui
$(eval $(call DEFINE_FLAVOR,gnu,$(CC_GNU),gnu,$(APP)-gnu))

# ===== Top-level convenience targets =====
.PHONY: all gui musl musl-gui gnu gnu-gui clean strip install help

all: native
gui: native-gui
musl: musl
musl-gui: musl-gui
gnu: gnu
gnu-gui: gnu-gui

clean:
	rm -rf build $(APP) $(APP)-gui $(APP)-musl $(APP)-musl-gui $(APP)-gnu $(APP)-gnu-gui

strip: native
	strip $(APP)

install: native
	install -Dm755 $(APP) /usr/local/bin/$(APP)

help:
	@echo "Builds:"
	@echo "  make           -> native (no GUI) => ./$(APP)"
	@echo "  make gui       -> native with GUI => ./$(APP)-gui"
	@echo "  make musl      -> arm-linux-musleabihf-gcc (no GUI) => ./$(APP)-musl"
	@echo "  make musl-gui  -> musl with GUI => ./$(APP)-musl-gui"
	@echo "  make gnu       -> arm-linux-gnueabihf-gcc (no GUI) => ./$(APP)-gnu"
	@echo "  make gnu-gui   -> gnu with GUI => ./$(APP)-gnu-gui"
	@echo ""
	@echo "Env overrides:"
	@echo "  CC_NATIVE=...  CC_MUSL=...  CC_GNU=...  PKGCONFIG=..."
