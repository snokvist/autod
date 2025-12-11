#ifndef AUTOD_AUTOD_H
#define AUTOD_AUTOD_H

#include <pthread.h>
#include <stddef.h>

#include "parson.h"
#include "scan.h"
#include "sync.h"

struct mg_context;
struct mg_connection;

typedef struct {
    char *body;
    size_t len;
} upload_t;

#define STARTUP_MAX_EXEC 16

typedef struct config {
    int  port;
    char bind_addr[64];
    int  enable_scan;

    char sync_role[16];
    char sync_master_url[256];
    char sync_id[64];
    int  sync_register_interval_s;
    int  sync_allow_bind;
    int  sync_slot_retention_s;
    sync_slot_config_t sync_slots[SYNC_MAX_SLOTS];

    scan_extra_subnet_t extra_subnets[SCAN_MAX_EXTRA_SUBNETS];
    unsigned            extra_subnet_count;

    char interpreter[128];
    int  exec_timeout_ms;
    int  max_output_bytes;

    int  startup_exec_count;
    struct { char json[512]; } startup_exec[STARTUP_MAX_EXEC];

    char device[64];
    char role[64];
    char version[32];
    char caps[256];
    int  include_net_info;

    struct { char name[64]; char url[192]; } sse[16];
    int  sse_count;

    char ui_path[256];
    int  serve_ui;
    int  ui_public;

    char media_dir[256];
    char firmware_dir[256];
} config_t;

typedef struct app {
    config_t cfg;
    config_t base_cfg;
    struct mg_context *ctx;
    pthread_mutex_t cfg_lock;
    JSON_Value *active_overrides;
    int active_override_generation;
    sync_master_state_t master;
    sync_slave_state_t slave;
} app_t;

long long now_ms(void);
int read_body(struct mg_connection *c, upload_t *u);
void send_json(struct mg_connection *c, JSON_Value *v, int code, int cors_public);
void send_plain(struct mg_connection *c, int code, const char *msg, int cors_public);
void app_config_snapshot(app_t *app, config_t *out);
void app_rebuild_config_locked(app_t *app);
void fill_scan_config(const config_t *cfg, scan_config_t *scfg);
int run_exec(const config_t *cfg, const char *path, JSON_Array *args,
             int timeout_ms, int max_bytes, int *rc_out, long long *elapsed_ms,
             char **out_stdout, char **out_stderr);

#endif
