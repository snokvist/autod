# ==== Project ====
APP           := autod

# Build configuration (override as needed)
PREFIX        ?= /usr/local
BINDIR        ?= $(PREFIX)/bin
SYSCONFDIR    ?= /etc
DATADIR       ?= $(PREFIX)/share
AUTOD_DATADIR ?= $(DATADIR)/$(APP)
VRXDIR        ?= $(AUTOD_DATADIR)/vrx
SYSTEMD_DIR   ?= /etc/systemd/system

# Toolchain (Buildroot friendly)
CC            ?= $(CROSS_COMPILE)gcc
STRIP         ?= $(CROSS_COMPILE)strip
PKG_CONFIG    ?= pkg-config

# Paths and sources
SRC_DIR       := src
BUILD_DIR     := build
SRCS          := autod.c sync.c scan.c parson.c civetweb.c
OBJS          := $(addprefix $(BUILD_DIR)/,$(SRCS:.c=.o))

# Flags
CPPFLAGS     += -I$(SRC_DIR) -D_POSIX_C_SOURCE=200809L -DNO_SSL -DNO_CGI -DNO_FILES
CFLAGS       ?= -Os -std=c11 -Wall -Wextra
CFLAGS       += -MMD -MP
LDFLAGS      += -pthread

.PHONY: all clean install help

all: $(APP)

$(APP): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@command -v $(STRIP) >/dev/null 2>&1 && $(STRIP) $@ || true

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/civetweb.o: $(SRC_DIR)/civetweb.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-unused-variable -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD_DIR) $(APP)

install: $(APP)
	install -Dm755 $(APP) $(DESTDIR)$(BINDIR)/$(APP)
	install -d $(DESTDIR)$(VRXDIR)
	install -m644 html/autod/vrx_index.html $(DESTDIR)$(VRXDIR)/vrx_index.html
	@set -e; \
	for dir in $$(find html/autod/assets -type d); do \
		rel="$${dir#html/autod/}"; \
		install -d "$(DESTDIR)$(VRXDIR)/$$rel"; \
	done
	@set -e; \
	for file in $$(find html/autod/assets -type f); do \
		rel="$${file#html/autod/}"; \
		install -m644 "$$file" "$(DESTDIR)$(VRXDIR)/$$rel"; \
	done
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

help:
	@echo "Builds:"
	@echo "  make                 -> $(APP)"
	@echo ""
	@echo "Env overrides:"
	@echo "  CC=... CROSS_COMPILE=... STRIP=... PREFIX=..."
