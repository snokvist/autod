# autod-lite (Go prototype)

This directory contains a lightweight Go implementation of the autod control-plane with only the essentials:

- `/health` for liveness/role reporting.
- `/exec` for local command execution with optional timeouts.
- `/sync/register` for slave registration against a master.
- `/nodes` and `/sync/slots` for master visibility and slot routing.
- Optional multi-subnet probing that polls `/health` across configured CIDRs.

The legacy C implementation remains untouched for comparison and review.

## Building

From the repository root, you can use the top-level `Makefile` targets:

```
# Native build (emits go/autod-lite)
make autod-lite

# Cross-compile for ARMv7 hard-float (emits go/autod-lite-armhf)
make autod-lite-armhf
```

To build manually from this directory:

```
cd go
go build ./cmd/autod-lite

# Cross-compile without the Makefile
GOOS=linux GOARCH=arm GOARM=7 go build -o autod-lite-armhf ./cmd/autod-lite
```

## Configuration

Settings are supplied via a YAML file. See [`example-config.yaml`](./example-config.yaml) for all fields.
Key options:

- `mode`: `master` or `slave`.
- `listen`: HTTP listen address (e.g., `:8080`).
- `advertise`: external `<host>:<port>` that the master should use to reach the node (slaves only).
- `master_url`: master base URL used by slaves for registration.
- `register_interval`: how often a slave re-registers.
- `probe_cidrs` / `probe_interval` / `probe_port`: optional master-side active health polling.
- `slots`: identifiers the node can service.

## Running

Master:

```
cat > /tmp/master.yaml <<'CFG'
mode: master
id: master-1
listen: ":8080"
probe_cidrs:
  - "192.168.1.0/30"
probe_interval: 30s
probe_port: 8080
slots:
  - slot-a
  - slot-b
CFG
./autod-lite -config /tmp/master.yaml
```

Slave:

```
cat > /tmp/slave.yaml <<'CFG'
mode: slave
id: slave-1
listen: ":9090"
advertise: "10.0.0.25:9090"
master_url: "http://10.0.0.10:8080"
slots:
  - slot-a
register_interval: 10s
CFG
./autod-lite -config /tmp/slave.yaml
```

Once the slave registers, the master can assign slots and dispatch commands:

```
# Assign slot-a to slave-1
curl -X PUT http://10.0.0.10:8080/sync/slots/slot-a \
  -H 'content-type: application/json' \
  -d '{"node_id":"slave-1"}'

# Invoke /exec on the slave through the master
curl -X POST http://10.0.0.10:8080/sync/slots/slot-a/exec \
  -H 'content-type: application/json' \
  -d '{"command":"echo","args":["hello"]}'
```

Health and node inventory are reachable at `/health` and `/nodes` respectively.
