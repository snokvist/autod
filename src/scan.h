// scan.h — LAN node scanner (standalone module)
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SCAN_MAX_NODES
#define SCAN_MAX_NODES 64
#endif

typedef struct {
    char   ip[16];      // "192.168.1.23"
    int    port;        // autod port
    char   role[64];
    char   device[64];
    char   version[32];
    double last_seen;   // time(NULL)

    // ---- New (non-breaking) QoL fields at end ----
    unsigned seen_scan; // internal: last scan sequence this node was seen in
    unsigned misses;    // consecutive scans not seen
    unsigned is_self;   // 1 if local interface; never pruned
} scan_node_t;

typedef struct {
    int      scanning;        // 0/1
    unsigned targets;         // planned targets this scan
    unsigned done;            // completed probes
    int      progress_pct;    // 0..100
    double   last_started;    // time(NULL) or 0
    double   last_finished;   // time(NULL) or 0
} scan_status_t;

#ifndef SCAN_MAX_EXTRA_SUBNETS
#define SCAN_MAX_EXTRA_SUBNETS 16
#endif

typedef struct {
    uint32_t network; // host-order IPv4 network address
    uint32_t netmask; // host-order IPv4 netmask
} scan_extra_subnet_t;

typedef struct {
    int  port;
    char role[64];
    char device[64];
    char version[32];
    scan_extra_subnet_t extra_subnets[SCAN_MAX_EXTRA_SUBNETS];
    unsigned            extra_subnet_count;
} scan_config_t;

// Optional tuning (call once at startup if you want to override defaults)
typedef struct {
    int      connect_timeout_ms; // default 200
    int      health_timeout_ms;  // default 150
    int      caps_timeout_ms;    // default 400
    unsigned concurrency;        // default 16 (workers)
    unsigned stale_max_misses;   // default 2 (prune if unseen for N scans)
} scan_tuning_t;

// Initialize internal structures (idempotent).
void scan_init(void);

// Optionally override timeouts / concurrency / stale policy.
void scan_set_tuning(const scan_tuning_t *t);

// Clear cache entirely.
void scan_reset_nodes(void);

// Add "self" IPs to the cache (one entry per non-loopback IPv4).
// Uses cfg.role/device/version for labels, and cfg.port for the port.
// Self nodes are never pruned.
void scan_seed_self_nodes(const scan_config_t *cfg);

// Start an async subnet scan for the current host's interfaces.
// Returns: 0 = started, 1 = already running, -1 = error.
int  scan_start_async(const scan_config_t *cfg);

// Is a scan currently running? (1/0)
int  scan_is_running(void);

// Fill status snapshot (safe to call anytime).
void scan_get_status(scan_status_t *st);

// Copy up to max nodes into dst; returns count copied.
int  scan_get_nodes(scan_node_t *dst, int max);

#ifdef __cplusplus
}
#endif
