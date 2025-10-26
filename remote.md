# Remote Control Workflow Overview

## System Context
The remote-control stack brings together three C utilities that ship with this repository:

1. **autod web UI / API backend** – a CivetWeb-powered HTTP control plane written in C. It exposes discovery, execution, UDP relay, and helper-management endpoints while optionally serving the bundled HTML UI. Operators use it to supervise joystick2crfs and ip2uart, persist configuration, and surface telemetry through Server-Sent Events.
2. **joystick2crfs** – an SDL2-based utility that samples a USB HID controller at up to 250 Hz, maps inputs to 16 CRSF channels, and outputs frames over UART or UDP. Runtime behaviour is defined in `/etc/joystick2crfs.conf`.
3. **ip2uart** – a UART↔IP bridge with TCP server/client and UDP peer modes. It can coalesce UDP packets, maintain reconnect loops, and reload settings from `/etc/ip2uart.conf` on `SIGHUP`.

Together they keep CRSF as the control language while IP networking provides transport flexibility.

## Technology Choices and Trade-offs
| Component | Key Technologies | Advantages | Considerations |
|-----------|------------------|------------|----------------|
| autod backend | C daemon embedding CivetWeb; optional SDL2 GUI overlay; INI configs under `configs/` | Minimal dependencies, suitable for SBCs, ships with HTML UI assets | Lacks built-in authentication; deployments should place it behind a trusted network |
| joystick2crfs | C + SDL2 joystick stack; configurable CRSF packer | Precise timing (250 Hz loop), flexible mapping/inversion, UDP streaming and SSE telemetry | Requires SDL2 runtime and accurate controller database; needs calibration for each device |
| ip2uart | C bridge with epoll loop, TCP/UDP/stdio back-ends | Handles blocking UARTs via ring buffers, supports cadence health stats, reloadable config | Only one TCP client at a time in server mode; UART performance depends on adapter quality |
| Controllers (e.g., Xbox Series pads) | USB HID over Linux input stack | Affordable, widely available, good ergonomics | Device-specific quirks demand per-axis tuning; drift necessitates deadbands |
| Transport | Wi-Fi or Ethernet between ground and vehicle | Uses standard networking gear; compatible with VPN tunnels | Wi-Fi interference and latency spikes must be mitigated with channel planning |

## Primary Deployment Workflow
Baseline assumption: a Radxa Zero 3 runs the ground station with autod and joystick2crfs, while the RC vehicle carries another Linux SBC with autod and ip2uart. The same binaries can be cross-compiled via the provided `Makefile` for other ARM targets, including Android (Termux) with OTG accessories.

1. **Ground Station (Radxa Zero 3)**
   - Build and install the tools with `make install`, or copy `autod`, `joystick2crfs`, and the UI bundle to the device.
   - Connect the USB controller and confirm it registers under `/dev/input/js*` or through SDL2 diagnostics.
   - Populate `/etc/joystick2crfs.conf` with the joystick index, channel map, inversion flags, deadbands, and UART/UDP transport parameters. Enable SSE streaming if the autod UI should show live channels.
   - Launch autod (optionally via `systemd`) so the HTML UI and helper endpoints are available. Configure joystick2crfs to run under autod supervision or as a companion service.

2. **Vehicle Node**
   - Deploy autod alongside `ip2uart` (from `make tools` or `make install`).
   - Wire the UART from the flight controller or ExpressLRS module and record its device path (for example `/dev/ttyUSB0`).
   - Edit `/etc/ip2uart.conf` with the chosen network mode (`tcp_server`, `tcp_client`, or `udp_peer`), baud rate (commonly 420000 for CRSF), and logging cadence. Confirm the daemon can reload the file with `SIGHUP` during tuning.
   - Start autod to host status dashboards and expose control endpoints for remote management.

3. **Link Orchestration**
   - autod’s HTTP API (documented in `README.md`) lets you start/stop helpers, stream telemetry, and push UDP packets. Use it to coordinate joystick2crfs start-up once a controller is detected and to monitor ip2uart statistics.
   - Establish an IP path between the nodes (direct Wi-Fi, Ethernet tether, or VPN). When using TCP client mode, ip2uart will automatically redial if the ground link drops.
   - Use autod’s SSE feeds to visualize link health in the browser UI and surface alarms if cadence deviates from the expected rate configured in ip2uart.

## Alternative CRSF Routing Scenarios
ip2uart can operate on either side of the network tunnel, enabling several CRSF workflows beyond direct joystick control.

### A. Ground Station as CRSF Forwarder (ELRS Receiver to IP)
1. Attach an ExpressLRS receiver to the ground station via USB/UART.
2. Configure `/etc/ip2uart.conf` with `uart_backend=tty` pointed at the receiver and `net_mode=tcp_server` or `udp_peer` to stream frames outward.
3. Run autod to expose the helper’s status and, if desired, relay the CRSF stream to other tools through its `/udp` endpoint or SSE announcements.

**Use case:** Bench testing a vehicle or forwarding live CRSF telemetry to a remote simulator while the airborne radio stays offline.

### B. Ground Station Driving an ELRS Transmitter (CRSF to ELRS TX)
1. Generate CRSF frames with joystick2crfs using the calibrated controller profile.
2. Configure ip2uart in transmit mode (UART connected to the ELRS transmitter module) and choose a network transport for the upstream frames.
3. Employ autod to synchronize process lifecycles, adjust channel maps remotely, and log statistics for later analysis.

**Use case:** Replace a handheld transmitter with a networked ground station while retaining ELRS compatibility.

### C. Vehicle Loopback for Diagnostics
1. Run ip2uart directly on the vehicle with `uart_backend=tty` and `net_mode=udp_peer` to mirror CRSF traffic back to the ground station.
2. Capture and inspect the stream with another ip2uart or a UDP consumer to validate timing, packet integrity, and channel values.

**Use case:** Debugging intermittent control issues without modifying the primary control loop.

## Controller Calibration Challenges
Budget USB controllers need careful tuning before they can safely drive CRSF outputs:

- **Axis Mapping:** SDL2 numbering may not match Linux joystick indices. Verify each axis/button using autod’s diagnostics or the `joystick2crfs` console output and adjust the `map[]` array in the config.
- **Deadband and Drift:** Cheap sticks drift; set the `dead[]` entries (typically 2–5%) to avoid unintended motion. Revisit these values after transport or temperature changes.
- **Inversion and Range:** Ensure throttle/tilt axes run in the expected direction by toggling `invert[]`. Calibrate min/max travel so CRSF outputs span 172–1811.
- **Button Debounce:** Some controllers register spurious presses. Use the `arm_toggle` latch and hold timing to prevent accidental arming, and prefer long-press mappings for critical modes.

## Android Porting Notes
The build system already supports ARM cross compilation (`make musl` or `make gnu`). Porting to Android involves:

- Compiling the binaries for ARM64 and running them inside Termux or a similar environment.
- Granting USB permissions for SDL2 joystick access and for UART adapters attached via OTG.
- Keeping autod in the foreground or using Android’s background service allowances to host the CivetWeb HTTP endpoints and static UI.
- Monitoring power draw, as continual 250 Hz polling and Wi-Fi radios can tax phone batteries faster than on a Radxa SBC.

## Summary
autod orchestrates the joystick2crfs and ip2uart helpers so that low-cost controllers, CRSF radios, and IP links interoperate. By leveraging C-based utilities with small footprints, the same workflow can stretch from bench-top testing to field deployments, supporting joystick-driven control, CRSF forwarding, transmitter emulation, and diagnostic loopbacks without switching toolchains.
