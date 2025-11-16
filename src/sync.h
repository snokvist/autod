#ifndef AUTOD_SYNC_H
#define AUTOD_SYNC_H

#include <pthread.h>
#include <stddef.h>

#include "parson.h"

#define SYNC_MAX_SLOTS 10
#define SYNC_SLOT_MAX_COMMANDS 16
#define SYNC_MAX_SLAVES 64

typedef struct {
    char name[64];
    char prefer_id[64];
    int command_count;
    struct { char json[512]; } commands[SYNC_SLOT_MAX_COMMANDS];
} sync_slot_config_t;

typedef struct {
    int in_use;
    char id[64];
    char remote_ip[64];
    char announced_address[256];
    char device[64];
    char role[64];
    char version[32];
    char caps[256];
    long long last_seen_ms;
    int slot_index;
    int last_ack_generation;
} sync_slave_record_t;

typedef struct {
    pthread_mutex_t lock;
    sync_slave_record_t records[SYNC_MAX_SLAVES];
    int slot_generation[SYNC_MAX_SLOTS];
    char slot_assignees[SYNC_MAX_SLOTS][64];
    unsigned char slot_manual_overrides[SYNC_MAX_SLOTS];
} sync_master_state_t;

typedef struct {
    pthread_mutex_t lock;
    pthread_t thread;
    int running;
    int stop;
    int applied_generation;
    int last_received_generation;
    int current_slot;
    char current_slot_label[64];
} sync_slave_state_t;

typedef struct config config_t;
typedef struct app app_t;
struct mg_context;
struct mg_connection;

void sync_cfg_defaults(config_t *cfg);
int sync_cfg_parse(config_t *cfg, const char *section, const char *key, const char *value);
void sync_ensure_id(config_t *cfg);

void sync_caps_from_json_value(const JSON_Value *value, char *dest, size_t dest_sz);
int sync_preferred_slot_for_id(const config_t *cfg, const char *id);

void sync_master_state_init(sync_master_state_t *state);
void sync_slave_state_init(sync_slave_state_t *state);
void sync_slave_reset_tracking(sync_slave_state_t *state);

void sync_slave_set_applied_generation(sync_slave_state_t *state, int generation);
int sync_slave_get_applied_generation(sync_slave_state_t *state);
void sync_slave_set_last_received(sync_slave_state_t *state, int generation);
int sync_slave_get_last_received(sync_slave_state_t *state);
void sync_slave_set_current_slot(sync_slave_state_t *state, int slot, const char *label);
int sync_slave_get_current_slot(sync_slave_state_t *state);
void sync_slave_get_current_slot_label(sync_slave_state_t *state, char *out, size_t out_sz);

void sync_append_capabilities(const config_t *cfg, JSON_Array *caps_arr);
JSON_Value *sync_build_status_json(const config_t *cfg, sync_slave_state_t *state);

void sync_register_http_handlers(struct mg_context *ctx, app_t *app);
int sync_slave_start_thread(app_t *app);
void sync_slave_stop_thread(sync_slave_state_t *state);

#endif
