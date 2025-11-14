# autod

`autod` is a lightweight HTTP control plane intended for embedded or fielded systems. It embeds the [CivetWeb](https://github.com/civetweb/civetweb) web server, exposes execution and discovery endpoints, optionally performs LAN scans to find peer nodes, and ships with small helper tools (`sse_tail`, `udp_relay`, and `ip2uart`) plus a browser UI that can be served directly from the daemon.

This document walks you through building the software, configuring it, and understanding the repository layout so that new users and developers can get started quickly.

---

## 1. Repository Layout

```
Makefile             # Build entry points for native & cross targets
AGENTS.md            # Contribution guidance for this repository
LICENSE.md           # Personal-use license terms (see Licensing)
README.md            # This guide
configs/             # Example configuration files for the daemon and tools
fonts/               # Sample fonts and placeholders used by the OSD helpers
handler_contract.txt # Execution plane API contract between daemon and handler script
html/                # Static assets for the optional web UI (dashboards, forms, embedded consoles)
remote.md            # End-to-end joystick / CRSF deployment guide
scripts/             # Helper scripts (HTML minifier, exec handlers, VRX/VTX wrappers backing the UI)
src/                 # C sources for the daemon, scanner, and utilities
tests/               # Smoke and regression tests for selected subsystems
```

Key executables built from `src/`:

- **`autod`** – HTTP control daemon.
- **`sse_tail`** – Convenience tool that follows Server-Sent Events endpoints.
- **`udp_relay`** – UDP fan-out utility (with optional UART sink) controlled through the daemon/UI.
- **`ip2uart`** – Bidirectional bridge between a UART (real TTY or stdio) and a UDP peer.
- **`antenna_osd`** – MSP/Canvas OSD renderer with configurable telemetry overlays.
- **`joystick2crfs`** – SDL2 joystick bridge that emits CRSF frames and optional SSE telemetry.

---

## 2. Prerequisites

### Native Linux Build

Install a compiler toolchain plus SDL2 headers (required for `joystick2crfs` and therefore the aggregated `make tools` target):

```bash
sudo apt-get update
sudo apt-get install build-essential pkg-config libsdl2-dev
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
| Native daemon | `make` | `./autod` |
| Helper tools (native)  | `make tools` | `./sse_tail`, `./udp_relay`, `./antenna_osd`, `./ip2uart`, `./joystick2crfs`* |
| `joystick2crfs` utility | `make joystick2crfs` | `./joystick2crfs` (requires SDL2; config at `/etc/joystick2crfs.conf`) |
| Musl cross build       | `make musl` | `./autod-musl` (and friends) |
| GNU cross build        | `make gnu`  | `./autod-gnu` (and friends) |
| Clean intermediates    | `make clean` | removes `build/` and produced binaries |

Each flavour drops intermediates under `build/<flavour>/` and strips binaries automatically when the matching `strip` tool is found.

\* `joystick2crfs` depends on SDL2. The `tools` target builds it alongside the other helpers; use the stand-alone `make joystick2crfs` target if you only want to compile the joystick bridge once the SDL2 development headers are present. Cross-toolchain aggregates (`make tools-musl`, `make tools-gnu`) omit the joystick helper because the build requires native SDL2 support.

### Installing on Debian/`systemd`

`make install` builds the native daemon and the bundled `udp_relay` and `joystick2crfs` helpers, then stages a simple system-wide layout that targets Debian 11:

- `autod` → `$(PREFIX)/bin/autod` (default prefix `/usr/local`).
- VRX web UI and helpers → `$(PREFIX)/share/autod/vrx/` (`vrx_index.html`, `exec-handler.sh`, `*.msg`).
- Configuration → `/etc/autod/autod.conf` (the shipped version overwrites any existing file so updates land immediately).
- Service unit → `/etc/systemd/system/autod.service` pointing at the installed binary and config, running as `root` so helper scripts can signal privileged daemons.
- `udp_relay` → `$(PREFIX)/bin/udp_relay`.
- `joystick2crfs` → `$(PREFIX)/bin/joystick2crfs`.
- `ip2uart` → build with `make ip2uart` or via the `make tools` aggregate target when you need the UART↔IP bridge.
- `udp_relay` configuration → `/etc/udp_relay/udp_relay.conf` (overwritten in-place during each install).
- `joystick2crfs` configuration → `/etc/joystick2crfs.conf` (overwritten in-place during each install).
- `ip2uart` configuration → `/etc/ip2uart.conf` (not installed automatically; see [Helper Tools](#helper-tools)).
- `udp_relay` service unit → `/etc/systemd/system/udp_relay.service` which runs the helper as `root` for consistent behaviour with the UI bindings.
- `joystick2crfs` service unit → `/etc/systemd/system/joystick2crfs.service` (includes an `ExecReload` that delivers `SIGHUP` so the daemon can re-read its config without a full restart).
- VRX udp_relay UI asset → `$(PREFIX)/share/autod/udp_relay/vrx_udp_relay.html` (the service runs the binary with `--ui` pointing at this file).

During installation on a host with `systemctl` available (and no `DESTDIR`), the recipe stops any running `autod`/`udp_relay`/`joystick2crfs` services, installs the new assets and configuration in place, reloads `systemd`, and starts the services again without enabling them. If you staged into a `DESTDIR`, reload manually once the files land on the target system, then enable the daemon:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now autod
# Optional helper
sudo systemctl enable --now udp_relay
# Optional joystick bridge
sudo systemctl enable --now joystick2crfs
```

The service starts in the VRX data directory so the bundled helper scripts can find their message payloads without additional configuration.

Pixelpilot Mini RK actions (`toggle_osd`, `toggle_recording`) signal the `pixelpilot_mini_rk` process directly (`SIGUSR1` for the OSD overlay, `SIGUSR2` for recording), while the `reboot` and `shutdown` commands launch `reboot now`/`shutdown now` in the background. The installed service runs as `root`, and the bundled `exec-handler.sh` now exits with a clear error when invoked without elevated privileges. If you run the daemon manually, mimic that setup so the helper can send the required signals; otherwise the UI will report `failed to signal pixelpilot_mini_rk`.

If you prefer direct compiler invocation, consult the comment at the top of [`src/autod.c`](src/autod.c), but using the provided `Makefile` keeps flags consistent.

---

## 4. Configuration & Runtime

The daemon looks for `./autod.conf` by default. You can provide an alternate path as the sole positional argument:

```bash
./autod configs/autod.conf
```

Sample configuration bundles ship with the repository:

- **Master example** – [`configs/autod.conf`](configs/autod.conf)
- **Slave example** – [`configs/slave/autod.conf`](configs/slave/autod.conf)

Important sections inside the master sample ([`configs/autod.conf`](configs/autod.conf)):

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

### Sync master/slave coordination

`autod` can now coordinate sync slots across a fleet using an HTTP-based control plane. Enable it via the `[sync]` section in `autod.conf`:

```ini
[sync]
# role can be "master" to accept slave registrations or "slave" to follow a master.
role = master
# When acting as a slave, point at the master's sync identifier using sync://.
# master_url = sync://autod-master/sync/register
register_interval_s = 30
allow_bind = 1        ; let POST /sync/bind re-point the slave at runtime
# id = custom-node-id ; defaults to the system hostname
```

Masters can advertise up to ten sync slots via `[sync.slotN]` sections. Each slot lists `/exec` payloads (JSON bodies) that run sequentially on the assigned slave whenever a new sync generation is issued:

```ini
[sync.slot1]
name = primary
exec = {"path": "/usr/local/bin/slot1-prepare"}
exec = {"path": "/usr/local/bin/slot1-finalise", "args": ["--ok"]}
```

- **Masters** advertise a `sync-master` capability in `/caps`, accept slave registrations at `POST /sync/register`, list known peers via `GET /sync/slaves`, and assign slots with `POST /sync/push`. The handler accepts bodies such as `{"moves": [{"slave_id": "alpha", "slot": 2}]}` to shuffle live assignments. During each heartbeat the master responds with the next slot command sequence (identified by generation) which the slave executes locally via the configured interpreter.
- **Slaves** (advertising `sync-slave`) maintain a background thread that posts to the configured `master_url` every `register_interval_s` seconds. When the value uses the `sync://` scheme the daemon resolves the identifier through the LAN discovery cache before contacting the master. The response includes the assigned slot, optional slot label, and any commands queued for the next generation; the slave runs each command in order and acknowledges completion on subsequent heartbeats. Slaves also expose `POST /sync/bind` so an operator or master can redirect a running node to a new controller without editing disk config—send either `{ "master_id": "sync-master-id" }` or a `master_url` that already uses the `sync://` format so the daemon persists the identifier.

POST `/sync/push` now accepts slot move requests (`{"moves": [...]}`) rather than configuration objects; the master increments the slot generation whenever an assignment changes so slaves replay their command list before acknowledging the new generation.

See the master ([`configs/autod.conf`](configs/autod.conf)) and slave ([`configs/slave/autod.conf`](configs/slave/autod.conf)) samples for full examples and the sync handlers in [`src/autod.c`](src/autod.c) for the request/response schema.

### Startup execution sequence

The optional `[startup]` section lets you queue `/exec` payloads that should run automatically once the HTTP server and background threads come online. Each `exec = ...` line is a JSON blob matching the body of a `POST /exec` request:

```ini
[startup]
exec = {"path": "/bin/echo", "args": ["autod", "ready"]}
exec = {"path": "/usr/local/bin/bootstrap"}
```

Entries execute sequentially (waterfall style): the daemon waits for each command to complete before launching the next. Standard output/stderr from each run is logged to stderr alongside the exit code so you can track bootstrap progress without instrumenting the handler script.

### Bundled UI

Static files under `html/` can be served by the daemon (when `serve_ui=1`) or by any external web server. The provided `scripts/minify_html.sh` helps regenerate minified assets if you edit the UI. Most role-specific pages (for example [`html/autod/vrx_index.html`](html/autod/vrx_index.html) and [`html/autod/vtx_index.html`](html/autod/vtx_index.html)) assume the helper wrappers in [`scripts/vrx/`](scripts/vrx/) and [`scripts/vtx/`](scripts/vtx/) are kept in sync; if you change the script inputs, command names, or help text make the parallel update in the corresponding HTML controls so buttons, dropdowns, and embedded consoles continue to match the backend behavior.

### Helper Tools

- `antenna_osd`: build with `make antenna_osd` (or the `musl`/`gnu` variants) to render an MSP/Canvas OSD overlay. It reads `/etc/antenna_osd.conf` by default and accepts an alternate path as its sole positional argument, with a sample config in [`configs/antenna_osd.conf`](configs/antenna_osd.conf). The helper can poll up to two telemetry files, smooth RSSI values across samples, and exposes configurable bar glyphs, headers, and system-message overlays. When both `info_file` and `info_file2` are provided, prefix telemetry keys with `file1:` or `file2:` to choose which source supplies each value.
- `sse_tail`: build with `make tools` and run `./sse_tail http://host:port/path` to observe SSE streams announced in the config.
- `udp_relay`: controlled through its own config file [`configs/udp_relay.conf`](configs/udp_relay.conf). When installed system-wide the default path is `/etc/udp_relay/udp_relay.conf`; you can run it directly or via UI bindings. Dest tokens now accept the literal UART tokens (`uart`, `uart1`, …), letting binds forward into up to four configured UART sinks. Populate the matching `uart<n>_*` keys (`uart0_device`, etc.) and add `bind=uart`/`bind=uart1:ip:port` entries so each serial bridge shows up as a regular bind source and can fan out to UDP peers. Outbound packets reuse the daemon's global `src_ip`, so no UART-specific source binding is required.
- `ip2uart`: run `make ip2uart` (or any of the cross variants) to build a bidirectional bridge between a UART and a UDP peer. It uses `/etc/ip2uart.conf` by default; a sample lives in [`configs/ip2uart.conf`](configs/ip2uart.conf). Pass `-c /path/to/conf` to point at a different configuration, use `-v` for once-per-second stats, and send `SIGHUP` to reload the config without dropping the process.
- `joystick2crfs`: build with `make joystick2crfs` to translate an SDL2 joystick into CRSF or MAVLink RC frames. The utility requires SDL2 development headers (`libsdl2-dev` on Debian-based systems) and reads `/etc/joystick2crfs.conf` by default. A documented sample lives in [`configs/joystick2crfs.conf`](configs/joystick2crfs.conf); adjust the UDP target, SSE streaming options, and channel mapping there. Set `protocol=crsf` (default) for the existing 16-channel CRSF frame packer or `protocol=mavlink` to emit MAVLink v2 `RC_CHANNELS_OVERRIDE` messages with the first eight channels scaled to 1000–2000 µs. When using MAVLink, tune `mavlink_sysid`, `mavlink_compid`, `mavlink_target_sysid`, and `mavlink_target_compid` to match your vehicle IDs. The `arm_toggle` key (default `5`) designates the momentary control that latches channel 5 high after a 1 s hold and releases on a short tap. Send `SIGHUP` to reload the config without restarting; when `sse_enabled=true` the binary hosts a single-client SSE feed at `sse_bind` + `sse_path`, publishing the latest channel values at 10 Hz.

For a complete walk-through that ties `autod`, `joystick2crfs`, and `ip2uart` together across a ground/vehicle link, read [`remote.md`](remote.md).

#### `ip2uart` configuration keys

The bridge consumes a simple `key=value` configuration file. Important options exposed by [`src/ip2uart.c`](src/ip2uart.c):

| Section | Key | Description |
| ------- | --- | ----------- |
| Selector | `uart_backend` | `tty` opens the TTY listed in `uart_device`, while `stdio` reads stdin and writes stdout (useful for chaining processes). |
| UART | `uart_device`, `uart_baud`, `uart_databits`, `uart_parity`, `uart_stopbits`, `uart_flow` | Standard serial-port parameters when `uart_backend=tty`. |
| UDP peer | `udp_bind_addr`, `udp_bind_port`, `udp_peer_addr`, `udp_peer_port` | Local bind address/port and optional static peer for UDP. If no peer is set the bridge accepts datagrams from any sender and learns the outbound destination from the most recent packet. |
| UDP coalescing | `udp_coalesce_bytes`, `udp_coalesce_idle_ms`, `udp_max_datagram` | Controls the packet-aggregation buffer before flushing to the peer. |
| Buffers | `rx_buf`, `tx_buf` | Size in bytes of receive scratch buffers and the ring buffers used for non-blocking short-write handling. |

The daemon keeps ring buffers for both directions so short writes (for example when a UART blocks) do not stall the network path. Bytes read from the UART accumulate until the coalesce threshold is met or the idle timer expires, at which point a datagram is emitted. Launching ip2uart with `-v` prints once-per-second stats (packets/s, bytes/s, and drop counters) to stderr. Leaving `udp_peer_addr` blank tells the bridge to learn the remote endpoint from inbound packets automatically, which keeps UART data flowing without manual configuration.

---

## 5. Development Tips

- Keep documentation in sync with any new configuration keys or API endpoints, especially [`handler_contract.txt`](handler_contract.txt).
- The codebase is warning-clean with `-Wall -Wextra`; please maintain that standard.
- If you change the HTTP surface area or scanner behavior, update the sample configs and README accordingly.

---

## 6. Licensing and Notice Placement

The repository ships with an **Autod Personal Use License** (`LICENSE.md`) that grants individuals the right to download, run, and modify the software for their own non-commercial projects while forbidding redistribution (source, binaries, configuration bundles, or online configurators) and any commercial exploitation without the owner’s prior written approval. The license also clarifies that derivative works must remain private unless explicit permission is granted. Joakim Snökvist (joakim.snokvist@gmail.com) is the rights holder who can authorize broader usage.

### Repository-level integration

1. Keep the authoritative text in `LICENSE.md`. Confirm the copyright line (`2025 Joakim Snökvist`) and contact address (`joakim.snokvist@gmail.com`) remain accurate; coordinate with the owner before editing.
2. Reference the license in this README (as done here) so new contributors understand the usage model before cloning or distributing copies.
3. When you publish release archives or container images, include `LICENSE.md` alongside the binaries and configuration templates.

All contributors, including the original author, operate under Joakim Snökvist’s ownership of the project; no additional redistribution or commercial rights exist beyond those explicitly granted in the personal-use license.

#### Why not adopt MIT/Apache or another stock license?

- **No OSI-approved license fits.** Popular permissive licenses such as MIT, BSD, or Apache explicitly allow redistribution and commercial reuse, so they cannot express the personal-use-only requirement.
- **Noncommercial templates still diverge.** Licenses like the PolyForm Noncommercial family are closer in spirit, but they typically allow wider noncommercial redistribution and collaboration than this project permits. Because the Autod license also forbids sharing binaries, source, and configurators without written approval, retaining the bespoke text keeps the restrictions unambiguous.
- **Custom terms match the owner’s intent.** The current license already aligns with Joakim Snökvist’s directive (personal experimentation only unless approval is granted). Keeping the tailored language avoids accidentally granting rights the owner does not wish to confer.

### File-level headers

Each source, script, and HTML asset should begin with a short notice that points back to the personal-use license. Examples:

```c
/*
 * autod – Autod Personal Use License
 * Copyright (c) 2025 Joakim Snökvist
 * Licensed for personal, non-commercial use only.
 * Redistribution or commercial use requires prior written approval from Joakim Snökvist.
 * See LICENSE.md for full terms.
 */
```

```sh
# autod – Autod Personal Use License
# Copyright (c) 2025 Joakim Snökvist
# Personal, non-commercial use only. Redistribution or commercial use requires
# prior written approval from Joakim Snökvist. See LICENSE.md for full terms.
```

```html
<!--
  autod – Autod Personal Use License
  Copyright (c) 2025 Joakim Snökvist
  Personal, non-commercial use only. Redistribution or commercial use requires
  prior written approval from Joakim Snökvist. See LICENSE.md for full terms.
-->
```

Scripts that generate other files (for example HTML minifiers) should also ensure the notice propagates into the output where practical.

Happy hacking!
