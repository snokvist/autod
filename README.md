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
go/                  # Go-based lightweight prototype (see go/README.md)
```

Key executables built from `src/`:

- **`autod`** – HTTP control daemon.
- **`sse_tail`** – Convenience tool that follows Server-Sent Events endpoints.
- **`udp_relay`** – UDP fan-out utility (with optional UART sink) controlled through the daemon/UI.
- **`ip2uart`** – Bidirectional bridge between a UART (real TTY or stdio) and a UDP peer.
- **`antenna_osd`** – MSP/Canvas OSD renderer with configurable telemetry overlays.
- **`joystick2crsf`** – SDL2 joystick bridge that emits CRSF frames and optional SSE telemetry.

---

## 2. Prerequisites

### Native Linux Build

Install a compiler toolchain plus SDL2 headers (required for `joystick2crsf` and therefore the aggregated `make tools` target):

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
| Helper tools (native)  | `make tools` | `./sse_tail`, `./udp_relay`, `./antenna_osd`, `./ip2uart`, `./joystick2crsf`* |
| `joystick2crsf` utility | `make joystick2crsf` | `./joystick2crsf` (requires SDL2; config at `/etc/joystick2crsf.conf`) |
| Musl cross build       | `make musl` | `./autod-musl` (and friends) |
| GNU cross build        | `make gnu`  | `./autod-gnu` (and friends) |
| Clean intermediates    | `make clean` | removes `build/` and produced binaries |

Each flavour drops intermediates under `build/<flavour>/` and strips binaries automatically when the matching `strip` tool is found.

\* `joystick2crsf` depends on SDL2. The `tools` target builds it alongside the other helpers; use the stand-alone `make joystick2crsf` target if you only want to compile the joystick bridge once the SDL2 development headers are present. Cross-toolchain aggregates (`make tools-musl`, `make tools-gnu`) omit the joystick helper because the build requires native SDL2 support.

### Go prototype (experimental)

A slim Go-based reimplementation that only exposes `/health`, `/exec`, `/nodes`, and sync endpoints lives under [`go/`](go/). Build it separately with:

```bash
make autod-lite

# Optional ARMv7 hard-float cross build
make autod-lite-armhf
```

Refer to [`go/README.md`](go/README.md) for configuration and usage examples.

### Installing on Debian/`systemd`

`make install` builds the native daemon and the bundled `udp_relay` and `joystick2crsf` helpers, then stages a simple system-wide layout that targets Debian 11:

- `autod` → `$(PREFIX)/bin/autod` (default prefix `/usr/local`).
- VRX web UI and helpers → `$(PREFIX)/share/autod/vrx/` (`vrx_index.html`, `exec-handler.sh`, `*.msg`).
- Configuration → `/etc/autod/autod.conf` (the shipped version overwrites any existing file so updates land immediately).
- Service unit → `/etc/systemd/system/autod.service` pointing at the installed binary and config, running as `root` so helper scripts can signal privileged daemons.
- `udp_relay` → `$(PREFIX)/bin/udp_relay`.
- `joystick2crsf` → `$(PREFIX)/bin/joystick2crsf`.
- `ip2uart` → build with `make ip2uart` or via the `make tools` aggregate target when you need the UART↔IP bridge.
- `udp_relay` configuration → `/etc/udp_relay/udp_relay.conf` (overwritten in-place during each install).
- `joystick2crsf` configuration → `/etc/joystick2crsf.conf` (overwritten in-place during each install).
- `ip2uart` configuration → `/etc/ip2uart.conf` (not installed automatically; see [Helper Tools](#helper-tools)).
- `udp_relay` service unit → `/etc/systemd/system/udp_relay.service` which runs the helper as `root` for consistent behaviour with the UI bindings.
- `joystick2crsf` service unit → `/etc/systemd/system/joystick2crsf.service` (includes an `ExecReload` that delivers `SIGHUP` so the daemon can re-read its config without a full restart).
- VRX udp_relay UI asset → `$(PREFIX)/share/autod/udp_relay/vrx_udp_relay.html` (the service runs the binary with `--ui` pointing at this file).

During installation on a host with `systemctl` available (and no `DESTDIR`), the recipe stops any running `autod`/`udp_relay`/`joystick2crsf` services, installs the new assets and configuration in place, reloads `systemd`, and starts the services again without enabling them. If you staged into a `DESTDIR`, reload manually once the files land on the target system, then enable the daemon:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now autod
# Optional helper
sudo systemctl enable --now udp_relay
# Optional joystick bridge
sudo systemctl enable --now joystick2crsf
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

### Relaying HTTP requests via the HTTP API

`autod` also exposes a `/http` endpoint for simple HTTP relays. Instead of free-form host/port forwarding, the relay resolves its target from the node cache or sync assignments: provide either a discovered `"node_ip"`, a registered slave `"sync_id"`, or a 1-based sync `"slot"` number (when acting as a master). The handler looks up the node in `/nodes` to obtain the IP and port, then returns the upstream status code, headers, and body encoded as base64.

```bash
curl -X POST http://HOST:PORT/http \
  -H 'Content-Type: application/json' \
  -d '{
        "sync_id": "slave-1",
        "path": "/status",
        "method": "GET",
        "headers": {"Accept": "application/json"},
        "timeout_ms": 4000
      }'
```

To send a body, include either a UTF-8 string in `"body"` or raw bytes in `"body_base64"`; the fields are mutually exclusive. TLS is not supported by this relay—requests with `"tls": true` return an error (`"ssl_disabled"` when `autod` is built with `NO_SSL`). If you need to point at a specific discovered host, supply `"node_ip"` (optionally with `"port"` to assert the cached port matches) instead of `"sync_id"`/`"slot"`.

### Optional LAN Scanner

When `[server] enable_scan = 1`, the daemon seeds itself into the scan database and launches background probing via functions in [`src/scan.c`](src/scan.c). Clients can poll `/nodes` for progress and discovered peers. If you also define one or more `extra_subnet = 10.10.10.0/24` lines inside a `[scan]` section, the scanner will include those CIDR blocks alongside any directly detected interfaces. `/32` entries are treated as single hosts.

### Sync master/slave coordination

`autod` can now coordinate sync slots across a fleet using an HTTP-based control plane. Enable it via the `[sync]` section in `autod.conf`. When slaves register with a master, the master probes the registering IP on its configured port and refreshes the `/nodes` cache so the HTTP relay and node listings stay current:

```ini
[sync]
# role can be "master" to accept slave registrations or "slave" to follow a master.
role = master
# When acting as a slave, point at the master's sync identifier using sync://.
# master_url = sync://autod-master/sync/register
register_interval_s = 30
allow_bind = 1        ; let POST /sync/bind re-point the slave at runtime
# id = custom-node-id ; defaults to the system hostname
# slot_retention_s = 0 ; seconds to keep an idle slot reserved (0 = forever)
```

Masters can advertise up to ten sync slots via `[sync.slotN]` sections. Each slot lists `/exec` payloads (JSON bodies) that run sequentially on the assigned slave whenever a new sync generation is issued:

```ini
[sync.slot1]
name = primary
prefer_id = alpha
exec = {"path": "/usr/local/bin/slot1-prepare"}
exec = {"path": "/usr/local/bin/slot1-finalise", "args": ["--ok"]}
```

Use `prefer_id` when you need deterministic slot ordering. The master still
lets any slave occupy that slot while the preferred ID is offline, but the next
time the matching ID registers it immediately claims the slot. The displaced
slave is auto-assigned to another free slot or falls back to the waiting queue
if all ten slots are busy, which lets you pre-plan layouts without giving up
the dynamic waterfall behavior.

- **Masters** advertise a `sync-master` capability in `/caps`, accept slave registrations at `POST /sync/register`, list known peers via `GET /sync/slaves`, and assign slots with `POST /sync/push`. The handler accepts bodies such as `{"moves": [{"slave_id": "alpha", "slot": 2}]}` to shuffle live assignments. During each heartbeat the master responds with the next slot command sequence (identified by generation) which the slave executes locally via the configured interpreter.
- **Slaves** (advertising `sync-slave`) maintain a background thread that posts to the configured `master_url` every `register_interval_s` seconds. When the value uses the `sync://` scheme the daemon resolves the identifier through the LAN discovery cache before contacting the master. The response includes the assigned slot, optional slot label, and any commands queued for the next generation; the slave runs each command in order and acknowledges completion on subsequent heartbeats. Slaves also expose `POST /sync/bind` so an operator or master can redirect a running node to a new controller without editing disk config—send either `{ "master_id": "sync-master-id" }` or a `master_url` that already uses the `sync://` format so the daemon persists the identifier.

Slot lifecycle highlights:

- Masters keep each slot assignment and registry record pinned to the registering slave ID until the optional `slot_retention_s` timer elapses. The default of `0` means "retain forever" so a slave that reboots or drops offline can reclaim its previous slot as soon as it reconnects. Set a positive retention window if you want the master to free unused slots and purge idle records automatically.
- When more than ten slaves register concurrently the extras receive a `status: "waiting"` response from `POST /sync/register`. They keep heartbeating (and logging the waiting status) until a slot frees up or you manually move another slave away. No `/exec` payloads are issued while a node is waiting.
- `POST /sync/push` accepts slot move requests (`{"moves": [...]}`) to reshuffle assignments. The master increments the affected slot generation whenever an assignment changes, guaranteeing that the slave replays its slot command waterfall the next time it checks in. Moves are processed atomically so swapping or rotating slots across multiple slaves is handled gracefully without race conditions.
- The same handler accepts `{"delete_ids": ["alpha"]}` (or a single `delete_id`) to flush stale registry entries. Deleting an ID clears its slot assignment immediately and removes the cached metadata so a rebooted device can register from scratch without inheriting old state.
- The same handler can now trigger a forced replay without changing assignments by sending `{"replay_slots": [2, 4]}` to bump specific slots or `{"replay_ids": ["alpha"]}` to target a slave ID. Each replay increments the slot generation and resets the slave's acknowledgement so the command stack runs again the moment it reports back. Requests referencing empty slots or unknown IDs are rejected so you immediately know when nothing was replayed.
- `GET /sync/slaves` includes a `slots` array describing each slot's label and
  optional `prefer_id` reservation so dashboards and CLI helpers can surface
  the intended ordering even when a placeholder slave is occupying the slot.

See the master ([`configs/autod.conf`](configs/autod.conf)) and slave ([`configs/slave/autod.conf`](configs/slave/autod.conf)) samples for full examples and the sync handlers in [`src/autod.c`](src/autod.c) for the request/response schema.

Operators can manage those assignments without crafting raw HTTP by using the bundled VRX assets:

- The VRX web console exposes a **Sync slots** card (`html/autod/vrx_index.html`) that polls `GET /sync/slaves`, lists the ten slots plus any waiting slaves, and lets you queue multi-move plans. Once you confirm the moves the UI POSTs `{"moves": [...]}` to `/sync/push`, you can trigger per-slot replays from the same view, and every card now includes a **Flush ID** action that calls `delete_ids` to remove stale entries.
- The `scripts/vrx/exec-handler.sh` wrapper now implements `/sys/sync/status`, `/sys/sync/move`, `/sys/sync/replay`, and `/sys/sync/delete` commands so you can drive the same control plane over `/exec`. The helper proxies those calls to `http://127.0.0.1:55667` by default; override `AUTOD_HTTP_BASE` (or `AUTOD_HTTP_HOST`/`AUTOD_HTTP_PORT`) before launching the daemon if the control plane listens elsewhere.

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

#### Auto-generated controls (get/set contract)

The ESP32/VTX dashboards auto-build their controls from the `/sys/<cap>/help` payload. To participate, expose a `get` command that accepts a single `name` argument (prefer `enum` + `control.kind: "select"`) and a `set` command that accepts a `pair` argument (`key=value`). Pair those with a `settings` array that lists each `key`, `type`, and `control` hint (`toggle`, `range`, `select`, or `text`). When that shape is present, the UI renders a typed control for every setting, auto-fetches current values via `get`, and auto-applies changes through `set` with a small debounce. The firmware env handler (`/sys/fw/*`) now follows this pattern so link mode, SSID, OSD TTY, and WLAN passphrase fields render alongside the video controls without manual wiring; `fw_get` returns raw values via `fw_printenv -n` (no `key=` prefix), SSIDs accept 4-24 alphanumerics (or empty to clear) while passphrases remain 8-24.

### Helper Tools

- `antenna_osd`: build with `make antenna_osd` (or the `musl`/`gnu` variants) to render an MSP/Canvas OSD overlay. It reads `/etc/antenna_osd.conf` by default and accepts an alternate path as its sole positional argument, with a sample config in [`configs/antenna_osd.conf`](configs/antenna_osd.conf). The helper can poll up to two telemetry files, smooth RSSI values across samples, and exposes configurable bar glyphs, headers, and system-message overlays. When both `info_file` and `info_file2` are provided, prefix telemetry keys with `file1:` or `file2:` to choose which source supplies each value. Set `rssi_2_enable=1` with `rssi_2_key=<metric>` to render a second RSSI indicator based on any available telemetry value instead of the previous UDP-only field.

  **Config highlights:**

  - **Telemetry sources.** `info_file`/`info_file2` (or the clearer `telemetry_file`/`telemetry_secondary` aliases) are polled independently; prefix per-metric keys (`signal_strength_key`/`rssi_key`, `stats_mcs_key`/`curr_tx_rate_key`, `stats_bw_key`/`curr_tx_bw_key`, `stats_tx_power_key`/`tx_power_key`, `secondary_rssi_key`/`rssi_2_key`) with `file1:` or `file2:` to pin them to the right file.
  - **Headers and ranges.** `osd_hdr` prefixes the main RSSI line when `rssi_control=0`; when enabled, `rssi_control` swaps in one of the range-specific headers (`rssi_range0_hdr`…`rssi_range5_hdr`) based on the current percentage. `osd_hdr2` appends to the stats row.
  - **Stats line clarity.** `show_stats_line` controls a secondary row: `0` disables it, `1` writes the header only (useful for spacing), `2` displays the MCS/bandwidth/tx-power trio, and `3` adds the temp/CPU prefix before those values.
  - **Layout cues.** `bar_width` sets the bar length; `top`/`bottom` define the thresholds for full/empty bars on the primary indicator, and `top2`/`bottom2` optionally override the scale for the secondary RSSI bar. `start_sym`, `end_sym`, and `empty_sym` customize the glyphs used to bracket and fill the primary bar, while `start_sym2`, `end_sym2`, and `empty_sym2` optionally style the secondary RSSI bar independently.
- `sse_tail`: build with `make tools` and run `./sse_tail http://host:port/path` to observe SSE streams announced in the config.
  Pass `-L` (or `-l N`) to drop stale lines and deliver only the latest N entries per stream in newest-first order; the default
  cap is 20 when this mode is enabled. Use `-t MS` alongside LIFO mode to throttle flushes to at most once every `MS` milliseconds
  (default 1000 ms), which limits outbound bursts to the queue depth even when upstream processes emit thousands of lines per
  second. Supply `-n NAME` to append an identifier to the SSE stream/event names (for example `stdout:worker1`, `stderr:worker1`)
  so clients can distinguish multiple publishers. The cleanest way to capture stdout/stderr from another process is to launch it
  under `sse_tail` using the `--` separator (for example `./sse_tail -p 8080 -n build -- ./long-job.sh`). `sse_tail` will fork the
  target as a child, pipe its stdout/stderr separately, and emit SSE frames tagged with `stdout`, `stderr`, and `status` (or their
  `-n` variants). Clients can then subscribe with `EventSource.addEventListener("stdout", ...)` and
  `addEventListener("stderr", ...)` without adding textual prefixes; if the target is a pipeline or requires shell expansion, wrap
  it with `sh -c '...'` after the `--` so the helper still owns the child process and can deliver both streams in SSE form. Each
  time a client connects (or reconnects) to `/events`, `sse_tail` immediately emits a `status` event describing the helper and
  child process IDs, the child process name, any `-n` label applied to the stream names, and current uptimes/queueing options.
  Send `SIGHUP` to request another snapshot without interrupting the stream. When the child exits, `sse_tail` emits a final
  `status` SSE line with the exit code (plus the child process name and label) and drains any buffered output first. If `sse_tail`
  itself receives `SIGINT`/`SIGTERM`, it flushes pending output, publishes a `status` message describing the received signal
  alongside the process name/label metadata, and then closes client connections; the tracked child inherits the termination
  signal so it does not keep running.
- `udp_relay`: controlled through its own config file [`configs/udp_relay.conf`](configs/udp_relay.conf). When installed system-wide the default path is `/etc/udp_relay/udp_relay.conf`; you can run it directly or via UI bindings. Dest tokens now accept the literal UART tokens (`uart`, `uart1`, …), letting binds forward into up to four configured UART sinks. Populate the matching `uart<n>_*` keys (`uart0_device`, etc.) and add `bind=uart`/`bind=uart1:ip:port` entries so each serial bridge shows up as a regular bind source and can fan out to UDP peers. Outbound packets reuse the daemon's global `src_ip`, so no UART-specific source binding is required.
- `ip2uart`: run `make ip2uart` (or any of the cross variants) to build a bidirectional bridge between a UART and a UDP peer. It uses `/etc/ip2uart.conf` by default; a sample lives in [`configs/ip2uart.conf`](configs/ip2uart.conf). Pass `-c /path/to/conf` to point at a different configuration, use `-v` for once-per-second stats, and send `SIGHUP` to reload the config without dropping the process.
- `joystick2crsf`: build with `make joystick2crsf` to translate an SDL2 joystick into CRSF or MAVLink RC frames. The utility requires SDL2 development headers (`libsdl2-dev` on Debian-based systems) and reads `/etc/joystick2crsf.conf` by default. A documented sample lives in [`configs/joystick2crsf.conf`](configs/joystick2crsf.conf); adjust the UDP target, SSE streaming options, and channel mapping there. Set `protocol=crsf` (default) for the existing 16-channel CRSF frame packer or `protocol=mavlink` to emit MAVLink v2 `RC_CHANNELS_OVERRIDE` messages with the first eight channels scaled to 1000–2000 µs. When using MAVLink, tune `mavlink_sysid`, `mavlink_compid`, `mavlink_target_sysid`, and `mavlink_target_compid` to match your vehicle IDs. The `arm_toggle` key (default `5`) designates the momentary control that latches channel 5 high after a 1 s hold and releases on a short tap. Toggle `use_gamecontroller` to choose between SDL's standardized `SDL_GameController` mappings and the legacy raw joystick layout. Send `SIGHUP` to reload the config without restarting; when `sse_enabled=true` the binary hosts a single-client SSE feed at `sse_bind` + `sse_path`, publishing the latest channel values at 10 Hz.
  Use the `rate` key to cap outbound RC frames to 25, 50, 125, 250, 333, or 500 Hz; even during dense joystick event bursts the scheduler enforces this upper bound so downstream UDP/SSE consumers only receive frames at the configured cadence. When `stats=1`, `joystick2crsf` also prints a once-per-second summary showing how the event-driven loop is behaving: `loop` covers end-to-end iteration time, `wait` tracks how long the thread slept until the nearest event or send deadline, and `wakes` separates event-driven wakeups from timeout-driven ones. The `udp` and `sse` counters report the number of packets emitted in the last window. Combined, these fields help confirm that the loop is using freshly sampled inputs while sleeping efficiently between deadlines.

For a complete walk-through that ties `autod`, `joystick2crsf`, and `ip2uart` together across a ground/vehicle link, read [`remote.md`](remote.md).

#### `ip2uart` configuration keys

The bridge consumes a simple `key=value` configuration file. Important options exposed by [`src/ip2uart.c`](src/ip2uart.c):

| Section | Key | Description |
| ------- | --- | ----------- |
| Selector | `uart_backend` | `tty` opens the TTY listed in `uart_device`, while `stdio` reads stdin and writes stdout (useful for chaining processes). |
| UART | `uart_device`, `uart_baud`, `uart_databits`, `uart_parity`, `uart_stopbits`, `uart_flow` | Standard serial-port parameters when `uart_backend=tty`. |
| UDP peer | `udp_bind_addr`, `udp_bind_port`, `udp_peer_addr`, `udp_peer_port` | Local bind address/port and optional static peer for UDP. If no peer is set the bridge accepts datagrams from any sender and learns the outbound destination from the most recent packet. |
| UDP coalescing | `udp_coalesce_bytes`, `udp_coalesce_idle_ms`, `udp_max_datagram` | Controls the packet-aggregation buffer before flushing to the peer. |
| Telemetry | `crsf_detect` | When set to `1`, the bridge parses UART and UDP traffic for CRSF frames and, with `-v`, prints once-per-second per-direction counts for RC channels, GPS, battery, and other frame types to stderr. |
| Telemetry logging | `crsf_log`, `crsf_log_path`, `crsf_log_rate_ms` | When `crsf_detect=1` and `crsf_log=1`, the latest UART-side CRSF GPS and battery readings are written to `crsf_log_path` (defaults to `/tmp/crsf_log.msg`) in `key=value` format at the configured interval (default 100 ms). |
| CRSF forwarding | `crsf_coalesce` | When `crsf_detect=1`, set to `1` to forward validated CRSF frames through the normal UDP coalescing path (`udp_coalesce_bytes`/`udp_coalesce_idle_ms`) so multiple frames can share a datagram. Default `0` keeps emitting one UDP packet per detected CRSF frame. |
| Buffers | `rx_buf`, `tx_buf` | Size in bytes of receive scratch buffers and the ring buffers used for non-blocking short-write handling. |

The daemon keeps ring buffers for both directions so short writes (for example when a UART blocks) do not stall the network path. Bytes read from the UART accumulate until the coalesce threshold is met or the idle timer expires, at which point a datagram is emitted. Launching ip2uart with `-v` prints once-per-second stats (packets/s, bytes/s, and drop counters) to stderr; enabling `crsf_detect` adds a matching stderr report of detected CRSF frame types, split by UART (outbound) and UDP (inbound) sources, without altering the forwarding path. Leaving `udp_peer_addr` blank tells the bridge to learn the remote endpoint from inbound packets automatically, which keeps UART data flowing without manual configuration.

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
