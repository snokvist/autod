# autod

`autod` is a lightweight HTTP control plane intended for embedded or fielded systems. It embeds the [CivetWeb](https://github.com/civetweb/civetweb) web server, exposes execution and discovery endpoints, optionally performs LAN scans to find peer nodes, and can ship with a minimal SDL2-based GUI. The project also includes small helper tools (`sse_tail` and `udp_relay`) and a browser UI that can be served directly from the daemon.

This document walks you through building the software, configuring it, and understanding the repository layout so that new users and developers can get started quickly.

---

## 1. Repository Layout

```
Makefile             # Build entry points for native & cross targets
AGENTS.md            # Contribution guidance for this repository
configs/             # Example configuration files for the daemon and tools
handler_contract.txt # Execution plane API contract between daemon and handler script
html/                # Static assets for the optional web UI (dashboards, forms, embedded consoles)
scripts/             # Helper scripts (HTML minifier, exec handlers, VRX/VTX wrappers backing the UI)
src/                 # C sources for the daemon, GUI, scanner, and utilities
```

Key executables built from `src/`:

- **`autod`** – HTTP control daemon without GUI.
- **`autod-gui`** – Same daemon but with an SDL2 overlay displaying scan status (requires SDL2).
- **`sse_tail`** – Convenience tool that follows Server-Sent Events endpoints.
- **`udp_relay`** – UDP fan-out utility controlled through the daemon/UI.

---

## 2. Prerequisites

### Native Linux Build

Install a compiler toolchain plus optional SDL2 dependencies if you want the GUI binary:

```bash
sudo apt-get update
sudo apt-get install build-essential pkg-config
# GUI build only
sudo apt-get install libsdl2-dev libsdl2-ttf-dev
```

### Cross Compilation

The `Makefile` understands two cross flavours out of the box:

- `arm-linux-musleabihf-gcc` for static musl builds (`make musl`).
- `arm-linux-gnueabihf-gcc` for glibc-based builds (`make gnu`).

Set the `CC_MUSL`, `CC_GNU`, `STRIP_MUSL`, and `STRIP_GNU` environment variables if your triplet names differ.

---

## 3. Building

All commands are run from the repository root.

| Goal | Command | Output |
| ---- | ------- | ------ |
| Native headless daemon | `make` | `./autod` |
| Native daemon with GUI | `make gui` | `./autod-gui` (requires SDL2/SDL_ttf) |
| Helper tools (native)  | `make tools` | `./sse_tail`, `./udp_relay` |
| Musl cross build       | `make musl` | `./autod-musl` (and friends) |
| GNU cross build        | `make gnu`  | `./autod-gnu` (and friends) |
| Clean intermediates    | `make clean` | removes `build/` and produced binaries |

Each flavour drops intermediates under `build/<flavour>/` and strips binaries automatically when the matching `strip` tool is found.

### Installing on Debian/`systemd`

`make install` builds the native daemon and the bundled `udp_relay` helper, then stages a simple system-wide layout that targets Debian 11:

- `autod` → `$(PREFIX)/bin/autod` (default prefix `/usr/local`).
- VRX web UI and helpers → `$(PREFIX)/share/autod/vrx/` (`vrx_index.html`, `exec-handler.sh`, `*.msg`).
- Configuration → `/etc/autod/autod.conf` (existing files are preserved and a `.dist` copy is written instead).
- Service unit → `/etc/systemd/system/autod.service` pointing at the installed binary and config, running as `root` so helper scripts can signal privileged daemons.
- `udp_relay` → `$(PREFIX)/bin/udp_relay`.
- `udp_relay` configuration → `/etc/udp_relay/udp_relay.conf` (existing files are preserved and a `.dist` copy is written instead).
- `udp_relay` service unit → `/etc/systemd/system/udp_relay.service` which runs the helper as `root` for consistent behaviour with the UI bindings.

After installation `make install` reloads `systemd` automatically when installing directly on the host (no `DESTDIR`). If you staged into a `DESTDIR`, reload manually once the files land on the target system, then enable the daemon:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now autod
# Optional helper
sudo systemctl enable --now udp_relay
```

The service starts in the VRX data directory so the bundled helper scripts can find their message payloads without additional configuration.

Pixelpilot Mini RK actions (`toggle_osd`, `toggle_recording`) signal the `pixelpilot_mini_rk` process directly (`SIGUSR1` for the OSD overlay, `SIGUSR2` for recording), while the `reboot` and `shutdown` commands launch `reboot now`/`shutdown now` in the background. The installed service runs as `root`, and the bundled `exec-handler.sh` now exits with a clear error when invoked without elevated privileges. If you run the daemon manually, mimic that setup so the helper can send the required signals; otherwise the UI will report `failed to signal pixelpilot_mini_rk`.

If you prefer direct compiler invocation, consult the comment at the top of [`src/autod.c`](src/autod.c), but using the provided `Makefile` keeps flags consistent.

---

## 4. Configuration & Runtime

The daemon looks for `./autod.conf` by default. You can provide an alternate path as the sole positional argument:

```bash
./autod configs/autod.conf
# or, when SDL2 support was enabled during compilation
./autod-gui --gui configs/autod.conf
```

Important sections inside [`configs/autod.conf`](configs/autod.conf):

- `[server]` – HTTP bind address/port and whether the LAN scanner starts automatically.
- `[exec]` – Interpreter invoked for `/exec` requests, plus timeout and output limits.
- `[caps]` – Device identity metadata and optional capability list exposed at `/caps`.
- `[announce]` – List of Server-Sent Event (SSE) streams advertised to clients.
- `[ui]` – Controls for serving the static UI bundle.

The execution plane contract (`/exec` requests and handler expectations) is documented in [`handler_contract.txt`](handler_contract.txt). Ensure your handler script matches that agreement; a minimal sample lives in [`scripts/simple_exec-handler.sh`](scripts/simple_exec-handler.sh).

### Optional LAN Scanner

When `[server] enable_scan = 1`, the daemon seeds itself into the scan database and launches background probing via functions in [`src/scan.c`](src/scan.c). Clients can poll `/nodes` for progress and discovered peers.

### Bundled UI

Static files under `html/` can be served by the daemon (when `serve_ui=1`) or by any external web server. The provided `scripts/minify_html.sh` helps regenerate minified assets if you edit the UI. Most role-specific pages (for example [`html/autod/vrx_index.html`](html/autod/vrx_index.html) and [`html/autod/vtx_index.html`](html/autod/vtx_index.html)) assume the helper wrappers in [`scripts/vrx/`](scripts/vrx/) and [`scripts/vtx/`](scripts/vtx/) are kept in sync; if you change the script inputs, command names, or help text make the parallel update in the corresponding HTML controls so buttons, dropdowns, and embedded consoles continue to match the backend behavior.

### Helper Tools

- `sse_tail`: build with `make tools` and run `./sse_tail http://host:port/path` to observe SSE streams announced in the config.
- `udp_relay`: controlled through its own config file [`configs/udp_relay.conf`](configs/udp_relay.conf). When installed system-wide the default path is `/etc/udp_relay/udp_relay.conf`; you can run it directly or via UI bindings.

---

## 5. Development Tips

- Keep documentation in sync with any new configuration keys or API endpoints, especially [`handler_contract.txt`](handler_contract.txt).
- The codebase is warning-clean with `-Wall -Wextra`; please maintain that standard.
- For GUI work, remember to export `AUTOD_GUI_FONT=/path/to/font.ttf` before launching `./autod-gui --gui` so SDL_ttf can locate a font (the daemon prints a hint on startup).
- If you change the HTTP surface area or scanner behavior, update the sample configs and README accordingly.

Happy hacking!
