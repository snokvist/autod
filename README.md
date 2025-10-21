# autod

`autod` is a lightweight HTTP control plane intended for embedded or fielded systems. It embeds the [CivetWeb](https://github.com/civetweb/civetweb) web server, exposes execution and discovery endpoints, optionally performs LAN scans to find peer nodes, and can ship with a minimal SDL2-based GUI. The project also includes small helper tools (`sse_tail`, `udp_relay`, and `ip2uart`) and a browser UI that can be served directly from the daemon.

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
- **`ip2uart`** – Bidirectional bridge between a UART (real TTY or stdio) and TCP/UDP sockets.

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
| Helper tools (native)  | `make tools` | `./sse_tail`, `./udp_relay`, `./ip2uart` |
| `joystick2crfs` utility | `make joystick2crfs` | `./joystick2crfs` (requires SDL2; config at `/etc/joystick2crfs.conf`) |
| Musl cross build       | `make musl` | `./autod-musl` (and friends) |
| GNU cross build        | `make gnu`  | `./autod-gnu` (and friends) |
| Clean intermediates    | `make clean` | removes `build/` and produced binaries |

Each flavour drops intermediates under `build/<flavour>/` and strips binaries automatically when the matching `strip` tool is found.

### Installing on Debian/`systemd`

`make install` builds the native daemon and the bundled `udp_relay` helper, then stages a simple system-wide layout that targets Debian 11:

- `autod` → `$(PREFIX)/bin/autod` (default prefix `/usr/local`).
- VRX web UI and helpers → `$(PREFIX)/share/autod/vrx/` (`vrx_index.html`, `exec-handler.sh`, `*.msg`).
- Configuration → `/etc/autod/autod.conf` (the shipped version overwrites any existing file so updates land immediately).
- Service unit → `/etc/systemd/system/autod.service` pointing at the installed binary and config, running as `root` so helper scripts can signal privileged daemons.
- `udp_relay` → `$(PREFIX)/bin/udp_relay`.
- `ip2uart` → build with `make ip2uart` or via the `make tools` aggregate target when you need the UART↔IP bridge.
- `udp_relay` configuration → `/etc/udp_relay/udp_relay.conf` (overwritten in-place during each install).
- `ip2uart` configuration → `/etc/ip2uart.conf` (not installed automatically; see [Helper Tools](#helper-tools)).
- `udp_relay` service unit → `/etc/systemd/system/udp_relay.service` which runs the helper as `root` for consistent behaviour with the UI bindings.
- VRX udp_relay UI asset → `$(PREFIX)/share/autod/udp_relay/vrx_udp_relay.html` (the service runs the binary with `--ui` pointing at this file).

During installation on a host with `systemctl` available (and no `DESTDIR`), the recipe stops any running `autod`/`udp_relay` services, installs the new assets and configuration in place, reloads `systemd`, and starts the services again without enabling them. If you staged into a `DESTDIR`, reload manually once the files land on the target system, then enable the daemon:

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
- `[scan]` – Optional list of additional CIDR blocks that should be probed every sweep.
- `[exec]` – Interpreter invoked for `/exec` requests, plus timeout and output limits.
- `[caps]` – Device identity metadata and optional capability list exposed at `/caps`.
- `[announce]` – List of Server-Sent Event (SSE) streams advertised to clients.
- `[ui]` – Controls for serving the static UI bundle.

The execution plane contract (`/exec` requests and handler expectations) is documented in [`handler_contract.txt`](handler_contract.txt). Ensure your handler script matches that agreement; a minimal sample lives in [`scripts/simple_exec-handler.sh`](scripts/simple_exec-handler.sh).

### Sending UDP packets via the HTTP API

`autod` exposes a `/udp` endpoint so web clients can emit connectionless UDP datagrams without needing raw socket access. The handler accepts `POST` requests with a JSON payload describing the target host, port, and message body. You may supply either a UTF-8 string via `"payload"` or arbitrary binary content via `"payload_base64"`:

```bash
curl -X POST http://HOST:PORT/udp \
  -H 'Content-Type: application/json' \
  -d '{
        "host": "192.168.1.55",
        "port": 5005,
        "payload": "{\"command\":\"ping\"}"
      }'
```

For binary datagrams, encode the bytes in base64 and place them in `"payload_base64"` instead. The response confirms delivery and echoes the number of bytes written.

### Optional LAN Scanner

When `[server] enable_scan = 1`, the daemon seeds itself into the scan database and launches background probing via functions in [`src/scan.c`](src/scan.c). Clients can poll `/nodes` for progress and discovered peers. If you also define one or more `extra_subnet = 10.10.10.0/24` lines inside a `[scan]` section, the scanner will include those CIDR blocks alongside any directly detected interfaces. `/32` entries are treated as single hosts.

### Bundled UI

Static files under `html/` can be served by the daemon (when `serve_ui=1`) or by any external web server. The provided `scripts/minify_html.sh` helps regenerate minified assets if you edit the UI. Most role-specific pages (for example [`html/autod/vrx_index.html`](html/autod/vrx_index.html) and [`html/autod/vtx_index.html`](html/autod/vtx_index.html)) assume the helper wrappers in [`scripts/vrx/`](scripts/vrx/) and [`scripts/vtx/`](scripts/vtx/) are kept in sync; if you change the script inputs, command names, or help text make the parallel update in the corresponding HTML controls so buttons, dropdowns, and embedded consoles continue to match the backend behavior.

### Helper Tools

- `antenna_osd`: build with `make antenna_osd` (or the `musl`/`gnu` variants) to render an MSP/Canvas OSD overlay. It reads `/etc/antenna_osd.conf` by default and accepts an alternate path as its sole positional argument, with a sample config in [`configs/antenna_osd.conf`](configs/antenna_osd.conf).
- `sse_tail`: build with `make tools` and run `./sse_tail http://host:port/path` to observe SSE streams announced in the config.
- `udp_relay`: controlled through its own config file [`configs/udp_relay.conf`](configs/udp_relay.conf). When installed system-wide the default path is `/etc/udp_relay/udp_relay.conf`; you can run it directly or via UI bindings.
- `ip2uart`: run `make ip2uart` (or any of the cross variants) to build a bidirectional bridge between a UART and a TCP or UDP peer. It uses `/etc/ip2uart.conf` by default; a sample lives in [`configs/ip2uart.conf`](configs/ip2uart.conf). Pass `-c /path/to/conf` to point at a different configuration, `-v`/`-vv`/`-vvv` for progressively verbose logging, and send `SIGHUP` to reload the config without dropping the process.
- `joystick2crfs`: build with `make joystick2crfs` to translate an SDL2 joystick into CRSF frames. The utility requires SDL2 development headers (`libsdl2-dev` on Debian-based systems) and reads `/etc/joystick2crfs.conf` by default. A documented sample lives in [`configs/joystick2crfs.conf`](configs/joystick2crfs.conf); adjust the transport and channel mapping there.

#### `ip2uart` configuration keys

The bridge consumes a simple `key=value` configuration file. Important options exposed by [`src/ip2uart.c`](src/ip2uart.c):

| Section | Key | Description |
| ------- | --- | ----------- |
| Selector | `uart_backend` | `tty` opens the TTY listed in `uart_device`, while `stdio` reads stdin and writes stdout (useful for chaining processes). |
| Selector | `net_mode` | Choose `tcp_server`, `tcp_client`, or `udp_peer`. The TCP server accepts a single client at a time; the client mode auto-reconnects. |
| UART | `uart_device`, `uart_baud`, `uart_databits`, `uart_parity`, `uart_stopbits`, `uart_flow` | Standard serial-port parameters when `uart_backend=tty`. |
| TCP server | `listen_addr`, `listen_port`, `tcp_listen_backlog` | Bind address/port and backlog depth for `net_mode=tcp_server`. |
| TCP client | `remote_host`, `remote_port`, `reconnect_delay_ms`, `tcp_nodelay` | Target host/port plus timer-driven reconnect delay for `net_mode=tcp_client`. |
| UDP peer | `udp_bind_addr`, `udp_bind_port`, `udp_peer_addr`, `udp_peer_port` | Local bind address/port and optional static peer for UDP. If no peer is set the bridge accepts datagrams from any sender and only uses outbound addressing when a peer is provided. |
| UDP coalescing | `udp_coalesce_bytes`, `udp_coalesce_idle_ms`, `udp_max_datagram` | Controls the packet-aggregation buffer before flushing to the peer. |
| Logging/stats | `log_file`, `dump_on_start`, `status_interval_ms` | Periodically dump counters and TCP peer metadata to an INI-style log file. |
| Buffers | `rx_buf`, `tx_buf` | Size in bytes of receive scratch buffers and the ring buffers used for non-blocking short-write handling. |

The daemon keeps ring buffers for both directions so short writes (for example when a UART blocks) do not stall the network path. When `net_mode=udp_peer` is active the bridge gathers bytes until the coalesce threshold is met or the idle timer expires, logging why a datagram was sent. Statistics snapshots include byte and packet counters for each direction plus TCP session metadata. The TCP client path maintains a timer-driven reconnect loop so UART reads and writes continue immediately even while the socket is offline, and the TCP server backlog can be tuned to absorb bursts of incoming clients. Leaving `udp_peer_addr` blank tells the bridge to learn the remote endpoint from inbound packets automatically, which keeps UART data flowing without manual configuration.

##### Behavior notes

- `reconnect_delay_ms` acts as a non-blocking retry interval; the bridge continues servicing the UART while it waits to redial the TCP client connection.
- `tcp_listen_backlog` defaults to 8 so short reconnect storms are queued instead of refused; raise or lower it to fit your deployment.
- If `udp_peer_addr` is empty the bridge updates its outbound peer each time it hears from a sender, so the last inbound datagram controls the return path.

---

## 5. Development Tips

- Keep documentation in sync with any new configuration keys or API endpoints, especially [`handler_contract.txt`](handler_contract.txt).
- The codebase is warning-clean with `-Wall -Wextra`; please maintain that standard.
- For GUI work, remember to export `AUTOD_GUI_FONT=/path/to/font.ttf` before launching `./autod-gui --gui` so SDL_ttf can locate a font (the daemon prints a hint on startup).
- If you change the HTTP surface area or scanner behavior, update the sample configs and README accordingly.

Happy hacking!
