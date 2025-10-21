# ==== Project ====
APP          := autod
UDP_APP      := udp_relay

# Install prefixes (override as needed)
PREFIX       ?= /usr/local
BINDIR       ?= $(PREFIX)/bin
SYSCONFDIR   ?= /etc
DATADIR      ?= $(PREFIX)/share
AUTOD_DATADIR ?= $(DATADIR)/$(APP)
VRXDIR       ?= $(AUTOD_DATADIR)/vrx
SYSTEMD_DIR  ?= /etc/systemd/system
UDP_CONFDIR  ?= $(SYSCONFDIR)/$(UDP_APP)
UDP_HTMLDIR  ?= $(AUTOD_DATADIR)/udp_relay
UDP_UI_ASSET ?= vrx_udp_relay.html
UDP_UI_PATH  ?= $(UDP_HTMLDIR)/$(UDP_UI_ASSET)

# Paths
SRC_DIR      := src

# Sources (filenames only; rules add SRC_DIR/)
SRC_CORE     := autod.c scan.c parson.c civetweb.c
SRC_GUI      := autod_gui_sdl.c

# Tool defaults (can be overridden from env)
CC_NATIVE    ?= gcc
CC_MUSL      ?= arm-linux-musleabihf-gcc
CC_GNU       ?= arm-linux-gnueabihf-gcc
PKGCONFIG    ?= pkg-config

# Strip tools (override if your triplets differ)
STRIP_NATIVE ?= strip
STRIP_MUSL   ?= arm-linux-musleabihf-strip
STRIP_GNU    ?= arm-linux-gnueabihf-strip

# Common flags
CPPFLAGS_COM := -I$(SRC_DIR) -D_POSIX_C_SOURCE=200809L -DNO_SSL -DNO_CGI -DNO_FILES
CFLAGS_COM   ?= -Os -std=c11 -Wall -Wextra
LDFLAGS_COM  := -pthread
CFLAGS_COM   += -MMD -MP

# Tool-specific flags
# sse_tail: gcc -O2 -std=c11 -Wall -Wextra
CFLAGS_TOOL_O2_C11   := -O2 -std=c11  -Wall -Wextra -MMD -MP
# udp_relay: gcc -O2 -Wall -Wextra -std=gnu11
CFLAGS_TOOL_O2_GNU11 := -O2 -std=gnu11 -Wall -Wextra -MMD -MP

# GUI helpers
GUI         ?= 0
GUI_DEFS     = -DUSE_SDL2_GUI -DUSE_SDL2_TTF
GUI_CFLAGS   = $(shell $(PKGCONFIG) --cflags sdl2 SDL2_ttf 2>/dev/null)
GUI_LDLIBS   = $(shell $(PKGCONFIG) --libs   sdl2 SDL2_ttf 2>/dev/null)

# SDL-only helpers (used by joystick2crfs)
SDL2_CFLAGS  = $(shell $(PKGCONFIG) --cflags sdl2 2>/dev/null)
SDL2_LDLIBS  = $(shell $(PKGCONFIG) --libs   sdl2 2>/dev/null)

# ===== Helper macro to define a build "flavor" =====
# $(call DEFINE_FLAVOR,flavor_name,CC,build_subdir,outbase,suffix,strip_tool)
define DEFINE_FLAVOR

# Flavor variables
$1_CC        := $2
$1_BUILD     := build/$3
$1_OUTBASE   := $4
$1_SUFFIX    := $5
$1_STRIP     := $6
$1_CPPFLAGS  := $(CPPFLAGS_COM)
$1_CFLAGS    := $(CFLAGS_COM)
$1_LDFLAGS   := $(LDFLAGS_COM)
$1_LDLIBS    :=

# Files per flavor
$1_OBJ_CORE  := $(addprefix $$($1_BUILD)/,$(SRC_CORE:.c=.o))
$1_OBJ_GUI   := $(addprefix $$($1_BUILD)/,$(SRC_GUI:.c=.o))
$1_OBJ_NOGUI := $$($1_OBJ_CORE)
$1_OBJ_GUION := $$($1_OBJ_CORE) $$($1_OBJ_GUI)

# Tools (single-file utilities)
$1_OBJ_SSE   := $$($1_BUILD)/sse_tail.o
$1_OBJ_UDP   := $$($1_BUILD)/udp_relay.o
$1_OBJ_AOSD  := $$($1_BUILD)/antenna_osd.o
$1_OBJ_IP2U  := $$($1_BUILD)/ip2uart.o

# CivetWeb per-file tweak
$1_CFLAGS_CIVETWEB := $$($1_CFLAGS) -Wno-unused-variable

# Output names (with/without GUI)
$1_BIN_NOGUI := $$($1_OUTBASE)
$1_BIN_GUI   := $$($1_OUTBASE)-gui

# Tool output names (use explicit suffix)
$1_BIN_SSE   := sse_tail$$($1_SUFFIX)
$1_BIN_UDP   := udp_relay$$($1_SUFFIX)
$1_BIN_AOSD  := antenna_osd$$($1_SUFFIX)
$1_BIN_IP2U  := ip2uart$$($1_SUFFIX)

# Phony targets for this flavor
.PHONY: $1 $1-gui $1-sse_tail $1-udp_relay $1-antenna_osd $1-ip2uart

$1: $$($1_BIN_NOGUI)
$1-gui: GUI=1
$1-gui: $$($1_BIN_GUI)

$1-sse_tail: $$($1_BIN_SSE)
$1-udp_relay: $$($1_BIN_UDP)
$1-antenna_osd: $$($1_BIN_AOSD)
$1-ip2uart: $$($1_BIN_IP2U)

# Link rules (strip output if strip tool exists)
$$($1_BIN_NOGUI): $$($1_OBJ_NOGUI)
	$$($1_CC) $$^ $$($1_LDFLAGS) $$($1_LDLIBS) -o $$@
	@command -v $$($1_STRIP) >/dev/null 2>&1 && $$($1_STRIP) $$@ || true

$$($1_BIN_GUI): $$($1_OBJ_GUION)
	$$($1_CC) $$^ $$($1_LDFLAGS) $$($1_LDLIBS) $(GUI_LDLIBS) -o $$@
	@command -v $$($1_STRIP) >/dev/null 2>&1 && $$($1_STRIP) $$@ || true

$$($1_BIN_SSE): $$($1_OBJ_SSE)
	$$($1_CC) $$^ -o $$@
	@command -v $$($1_STRIP) >/dev/null 2>&1 && $$($1_STRIP) $$@ || true

$$($1_BIN_UDP): $$($1_OBJ_UDP)
	$$($1_CC) $$^ -o $$@
	@command -v $$($1_STRIP) >/dev/null 2>&1 && $$($1_STRIP) $$@ || true

$$($1_BIN_AOSD): $$($1_OBJ_AOSD)
	$$($1_CC) $$($1_CPPFLAGS) $$($1_CFLAGS) $$^ $$($1_LDFLAGS) $$($1_LDLIBS) -o $$@
	@command -v $$($1_STRIP) >/dev/null 2>&1 && $$($1_STRIP) $$@ || true

$$($1_BIN_IP2U): $$($1_OBJ_IP2U)
	$$($1_CC) $$^ $$($1_LDFLAGS) -o $$@
	@command -v $$($1_STRIP) >/dev/null 2>&1 && $$($1_STRIP) $$@ || true

# Compile rules
$$($1_BUILD)/%.o: $(SRC_DIR)/%.c | $$($1_BUILD)
	$$(if $$(filter 1,$$(GUI)), \
		$$($1_CC) $$($1_CPPFLAGS) $(GUI_DEFS) $(GUI_CFLAGS) $$($1_CFLAGS) -c $$< -o $$@, \
		$$($1_CC) $$($1_CPPFLAGS) $$($1_CFLAGS) -c $$< -o $$@)

# CivetWeb with extra flag
$$($1_BUILD)/civetweb.o: $(SRC_DIR)/civetweb.c | $$($1_BUILD)
	$$(if $$(filter 1,$$(GUI)), \
		$$($1_CC) $$($1_CPPFLAGS) $(GUI_DEFS) $(GUI_CFLAGS) $$($1_CFLAGS_CIVETWEB) -c $$< -o $$@, \
		$$($1_CC) $$($1_CPPFLAGS) $$($1_CFLAGS_CIVETWEB) -c $$< -o $$@)

# Tool compile rules with their specific flags
$$($1_BUILD)/sse_tail.o: $(SRC_DIR)/sse_tail.c | $$($1_BUILD)
	$$($1_CC) $$($1_CPPFLAGS) $(CFLAGS_TOOL_O2_C11) -c $$< -o $$@

$$($1_BUILD)/udp_relay.o: $(SRC_DIR)/udp_relay.c | $$($1_BUILD)
	$$($1_CC) $$($1_CPPFLAGS) $(CFLAGS_TOOL_O2_GNU11) -c $$< -o $$@

$$($1_BUILD)/ip2uart.o: $(SRC_DIR)/ip2uart.c | $$($1_BUILD)
	$$($1_CC) $$($1_CPPFLAGS) $(CFLAGS_TOOL_O2_GNU11) -c $$< -o $$@

# Build dir
$$($1_BUILD):
	mkdir -p $$@

# Include deps
-include $$($1_OBJ_NOGUI:.o=.d)
-include $$($1_OBJ_GUION:.o=.d)
-include $$($1_OBJ_SSE:.o=.d)
-include $$($1_OBJ_UDP:.o=.d)
-include $$($1_OBJ_AOSD:.o=.d)
-include $$($1_OBJ_IP2U:.o=.d)

endef

# ===== Define the three flavors =====
# native -> ./autod / ./autod-gui / ./sse_tail / ./udp_relay
$(eval $(call DEFINE_FLAVOR,native,$(CC_NATIVE),native,$(APP),,$(STRIP_NATIVE)))
# musl   -> ./autod-musl / ./autod-musl-gui / ./sse_tail-musl / ./udp_relay-musl
$(eval $(call DEFINE_FLAVOR,musl,$(CC_MUSL),musl,$(APP)-musl,-musl,$(STRIP_MUSL)))
# gnu    -> ./autod-gnu / ./autod-gnu-gui / ./sse_tail-gnu / ./udp_relay-gnu
$(eval $(call DEFINE_FLAVOR,gnu,$(CC_GNU),gnu,$(APP)-gnu,-gnu,$(STRIP_GNU)))

# ===== Top-level convenience targets =====
.PHONY: all gui tools tools-musl tools-gnu clean strip install help

all: native
gui: native-gui

# Utilities (native by default)
tools: sse_tail udp_relay antenna_osd ip2uart joystick2crfs
tools-musl: sse_tail-musl udp_relay-musl antenna_osd-musl ip2uart-musl
tools-gnu: sse_tail-gnu udp_relay-gnu antenna_osd-gnu ip2uart-gnu

clean:
	rm -rf build \
	       $(APP) $(APP)-gui $(APP)-musl $(APP)-musl-gui $(APP)-gnu $(APP)-gnu-gui \
	       sse_tail sse_tail-musl sse_tail-gnu \
	       udp_relay udp_relay-musl udp_relay-gnu \
	       antenna_osd antenna_osd-musl antenna_osd-gnu \
	       ip2uart ip2uart-musl ip2uart-gnu \
	       joystick2crfs

# Strip whatever exists (no-op if strip tools missing)
strip: ;
	-@command -v $(STRIP_NATIVE) >/dev/null 2>&1 && $(STRIP_NATIVE) $(APP) sse_tail udp_relay antenna_osd ip2uart joystick2crfs 2>/dev/null || true
	-@command -v $(STRIP_MUSL)   >/dev/null 2>&1 && $(STRIP_MUSL)   $(APP)-musl sse_tail-musl udp_relay-musl antenna_osd-musl ip2uart-musl 2>/dev/null || true
	-@command -v $(STRIP_GNU)    >/dev/null 2>&1 && $(STRIP_GNU)    $(APP)-gnu sse_tail-gnu udp_relay-gnu antenna_osd-gnu ip2uart-gnu 2>/dev/null || true

JOYSTICK2CRFS_BUILD := build/native
JOYSTICK2CRFS_BIN   := joystick2crfs
JOYSTICK2CRFS_OBJ   := $(JOYSTICK2CRFS_BUILD)/joystick2crfs.o

$(JOYSTICK2CRFS_BIN): $(JOYSTICK2CRFS_OBJ)
	$(CC_NATIVE) $^ $(LDFLAGS_COM) $(SDL2_LDLIBS) -o $@
	@command -v $(STRIP_NATIVE) >/dev/null 2>&1 && $(STRIP_NATIVE) $@ || true

$(JOYSTICK2CRFS_OBJ): $(SRC_DIR)/joystick2crfs.c | $(JOYSTICK2CRFS_BUILD)
	$(CC_NATIVE) $(CPPFLAGS_COM) $(CFLAGS_TOOL_O2_GNU11) $(SDL2_CFLAGS) -c $< -o $@

-include $(JOYSTICK2CRFS_OBJ:.o=.d)

install: native udp_relay
	@if command -v systemctl >/dev/null 2>&1 && [ -z "$(DESTDIR)" ]; then \
		systemctl stop $(APP).service 2>/dev/null || true; \
		systemctl stop $(UDP_APP).service 2>/dev/null || true; \
	fi
	install -Dm755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)
	install -d $(DESTDIR)$(VRXDIR)
	install -m644 html/autod/vrx_index.html $(DESTDIR)$(VRXDIR)/vrx_index.html
	install -m755 scripts/vrx/exec-handler.sh $(DESTDIR)$(VRXDIR)/exec-handler.sh
	@set -e; \
	for helper in scripts/vrx/*.msg; do \
		[ -e "$$helper" ] || continue; \
		install -m644 "$$helper" "$(DESTDIR)$(VRXDIR)/"; \
	done
	@set -e; \
	confdir="$(DESTDIR)$(SYSCONFDIR)/$(APP)"; \
	install -d "$$confdir"; \
	target="$$confdir/autod.conf"; \
	install -m644 configs/autod.conf "$$target"; \
	sed -i \
		-e 's#^interpreter=.*#interpreter=$(VRXDIR)/exec-handler.sh#' \
		-e 's#^ui_path=.*#ui_path=$(VRXDIR)/vrx_index.html#' \
		"$$target"
	install -d $(DESTDIR)$(SYSTEMD_DIR)
	sed \
		-e 's#@AUTOD_BIN@#$(BINDIR)/$(APP)#g' \
		-e 's#@AUTOD_CONF@#$(SYSCONFDIR)/$(APP)/autod.conf#g' \
		-e 's#@VRX_DIR@#$(VRXDIR)#g' \
		configs/autod.service > $(DESTDIR)$(SYSTEMD_DIR)/$(APP).service
	chmod 644 $(DESTDIR)$(SYSTEMD_DIR)/$(APP).service
	install -Dm755 $(UDP_APP) $(DESTDIR)$(BINDIR)/$(UDP_APP)
	@set -e; \
	udp_confdir="$(DESTDIR)$(UDP_CONFDIR)"; \
	install -d "$$udp_confdir"; \
	udp_target="$$udp_confdir/$(UDP_APP).conf"; \
	install -m644 configs/$(UDP_APP).conf "$$udp_target";
	install -d $(DESTDIR)$(UDP_HTMLDIR)
	install -m644 html/udp_relay/$(UDP_UI_ASSET) $(DESTDIR)$(UDP_UI_PATH)
	sed \
		-e 's#@UDP_RELAY_BIN@#$(BINDIR)/$(UDP_APP)#g' \
		-e 's#@UDP_UI_PATH@#$(UDP_UI_PATH)#g' \
		configs/udp_relay.service > $(DESTDIR)$(SYSTEMD_DIR)/$(UDP_APP).service
	chmod 644 $(DESTDIR)$(SYSTEMD_DIR)/$(UDP_APP).service
	@if command -v systemctl >/dev/null 2>&1 && [ -z "$(DESTDIR)" ]; then \
		systemctl daemon-reload; \
		systemctl start $(APP).service 2>/dev/null || true; \
		systemctl start $(UDP_APP).service 2>/dev/null || true; \
	fi

help:
	@echo "Builds:"
	@echo "  make                 -> native (no GUI) => ./$(APP)"
	@echo "  make gui             -> native with GUI => ./$(APP)-gui"
	@echo "  make musl            -> musl (no GUI)   => ./$(APP)-musl"
	@echo "  make musl-gui        -> musl with GUI   => ./$(APP)-musl-gui"
	@echo "  make gnu             -> gnu (no GUI)    => ./$(APP)-gnu"
	@echo "  make gnu-gui         -> gnu with GUI    => ./$(APP)-gnu-gui"
	@echo ""
	@echo "Utilities:"
	@echo "  make tools           -> native sse_tail and udp_relay"
	@echo "  make tools-musl      -> musl  sse_tail-musl and udp_relay-musl"
	@echo "  make tools-gnu       -> gnu   sse_tail-gnu  and udp_relay-gnu"
	@echo ""
	@echo "Env overrides:"
	@echo "  CC_NATIVE=...  CC_MUSL=...  CC_GNU=...  PKGCONFIG=..."
	@echo "  STRIP_NATIVE=... STRIP_MUSL=... STRIP_GNU=..."
