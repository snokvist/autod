#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "civetweb.h"
#include "parson.h"
#include "scan.h"
#include "autod.h"
#include "sync.h"

extern volatile sig_atomic_t g_stop;

typedef struct {
    char host[128];
    int port;
    char path[256];
} http_url_t;

static void sync_trim(char *s) {
    if (!s) return;
    size_t l = strlen(s), i = 0;
    while (i < l && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, l - i + 1);
    l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
}

void sync_cfg_defaults(config_t *cfg) {
    if (!cfg) return;
    cfg->sync_role[0] = '\0';
    cfg->sync_master_url[0] = '\0';
    cfg->sync_id[0] = '\0';
    cfg->sync_register_interval_s = 30;
    cfg->sync_allow_bind = 1;
    cfg->sync_slot_retention_s = 0;
    memset(cfg->sync_slots, 0, sizeof(cfg->sync_slots));
}

int sync_cfg_parse(config_t *cfg, const char *section, const char *key, const char *value) {
    if (!cfg || !section || !key || !value) return 0;
    if (strcmp(section, "sync") == 0) {
        if (!strcmp(key, "role")) {
            strncpy(cfg->sync_role, value, sizeof(cfg->sync_role) - 1);
            cfg->sync_role[sizeof(cfg->sync_role) - 1] = '\0';
        } else if (!strcmp(key, "master_url")) {
            strncpy(cfg->sync_master_url, value, sizeof(cfg->sync_master_url) - 1);
            cfg->sync_master_url[sizeof(cfg->sync_master_url) - 1] = '\0';
        } else if (!strcmp(key, "id")) {
            strncpy(cfg->sync_id, value, sizeof(cfg->sync_id) - 1);
            cfg->sync_id[sizeof(cfg->sync_id) - 1] = '\0';
        } else if (!strcmp(key, "register_interval_s")) {
            cfg->sync_register_interval_s = atoi(value);
        } else if (!strcmp(key, "allow_bind")) {
            cfg->sync_allow_bind = atoi(value);
        } else if (!strcmp(key, "slot_retention_s")) {
            cfg->sync_slot_retention_s = atoi(value);
        }
        return 1;
    }
    if (strncmp(section, "sync.slot", 9) != 0) return 0;

    int slot_index = atoi(section + 9);
    if (slot_index <= 0 || slot_index > SYNC_MAX_SLOTS) {
        fprintf(stderr,
                "WARN: ignoring sync slot section '%s' (index out of range)\n",
                section);
        return 1;
    }

    sync_slot_config_t *slot = &cfg->sync_slots[slot_index - 1];
    if (!strcmp(key, "name")) {
        strncpy(slot->name, value, sizeof(slot->name) - 1);
        slot->name[sizeof(slot->name) - 1] = '\0';
    } else if (!strcmp(key, "prefer_id")) {
        strncpy(slot->prefer_id, value, sizeof(slot->prefer_id) - 1);
        slot->prefer_id[sizeof(slot->prefer_id) - 1] = '\0';
    } else if ((!strcmp(key, "exec") || !strcmp(key, "command"))) {
        if (slot->command_count >= SYNC_SLOT_MAX_COMMANDS) {
            fprintf(stderr,
                    "WARN: sync slot %d command capacity reached (%d)\n",
                    slot_index, SYNC_SLOT_MAX_COMMANDS);
            return 1;
        }
        JSON_Value *tmp = json_parse_string(value);
        if (!tmp || json_value_get_type(tmp) != JSONObject) {
            fprintf(stderr,
                    "WARN: ignoring invalid sync slot %d command '%s'\n",
                    slot_index, value);
            if (tmp) json_value_free(tmp);
            return 1;
        }
        json_value_free(tmp);
        int idx = slot->command_count++;
        strncpy(slot->commands[idx].json, value,
                sizeof(slot->commands[idx].json) - 1);
        slot->commands[idx].json[sizeof(slot->commands[idx].json) - 1] = '\0';
    }
    return 1;
}

void sync_ensure_id(config_t *cfg) {
    if (!cfg) return;
    if (cfg->sync_id[0]) return;
    char hostbuf[sizeof(cfg->sync_id)];
    if (gethostname(hostbuf, sizeof(hostbuf)) == 0) {
        hostbuf[sizeof(hostbuf) - 1] = '\0';
        strncpy(cfg->sync_id, hostbuf, sizeof(cfg->sync_id) - 1);
        cfg->sync_id[sizeof(cfg->sync_id) - 1] = '\0';
    } else {
        strncpy(cfg->sync_id, "autod-node", sizeof(cfg->sync_id) - 1);
        cfg->sync_id[sizeof(cfg->sync_id) - 1] = '\0';
    }
}

void sync_caps_from_json_value(const JSON_Value *value, char *dest, size_t dest_sz) {
    if (!dest || dest_sz == 0) return;
    dest[0] = '\0';
    if (!value) return;
    JSON_Value_Type t = json_value_get_type(value);
    if (t == JSONString) {
        const char *s = json_value_get_string(value);
        if (s) {
            strncpy(dest, s, dest_sz - 1);
            dest[dest_sz - 1] = '\0';
        }
    } else if (t == JSONArray) {
        JSON_Array *arr = json_value_get_array((JSON_Value *)value);
        size_t cnt = json_array_get_count(arr);
        size_t pos = 0;
        for (size_t i = 0; i < cnt; i++) {
            const char *s = json_array_get_string(arr, i);
            if (!s || !*s) continue;
            char tmp[256];
            strncpy(tmp, s, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            sync_trim(tmp);
            if (!*tmp) continue;
            size_t len = strlen(tmp);
            if (len + (pos ? 1 : 0) >= dest_sz - pos) break;
            if (pos) dest[pos++] = ',';
            memcpy(dest + pos, tmp, len);
            pos += len;
            dest[pos] = '\0';
        }
    }
}

int sync_preferred_slot_for_id(const config_t *cfg, const char *id) {
    if (!cfg || !id || !*id) return -1;
    for (int i = 0; i < SYNC_MAX_SLOTS; i++) {
        if (!cfg->sync_slots[i].prefer_id[0]) continue;
        if (strncmp(cfg->sync_slots[i].prefer_id, id,
                    sizeof(cfg->sync_slots[i].prefer_id)) == 0) {
            return i;
        }
    }
    return -1;
}

void sync_slave_state_init(sync_slave_state_t *state) {
    if (!state) return;
    pthread_mutex_init(&state->lock, NULL);
    state->thread = (pthread_t)0;
    state->running = 0;
    state->stop = 0;
    state->applied_generation = 0;
    state->last_received_generation = 0;
    state->current_slot = -1;
    state->current_slot_label[0] = '\0';
}

void sync_master_state_init(sync_master_state_t *state) {
    if (!state) return;
    pthread_mutex_init(&state->lock, NULL);
    memset(state->records, 0, sizeof(state->records));
    memset(state->slot_generation, 0, sizeof(state->slot_generation));
    memset(state->slot_assignees, 0, sizeof(state->slot_assignees));
    memset(state->slot_manual_overrides, 0, sizeof(state->slot_manual_overrides));
}

void sync_slave_reset_tracking(sync_slave_state_t *state) {
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    state->applied_generation = 0;
    state->last_received_generation = 0;
    state->current_slot = -1;
    state->current_slot_label[0] = '\0';
    pthread_mutex_unlock(&state->lock);
}

void sync_slave_set_applied_generation(sync_slave_state_t *state, int generation) {
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    state->applied_generation = generation;
    pthread_mutex_unlock(&state->lock);
}

int sync_slave_get_applied_generation(sync_slave_state_t *state) {
    if (!state) return 0;
    pthread_mutex_lock(&state->lock);
    int v = state->applied_generation;
    pthread_mutex_unlock(&state->lock);
    return v;
}

void sync_slave_set_last_received(sync_slave_state_t *state, int generation) {
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    state->last_received_generation = generation;
    pthread_mutex_unlock(&state->lock);
}

int sync_slave_get_last_received(sync_slave_state_t *state) {
    if (!state) return 0;
    pthread_mutex_lock(&state->lock);
    int v = state->last_received_generation;
    pthread_mutex_unlock(&state->lock);
    return v;
}

void sync_slave_set_current_slot(sync_slave_state_t *state, int slot, const char *label) {
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    state->current_slot = slot;
    if (label && *label) {
        strncpy(state->current_slot_label, label,
                sizeof(state->current_slot_label) - 1);
        state->current_slot_label[sizeof(state->current_slot_label) - 1] = '\0';
    } else {
        state->current_slot_label[0] = '\0';
    }
    pthread_mutex_unlock(&state->lock);
}

int sync_slave_get_current_slot(sync_slave_state_t *state) {
    if (!state) return 0;
    pthread_mutex_lock(&state->lock);
    int slot = state->current_slot;
    pthread_mutex_unlock(&state->lock);
    return slot;
}

void sync_slave_get_current_slot_label(sync_slave_state_t *state, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    strncpy(out, state->current_slot_label, out_sz - 1);
    out[out_sz - 1] = '\0';
    pthread_mutex_unlock(&state->lock);
}

static sync_slave_record_t *sync_master_find_record(sync_master_state_t *state, const char *id, int create) {
    if (!state || !id || !*id) return NULL;
    sync_slave_record_t *slot = NULL;
    for (int i = 0; i < SYNC_MAX_SLAVES; i++) {
        if (state->records[i].in_use && strcmp(state->records[i].id, id) == 0) {
            return &state->records[i];
        }
        if (!state->records[i].in_use && !slot) slot = &state->records[i];
    }
    if (!create || !slot) return NULL;
    memset(slot, 0, sizeof(*slot));
    slot->in_use = 1;
    strncpy(slot->id, id, sizeof(slot->id) - 1);
    slot->id[sizeof(slot->id) - 1] = '\0';
    slot->slot_index = -1;
    slot->last_ack_generation = 0;
    return slot;
}

static int sync_master_mark_slot_generation(sync_master_state_t *state, int slot_index) {
    if (!state || slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) return 0;
    int gen = state->slot_generation[slot_index] + 1;
    if (gen < 1) gen = 1;
    state->slot_generation[slot_index] = gen;
    return gen;
}

static int sync_master_slot_matches(const sync_master_state_t *state,
                                    int slot_index,
                                    const char *id) {
    if (!state || !id || !*id) return 0;
    if (slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) return 0;
    if (!state->slot_assignees[slot_index][0]) return 0;
    return strncmp(state->slot_assignees[slot_index], id,
                   sizeof(state->slot_assignees[slot_index])) == 0;
}

static void sync_master_release_slot_locked(sync_master_state_t *state,
                                            int slot_index) {
    if (!state || slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) return;
    if (!state->slot_assignees[slot_index][0]) return;
    sync_slave_record_t *rec =
        sync_master_find_record(state, state->slot_assignees[slot_index], 0);
    if (rec && rec->slot_index == slot_index) {
        rec->slot_index = -1;
        rec->last_ack_generation = 0;
    }
    state->slot_assignees[slot_index][0] = '\0';
    state->slot_manual_overrides[slot_index] = 0;
    sync_master_mark_slot_generation(state, slot_index);
}

static int sync_master_delete_record_locked(sync_master_state_t *state,
                                            const char *id) {
    if (!state || !id || !*id) return 0;
    sync_slave_record_t *rec = sync_master_find_record(state, id, 0);
    if (!rec || !rec->in_use) return 0;
    if (rec->slot_index >= 0 && rec->slot_index < SYNC_MAX_SLOTS &&
        sync_master_slot_matches(state, rec->slot_index, rec->id)) {
        sync_master_release_slot_locked(state, rec->slot_index);
    }
    memset(rec, 0, sizeof(*rec));
    return 1;
}

static void sync_master_prune_locked(sync_master_state_t *state,
                                     const config_t *cfg) {
    if (!state) return;
    long long retention_ms = 0;
    if (cfg && cfg->sync_slot_retention_s > 0) {
        retention_ms = (long long)cfg->sync_slot_retention_s * 1000LL;
    }
    long long now = retention_ms > 0 ? now_ms() : 0;
    long long cutoff = retention_ms > 0 ? now - retention_ms : 0;

    for (int slot = 0; slot < SYNC_MAX_SLOTS; slot++) {
        if (!state->slot_assignees[slot][0]) continue;
        int release = 0;
        sync_slave_record_t *rec =
            sync_master_find_record(state, state->slot_assignees[slot], 0);
        if (!rec || !rec->in_use) {
            release = 1;
        } else if (retention_ms > 0 && rec->last_seen_ms > 0 &&
                   rec->last_seen_ms < cutoff) {
            release = 1;
        }
        if (release) {
            sync_master_release_slot_locked(state, slot);
        }
    }

    if (retention_ms <= 0) return;

    for (int i = 0; i < SYNC_MAX_SLAVES; i++) {
        sync_slave_record_t *rec = &state->records[i];
        if (!rec->in_use) continue;
        if (rec->slot_index >= 0) continue;
        if (rec->last_seen_ms <= 0) continue;
        if (rec->last_seen_ms < cutoff) {
            memset(rec, 0, sizeof(*rec));
        }
    }
}

static void sync_master_force_slot_replay_locked(sync_master_state_t *state,
                                                 int slot_index) {
    if (!state || slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) return;
    if (!state->slot_assignees[slot_index][0]) return;
    sync_slave_record_t *rec =
        sync_master_find_record(state, state->slot_assignees[slot_index], 0);
    if (rec) {
        rec->last_ack_generation = 0;
    }
    sync_master_mark_slot_generation(state, slot_index);
}

static int sync_master_assign_slot_locked(sync_master_state_t *state,
                                          sync_slave_record_t *rec,
                                          int slot_index,
                                          int preserve_override) {
    if (!state || !rec || slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) {
        return -1;
    }

    unsigned char had_override = 0;
    if (preserve_override && state->slot_manual_overrides[slot_index]) {
        had_override = state->slot_manual_overrides[slot_index];
    }

    if (sync_master_slot_matches(state, slot_index, rec->id)) {
        rec->slot_index = slot_index;
        rec->last_ack_generation = 0;
        if (state->slot_generation[slot_index] <= 0) {
            state->slot_generation[slot_index] = 1;
        }
        if (had_override) {
            state->slot_manual_overrides[slot_index] = had_override;
        }
        return state->slot_generation[slot_index];
    }

    for (int i = 0; i < SYNC_MAX_SLOTS; i++) {
        if (i == slot_index) continue;
        if (sync_master_slot_matches(state, i, rec->id)) {
            state->slot_assignees[i][0] = '\0';
            sync_master_mark_slot_generation(state, i);
        }
    }

    if (state->slot_assignees[slot_index][0]) {
        sync_slave_record_t *prev =
            sync_master_find_record(state, state->slot_assignees[slot_index], 0);
        if (prev && prev->slot_index == slot_index) {
            prev->slot_index = -1;
            prev->last_ack_generation = 0;
        }
    }

    strncpy(state->slot_assignees[slot_index], rec->id,
            sizeof(state->slot_assignees[slot_index]) - 1);
    state->slot_assignees[slot_index][sizeof(state->slot_assignees[slot_index]) - 1] = '\0';
    state->slot_manual_overrides[slot_index] = had_override;
    rec->slot_index = slot_index;
    rec->last_ack_generation = 0;
    return sync_master_mark_slot_generation(state, slot_index);
}

static int sync_master_auto_assign_slot_locked_impl(sync_master_state_t *state,
                                                    sync_slave_record_t *rec,
                                                    const config_t *cfg,
                                                    int forbid_slot) {
    if (!state || !rec) return -1;

    if (rec->slot_index >= 0 && rec->slot_index < SYNC_MAX_SLOTS) {
        if (rec->slot_index == forbid_slot) {
            rec->slot_index = -1;
        } else if (!sync_master_slot_matches(state, rec->slot_index, rec->id)) {
            (void)sync_master_assign_slot_locked(state, rec, rec->slot_index, 1);
            return rec->slot_index;
        } else {
            if (state->slot_generation[rec->slot_index] <= 0) {
                state->slot_generation[rec->slot_index] = 1;
            }
            return rec->slot_index;
        }
    }

    int preferred_slot = sync_preferred_slot_for_id(cfg, rec->id);
    if (preferred_slot >= 0 && preferred_slot < SYNC_MAX_SLOTS &&
        preferred_slot != forbid_slot) {
        if (state->slot_manual_overrides[preferred_slot] &&
            state->slot_assignees[preferred_slot][0] &&
            strcmp(state->slot_assignees[preferred_slot], rec->id) != 0) {
            preferred_slot = -1;
        }
    }
    if (preferred_slot >= 0 && preferred_slot < SYNC_MAX_SLOTS &&
        preferred_slot != forbid_slot) {
        char displaced_id[64];
        displaced_id[0] = '\0';
        if (state->slot_assignees[preferred_slot][0] &&
            strcmp(state->slot_assignees[preferred_slot], rec->id) != 0) {
            strncpy(displaced_id, state->slot_assignees[preferred_slot],
                    sizeof(displaced_id) - 1);
            displaced_id[sizeof(displaced_id) - 1] = '\0';
        }
        (void)sync_master_assign_slot_locked(state, rec, preferred_slot, 1);
        if (displaced_id[0]) {
            sync_slave_record_t *displaced =
                sync_master_find_record(state, displaced_id, 0);
            if (displaced) {
                displaced->slot_index = -1;
                (void)sync_master_auto_assign_slot_locked_impl(
                    state, displaced, cfg, preferred_slot);
            }
        }
        return preferred_slot;
    }

    for (int i = 0; i < SYNC_MAX_SLOTS; i++) {
        if (i == forbid_slot) continue;
        if (sync_master_slot_matches(state, i, rec->id)) {
            (void)sync_master_assign_slot_locked(state, rec, i, 1);
            return i;
        }
    }

    for (int i = 0; i < SYNC_MAX_SLOTS; i++) {
        if (i == forbid_slot) continue;
        if (state->slot_assignees[i][0]) continue;
        (void)sync_master_assign_slot_locked(state, rec, i, 1);
        return i;
    }
    return -1;
}

static int sync_master_auto_assign_slot_locked(sync_master_state_t *state,
                                               sync_slave_record_t *rec,
                                               const config_t *cfg) {
    return sync_master_auto_assign_slot_locked_impl(state, rec, cfg, -1);
}

static void sync_master_apply_slot_assignment_locked(sync_master_state_t *state,
                                                     const config_t *cfg,
                                                     int slot_index,
                                                     const char *new_id) {
    if (!state || slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) return;

    const char *current = state->slot_assignees[slot_index];
    int current_has = current && *current;
    int new_has = new_id && *new_id;

    if (current_has && new_has && strcmp(current, new_id) == 0) {
        sync_slave_record_t *rec = sync_master_find_record(state, new_id, 0);
        if (rec) rec->slot_index = slot_index;
        if (state->slot_generation[slot_index] <= 0) {
            state->slot_generation[slot_index] = 1;
        }
        return;
    }

    if (current_has) {
        sync_slave_record_t *rec = sync_master_find_record(state, current, 0);
        if (rec && rec->slot_index == slot_index) {
            rec->slot_index = -1;
            rec->last_ack_generation = 0;
        }
    }

    if (new_has) {
        sync_slave_record_t *rec = sync_master_find_record(state, new_id, 0);
        if (rec) {
            rec->slot_index = slot_index;
            rec->last_ack_generation = 0;
        }
    }

    if (new_has) {
        strncpy(state->slot_assignees[slot_index], new_id,
                sizeof(state->slot_assignees[slot_index]) - 1);
        state->slot_assignees[slot_index][sizeof(state->slot_assignees[slot_index]) - 1] = '\0';
    } else {
        state->slot_assignees[slot_index][0] = '\0';
    }

    state->slot_manual_overrides[slot_index] = 0;
    if (new_has && cfg && cfg->sync_slots[slot_index].prefer_id[0]) {
        if (strncmp(cfg->sync_slots[slot_index].prefer_id, new_id,
                    sizeof(cfg->sync_slots[slot_index].prefer_id)) != 0) {
            state->slot_manual_overrides[slot_index] = 1;
        }
    }

    sync_master_mark_slot_generation(state, slot_index);
}

static JSON_Value *sync_master_build_slot_commands(const config_t *cfg,
                                                   int slot_index) {
    if (!cfg || slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) return NULL;

    JSON_Value *arr_v = json_value_init_array();
    if (!arr_v) return NULL;
    JSON_Array *arr = json_array(arr_v);

    const sync_slot_config_t *slot = &cfg->sync_slots[slot_index];
    for (int i = 0; i < slot->command_count; i++) {
        const char *raw = slot->commands[i].json;
        if (!raw[0]) continue;
        JSON_Value *cmd = json_parse_string(raw);
        if (!cmd || json_value_get_type(cmd) != JSONObject) {
            if (cmd) json_value_free(cmd);
            fprintf(stderr,
                    "WARN: malformed sync slot %d command skipped ('%s')\n",
                    slot_index + 1, raw);
            continue;
        }
        json_array_append_value(arr, cmd);
    }

    return arr_v;
}

static int parse_http_url(const char *url, http_url_t *out) {
    if (!url || !out) return -1;
    memset(out, 0, sizeof(*out));
    const char *p = NULL;
    if (strncmp(url, "http://", 7) == 0) {
        p = url + 7;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;

    const char *colon = memchr(p, ':', host_len);
    if (colon) {
        size_t name_len = (size_t)(colon - p);
        if (name_len == 0 || name_len >= sizeof(out->host)) return -1;
        memcpy(out->host, p, name_len);
        out->host[name_len] = '\0';
        const char *port_str = colon + 1;
        size_t port_len = host_len - name_len - 1;
        if (port_len == 0 || port_len >= 16) return -1;
        char tmp[16];
        memcpy(tmp, port_str, port_len);
        tmp[port_len] = '\0';
        out->port = atoi(tmp);
        if (out->port <= 0 || out->port > 65535) return -1;
    } else {
        memcpy(out->host, p, host_len);
        out->host[host_len] = '\0';
        out->port = 80;
    }

    if (slash && *(slash) != '\0') {
        strncpy(out->path, slash, sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
    } else {
        strncpy(out->path, "/sync/register", sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
    }

    if (!out->path[0]) {
        strncpy(out->path, "/sync/register", sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
    }
    return 0;
}

static int parse_sync_reference(const char *ref, char *id_out, size_t id_sz,
                                char *path_out, size_t path_sz) {
    if (!ref || !*ref || !id_out || id_sz == 0) return -1;

    const char *cursor = ref;
    if (strncasecmp(ref, "sync://", 7) == 0) {
        cursor = ref + 7;
    } else if (strstr(ref, "://")) {
        // treat other schemes as unsupported for sync references
        return -1;
    }

    const char *slash = strchr(cursor, '/');
    size_t id_len = slash ? (size_t)(slash - cursor) : strlen(cursor);
    if (id_len == 0 || id_len >= id_sz) return -1;

    memcpy(id_out, cursor, id_len);
    id_out[id_len] = '\0';

    if (path_out && path_sz > 0) {
        if (slash && *(slash)) {
            strncpy(path_out, slash, path_sz - 1);
            path_out[path_sz - 1] = '\0';
        } else {
            strncpy(path_out, "/sync/register", path_sz - 1);
            path_out[path_sz - 1] = '\0';
        }
    }
    return 0;
}

static int sync_normalize_master_reference(const char *value, char *out, size_t out_sz) {
    if (!value || !*value || !out || out_sz == 0) return -1;

    char id_buf[64];
    char path_buf[256];
    if (parse_sync_reference(value, id_buf, sizeof(id_buf), path_buf, sizeof(path_buf)) == 0) {
        int w = snprintf(out, out_sz, "sync://%s%s", id_buf, path_buf);
        if (w < 0 || (size_t)w >= out_sz) return -1;
        return 0;
    }

    http_url_t parsed;
    if (parse_http_url(value, &parsed) != 0) return -1;

    char host_candidates[8][16];
    int candidate_count = 0;

    struct in_addr ip4;
    if (inet_pton(AF_INET, parsed.host, &ip4) == 1) {
        strncpy(host_candidates[candidate_count], parsed.host,
                sizeof(host_candidates[candidate_count]) - 1);
        host_candidates[candidate_count][sizeof(host_candidates[candidate_count]) - 1] = '\0';
        candidate_count = 1;
    } else {
        struct addrinfo hints; memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        struct addrinfo *res = NULL;
        if (getaddrinfo(parsed.host, NULL, &hints, &res) == 0) {
            for (struct addrinfo *ai = res; ai && candidate_count < 8; ai = ai->ai_next) {
                if (!ai->ai_addr || ai->ai_addr->sa_family != AF_INET) continue;
                const char *ntop = inet_ntop(AF_INET,
                                             &((struct sockaddr_in *)ai->ai_addr)->sin_addr,
                                             host_candidates[candidate_count],
                                             sizeof(host_candidates[candidate_count]));
                if (ntop) {
                    host_candidates[candidate_count][sizeof(host_candidates[candidate_count]) - 1] = '\0';
                    candidate_count++;
                }
            }
            freeaddrinfo(res);
        }
    }

    if (candidate_count == 0) {
        strncpy(host_candidates[0], parsed.host, sizeof(host_candidates[0]) - 1);
        host_candidates[0][sizeof(host_candidates[0]) - 1] = '\0';
        candidate_count = 1;
    }

    scan_node_t nodes[SCAN_MAX_NODES];
    int count = scan_get_nodes(nodes, SCAN_MAX_NODES);
    const char *matched_id = NULL;
    for (int i = 0; i < count && !matched_id; i++) {
        scan_node_t *node = &nodes[i];
        if (!node->sync_id[0]) continue;
        if (parsed.port > 0 && node->port != parsed.port) continue;
        for (int j = 0; j < candidate_count; j++) {
            if (strcmp(node->ip, host_candidates[j]) == 0) {
                matched_id = node->sync_id;
                break;
            }
        }
    }

    if (!matched_id) return -1;

    const char *path = parsed.path[0] ? parsed.path : "/sync/register";
    int w = snprintf(out, out_sz, "sync://%s%s", matched_id, path);
    if (w < 0 || (size_t)w >= out_sz) return -1;
    return 0;
}

static int http_post_json_simple(const http_url_t *url, const char *body,
                                 char **resp_body, size_t *resp_len,
                                 int timeout_ms) {
    if (!url) return -1;
    if (resp_body) *resp_body = NULL;
    if (resp_len) *resp_len = 0;

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", url->port > 0 ? url->port : 80);

    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(url->host, portbuf, &hints, &res);
    if (gai != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (timeout_ms > 0) {
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -1;

    size_t body_len = body ? strlen(body) : 0;
    char header[512];
    int header_len = snprintf(header, sizeof(header),
                              "POST %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "Content-Type: application/json\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n\r\n",
                              url->path[0] ? url->path : "/",
                              url->host,
                              body_len);
    if (header_len <= 0 || header_len >= (int)sizeof(header)) {
        close(fd);
        return -1;
    }

    ssize_t sent = send(fd, header, (size_t)header_len, 0);
    if (sent != header_len) {
        close(fd);
        return -1;
    }
    if (body_len > 0) {
        ssize_t bsent = send(fd, body, body_len, 0);
        if (bsent != (ssize_t)body_len) {
            close(fd);
            return -1;
        }
    }

    char *buffer = NULL;
    size_t total = 0;
    const size_t max_resp = 65536;
    char tmpbuf[1024];
    for (;;) {
        ssize_t r = recv(fd, tmpbuf, sizeof(tmpbuf), 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            free(buffer);
            close(fd);
            return -1;
        }
        if (r == 0) break;
        if (total + (size_t)r > max_resp) {
            free(buffer);
            close(fd);
            return -1;
        }
        char *nbuf = (char *)realloc(buffer, total + (size_t)r + 1);
        if (!nbuf) {
            free(buffer);
            close(fd);
            return -1;
        }
        buffer = nbuf;
        memcpy(buffer + total, tmpbuf, (size_t)r);
        total += (size_t)r;
        buffer[total] = '\0';
    }
    close(fd);

    if (!buffer) return -1;

    char *line_end = strstr(buffer, "\r\n");
    if (!line_end) { free(buffer); return -1; }
    int status = 0;
    sscanf(buffer, "HTTP/%*s %d", &status);

    char *body_start = strstr(buffer, "\r\n\r\n");
    if (!body_start) body_start = line_end;
    if (body_start) {
        body_start += 4;
    } else {
        body_start = buffer;
    }
    size_t body_size = total - (size_t)(body_start - buffer);
    char *body_copy = (char *)malloc(body_size + 1);
    if (!body_copy) {
        free(buffer);
        return -1;
    }
    memcpy(body_copy, body_start, body_size);
    body_copy[body_size] = '\0';

    if (resp_body) *resp_body = body_copy;
    else free(body_copy);
    if (resp_len) *resp_len = body_size;

    free(buffer);
    return status;
}

static int sync_slave_resolve_target(app_t *app, const config_t *cfg,
                                     http_url_t *target, char *resolved_id,
                                     size_t resolved_sz) {
    if (!cfg || !target) return -1;

    if (parse_http_url(cfg->sync_master_url, target) == 0) {
        if (resolved_id && resolved_sz > 0) {
            resolved_id[0] = '\0';
        }
        return 0;
    }

    char sync_id[64];
    char path[256];
    if (parse_sync_reference(cfg->sync_master_url, sync_id, sizeof(sync_id),
                             path, sizeof(path)) != 0) {
        return -1;
    }

    scan_node_t nodes[SCAN_MAX_NODES];
    int count = scan_get_nodes(nodes, SCAN_MAX_NODES);
    for (int i = 0; i < count; i++) {
        scan_node_t *node = &nodes[i];
        if (!node->sync_id[0]) continue;
        if (strcasecmp(node->sync_id, sync_id) != 0) continue;

        memset(target, 0, sizeof(*target));
        strncpy(target->host, node->ip, sizeof(target->host) - 1);
        target->host[sizeof(target->host) - 1] = '\0';
        target->port = node->port > 0 ? node->port : 80;
        strncpy(target->path, path, sizeof(target->path) - 1);
        target->path[sizeof(target->path) - 1] = '\0';
        if (!target->path[0]) {
            strncpy(target->path, "/sync/register", sizeof(target->path) - 1);
            target->path[sizeof(target->path) - 1] = '\0';
        }
        if (resolved_id && resolved_sz > 0) {
            strncpy(resolved_id, sync_id, resolved_sz - 1);
            resolved_id[resolved_sz - 1] = '\0';
        }
        return 0;
    }

    (void)app;
    return -1;
}

static int sync_slave_run_slot_commands(app_t *app, JSON_Array *commands,
                                        int slot_number) {
    if (!app) return -1;
    if (!commands) return 0;

    size_t count = json_array_get_count(commands);
    if (count == 0) return 0;

    config_t cfg; app_config_snapshot(app, &cfg);
    for (size_t i = 0; i < count; i++) {
        JSON_Object *cmd = json_array_get_object(commands, i);
        if (!cmd) {
            fprintf(stderr,
                    "sync slave: slot %d command %zu missing payload\n",
                    slot_number, i + 1);
            return -1;
        }
        const char *path = json_object_get_string(cmd, "path");
        if (!path || !*path) {
            fprintf(stderr,
                    "sync slave: slot %d command %zu missing path\n",
                    slot_number, i + 1);
            return -1;
        }
        JSON_Array *args = json_object_get_array(cmd, "args");
        int rc = 0;
        long long elapsed = 0;
        char *out = NULL;
        char *err = NULL;
        int exec_r = run_exec(&cfg, path, args, cfg.exec_timeout_ms,
                              cfg.max_output_bytes, &rc, &elapsed, &out, &err);
        if (exec_r != 0) {
            fprintf(stderr,
                    "sync slave: slot %d command %zu failed to execute '%s'\n",
                    slot_number, i + 1, path);
            if (out) free(out);
            if (err) free(err);
            return -1;
        }
        fprintf(stderr,
                "sync slave: slot %d command %zu rc=%d elapsed=%lldms\n",
                slot_number, i + 1, rc, elapsed);
        if (out) free(out);
        if (err) free(err);
    }
    return 0;
}

static void *sync_slave_thread_main(void *arg) {
    app_t *app = (app_t *)arg;
    int sleep_seconds = 5;
    char last_log_id[64] = "";
    char last_log_host[128] = "";
    int last_log_port = 0;
    char last_log_path[256] = "";
    char last_resolve_error[256] = "";
    int last_slot_reported = 0;
    char last_slot_label[64] = "";
    int last_waiting_notice = 0;
    while (!app->slave.stop && !g_stop) {
        config_t cfg; app_config_snapshot(app, &cfg);
        if (strcasecmp(cfg.sync_role, "slave") != 0) {
            sleep(2);
            continue;
        }
        if (!cfg.sync_master_url[0]) {
            sleep(5);
            continue;
        }

        http_url_t target;
        char resolved_id[64];
        if (sync_slave_resolve_target(app, &cfg, &target, resolved_id,
                                      sizeof(resolved_id)) != 0) {
            if (strcmp(last_resolve_error, cfg.sync_master_url) != 0) {
                fprintf(stderr,
                        "sync slave: unable to resolve master reference '%s'\n",
                        cfg.sync_master_url);
                strncpy(last_resolve_error, cfg.sync_master_url,
                        sizeof(last_resolve_error) - 1);
                last_resolve_error[sizeof(last_resolve_error) - 1] = '\0';
            }
            if (cfg.enable_scan) {
                scan_config_t scfg; fill_scan_config(&cfg, &scfg);
                (void)scan_start_async(&scfg);
            }
            sleep(5);
            continue;
        }

        last_resolve_error[0] = '\0';

        if (resolved_id[0]) {
            if (strcmp(last_log_id, resolved_id) != 0 ||
                strcmp(last_log_host, target.host) != 0 ||
                last_log_port != target.port ||
                strcmp(last_log_path, target.path) != 0) {
                fprintf(stderr,
                        "sync slave: resolved master_id '%s' to %s:%d%s\n",
                        resolved_id, target.host, target.port,
                        target.path[0] ? target.path : "/sync/register");
                strncpy(last_log_id, resolved_id, sizeof(last_log_id) - 1);
                last_log_id[sizeof(last_log_id) - 1] = '\0';
                strncpy(last_log_host, target.host, sizeof(last_log_host) - 1);
                last_log_host[sizeof(last_log_host) - 1] = '\0';
                last_log_port = target.port;
                strncpy(last_log_path, target.path, sizeof(last_log_path) - 1);
                last_log_path[sizeof(last_log_path) - 1] = '\0';
            }
        }

        JSON_Value *req = json_value_init_object();
        JSON_Object *obj = json_object(req);
        json_object_set_string(obj, "id", cfg.sync_id);
        if (cfg.device[0]) json_object_set_string(obj, "device", cfg.device);
        if (cfg.role[0]) json_object_set_string(obj, "role", cfg.role);
        if (cfg.version[0]) json_object_set_string(obj, "version", cfg.version);
        if (cfg.caps[0]) {
            JSON_Value *caps = json_value_init_array();
            JSON_Array *arr = json_array(caps);
            char tmp[256]; strncpy(tmp, cfg.caps, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
            char *tok, *save = NULL;
            for (tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
                sync_trim(tok);
                if (*tok) json_array_append_string(arr, tok);
            }
            json_object_set_value(obj, "caps", caps);
        }
        json_object_set_number(obj, "ack_generation", sync_slave_get_applied_generation(&app->slave));

        char *body = json_serialize_to_string(req);
        json_value_free(req);
        if (!body) {
            sleep(5);
            continue;
        }

        char *resp_body = NULL;
        int timeout_ms = cfg.sync_register_interval_s > 0 ? cfg.sync_register_interval_s * 1000 : 5000;
        int http_status = http_post_json_simple(&target, body, &resp_body, NULL, timeout_ms);
        json_free_serialized_string(body);

        if (http_status != 200 || !resp_body) {
            if (resp_body) free(resp_body);
            sleep(5);
            continue;
        }

        JSON_Value *resp = json_parse_string(resp_body);
        free(resp_body);
        if (!resp) {
            sleep(5);
            continue;
        }

        JSON_Object *ro = json_object(resp);
        int generation = 0;
        JSON_Value *gen_v = json_object_get_value(ro, "generation");
        if (gen_v && json_value_get_type(gen_v) == JSONNumber) {
            generation = (int)json_value_get_number(gen_v);
        }
        const char *status = json_object_get_string(ro, "status");
        int waiting_status = (status && strcasecmp(status, "waiting") == 0);
        if (waiting_status) {
            if (!last_waiting_notice) {
                fprintf(stderr, "sync slave: waiting for master slot\n");
                last_waiting_notice = 1;
            }
        } else if (last_waiting_notice) {
            last_waiting_notice = 0;
        }
        int slot_number = 0;
        JSON_Value *slot_v = json_object_get_value(ro, "slot");
        if (slot_v && json_value_get_type(slot_v) == JSONNumber) {
            slot_number = (int)json_value_get_number(slot_v);
            if (slot_number < 0) slot_number = 0;
        }
        const char *slot_label = json_object_get_string(ro, "slot_label");
        const char *label_checked = (slot_label && *slot_label) ? slot_label : "";
        if (slot_number != last_slot_reported ||
            strcmp(label_checked, last_slot_label) != 0) {
            if (slot_number > 0) {
                fprintf(stderr, "sync slave: assigned to slot %d", slot_number);
                if (*label_checked) {
                    fprintf(stderr, " (%s)", label_checked);
                }
                fprintf(stderr, "\n");
            } else {
                fprintf(stderr, "sync slave: slot assignment cleared\n");
            }
            last_slot_reported = slot_number;
            strncpy(last_slot_label, label_checked, sizeof(last_slot_label) - 1);
            last_slot_label[sizeof(last_slot_label) - 1] = '\0';
        }
        sync_slave_set_current_slot(&app->slave, slot_number, label_checked);

        JSON_Array *commands_arr = NULL;
        JSON_Value *commands_v = json_object_get_value(ro, "commands");
        if (commands_v && json_value_get_type(commands_v) == JSONArray) {
            commands_arr = json_value_get_array(commands_v);
        }
        if (generation > 0) {
            int exec_res = 0;
            if (commands_arr) {
                exec_res = sync_slave_run_slot_commands(app, commands_arr,
                                                        slot_number);
            }
            if (!commands_arr) {
                exec_res = 0;
            }
            if (exec_res == 0) {
                sync_slave_set_last_received(&app->slave, generation);
                sync_slave_set_applied_generation(&app->slave, generation);
                pthread_mutex_lock(&app->cfg_lock);
                app->active_override_generation = generation;
                pthread_mutex_unlock(&app->cfg_lock);
            } else {
                fprintf(stderr,
                        "sync slave: failed to execute slot %d commands for generation %d\n",
                        slot_number, generation);
            }
        }
        json_value_free(resp);

        sleep_seconds = cfg.sync_register_interval_s > 0 ? cfg.sync_register_interval_s : 15;
        for (int i = 0; i < sleep_seconds && !app->slave.stop && !g_stop; i++) {
            sleep(1);
        }
    }

    pthread_mutex_lock(&app->slave.lock);
    app->slave.running = 0;
    pthread_mutex_unlock(&app->slave.lock);
    return NULL;
}

static int h_sync_register(struct mg_connection *c, void *ud) {
    app_t *app = (app_t *)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    if (strcasecmp(cfg.sync_role, "master") != 0) {
        send_plain(c, 404, "not_found", 1);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(c);
    if (!ri || strcmp(ri->request_method, "POST") != 0) {
        send_plain(c, 405, "method_not_allowed", 1);
        return 1;
    }

    upload_t u = {0};
    if (read_body(c, &u) != 0) {
        if (u.body) free(u.body);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "body_read_failed");
        send_json(c, v, 400, 1);
        json_value_free(v);
        return 1;
    }

    JSON_Value *root = json_parse_string(u.body ? u.body : "{}");
    free(u.body);
    if (!root) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "bad_json");
        send_json(c, v, 400, 1);
        json_value_free(v);
        return 1;
    }

    JSON_Object *obj = json_object(root);
    const char *id = json_object_get_string(obj, "id");
    if (!id || !*id) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "missing_id");
        send_json(c, v, 400, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    const char *device = json_object_get_string(obj, "device");
    const char *role = json_object_get_string(obj, "role");
    const char *version = json_object_get_string(obj, "version");
    const char *address = json_object_get_string(obj, "address");
    const char *callback = json_object_get_string(obj, "callback_url");
    const JSON_Value *caps_val = json_object_get_value(obj, "caps");
    int ack_generation = 0;
    JSON_Value *ack_v = json_object_get_value(obj, "ack_generation");
    if (ack_v && json_value_get_type(ack_v) == JSONNumber) {
        ack_generation = (int)json_value_get_number(ack_v);
    }

    int assigned_slot = -1;
    int send_generation = 0;
    int slot_generation = 0;
    char slot_label[64]; slot_label[0] = '\0';

    pthread_mutex_lock(&app->master.lock);
    sync_master_prune_locked(&app->master, &cfg);
    sync_slave_record_t *rec = sync_master_find_record(&app->master, id, 1);
    if (!rec) {
        pthread_mutex_unlock(&app->master.lock);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "registry_full");
        send_json(c, v, 503, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    rec->last_seen_ms = now_ms();
    strncpy(rec->remote_ip, ri->remote_addr, sizeof(rec->remote_ip) - 1);
    rec->remote_ip[sizeof(rec->remote_ip) - 1] = '\0';
    if (address && *address) {
        strncpy(rec->announced_address, address, sizeof(rec->announced_address) - 1);
        rec->announced_address[sizeof(rec->announced_address) - 1] = '\0';
    } else if (callback && *callback) {
        strncpy(rec->announced_address, callback, sizeof(rec->announced_address) - 1);
        rec->announced_address[sizeof(rec->announced_address) - 1] = '\0';
    }
    if (device && *device) {
        strncpy(rec->device, device, sizeof(rec->device) - 1);
        rec->device[sizeof(rec->device) - 1] = '\0';
    }
    if (role && *role) {
        strncpy(rec->role, role, sizeof(rec->role) - 1);
        rec->role[sizeof(rec->role) - 1] = '\0';
    }
    if (version && *version) {
        strncpy(rec->version, version, sizeof(rec->version) - 1);
        rec->version[sizeof(rec->version) - 1] = '\0';
    }
    sync_caps_from_json_value(caps_val, rec->caps, sizeof(rec->caps));

    if (ri->remote_addr[0]) {
        int probe_port = cfg.port > 0 ? cfg.port : 8080;
        (void)scan_probe_node(ri->remote_addr, probe_port);
    }

    int previous_slot = rec->slot_index;
    assigned_slot = sync_master_auto_assign_slot_locked(&app->master, rec, &cfg);
    if (assigned_slot >= 0) {
        slot_generation = app->master.slot_generation[assigned_slot];
        int slot_changed = (previous_slot != assigned_slot);
        /*
         * Slot commands carry a generation value so slaves can avoid
         * re-running commands they have already applied. A slave reports the
         * last generation it executed via ack_generation; if that matches the
         * current slot generation we skip sending commands, otherwise we replay
         * them. Slot changes and out-of-range acknowledgements must reset the
         * tracking so a move to a lower-generation slot still receives its
         * commands.
         */
        if (slot_changed) {
            rec->last_ack_generation = 0;
        } else if (ack_generation > 0) {
            if (ack_generation > slot_generation) {
                rec->last_ack_generation = 0;
            } else if (ack_generation > rec->last_ack_generation) {
                rec->last_ack_generation = ack_generation;
            }
        }
        if (slot_generation > rec->last_ack_generation) {
            send_generation = slot_generation;
        }
        if (cfg.sync_slots[assigned_slot].name[0]) {
            strncpy(slot_label, cfg.sync_slots[assigned_slot].name,
                    sizeof(slot_label) - 1);
            slot_label[sizeof(slot_label) - 1] = '\0';
        }
    }
    pthread_mutex_unlock(&app->master.lock);

    if (assigned_slot < 0) {
        JSON_Value *resp = json_value_init_object();
        JSON_Object *ro = json_object(resp);
        json_object_set_string(ro, "status", "waiting");
        json_object_set_string(ro, "id", id);
        json_object_set_number(ro, "interval_s", cfg.sync_register_interval_s);
        json_object_set_string(ro, "reason", "no_slots_available");
        json_object_set_number(ro, "max_slots", SYNC_MAX_SLOTS);
        json_object_set_null(ro, "slot");
        send_json(c, resp, 200, 1);
        json_value_free(resp);
        json_value_free(root);
        return 1;
    }

    JSON_Value *commands_to_send = NULL;
    if (send_generation > 0) {
        commands_to_send = sync_master_build_slot_commands(&cfg, assigned_slot);
    }

    JSON_Value *resp = json_value_init_object();
    JSON_Object *ro = json_object(resp);
    json_object_set_string(ro, "status", "registered");
    json_object_set_string(ro, "id", id);
    json_object_set_number(ro, "interval_s", cfg.sync_register_interval_s);
    json_object_set_number(ro, "generation", send_generation);
    json_object_set_number(ro, "slot", assigned_slot + 1);
    json_object_set_number(ro, "slot_generation", slot_generation);
    if (slot_label[0]) json_object_set_string(ro, "slot_label", slot_label);
    if (commands_to_send) {
        json_object_set_value(ro, "commands", commands_to_send);
    }

    send_json(c, resp, 200, 1);
    json_value_free(resp);
    json_value_free(root);
    return 1;
}

static int h_sync_slaves(struct mg_connection *c, void *ud) {
    app_t *app = (app_t *)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    if (strcasecmp(cfg.sync_role, "master") != 0) {
        send_plain(c, 404, "not_found", 1);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(c);
    if (!ri || strcmp(ri->request_method, "GET") != 0) {
        send_plain(c, 405, "method_not_allowed", 1);
        return 1;
    }

    JSON_Value *resp = json_value_init_object();
    JSON_Object *ro = json_object(resp);
    JSON_Value *arr_v = json_value_init_array();
    JSON_Array *arr = json_array(arr_v);

    pthread_mutex_lock(&app->master.lock);
    sync_master_prune_locked(&app->master, &cfg);
    for (int i = 0; i < SYNC_MAX_SLAVES; i++) {
        sync_slave_record_t *rec = &app->master.records[i];
        if (!rec->in_use) continue;
        JSON_Value *item = json_value_init_object();
        JSON_Object *io = json_object(item);
        json_object_set_string(io, "id", rec->id);
        json_object_set_string(io, "remote_ip", rec->remote_ip);
        if (rec->announced_address[0]) json_object_set_string(io, "address", rec->announced_address);
        if (rec->device[0]) json_object_set_string(io, "device", rec->device);
        if (rec->role[0]) json_object_set_string(io, "role", rec->role);
        if (rec->version[0]) json_object_set_string(io, "version", rec->version);
        if (rec->caps[0]) json_object_set_string(io, "caps", rec->caps);
        json_object_set_number(io, "last_seen_ms", (double)rec->last_seen_ms);
        json_object_set_number(io, "last_ack_generation", rec->last_ack_generation);
        if (rec->slot_index >= 0 && rec->slot_index < SYNC_MAX_SLOTS) {
            json_object_set_number(io, "slot", rec->slot_index + 1);
            json_object_set_number(io, "slot_generation",
                                   app->master.slot_generation[rec->slot_index]);
            if (cfg.sync_slots[rec->slot_index].name[0]) {
                json_object_set_string(io, "slot_label",
                                       cfg.sync_slots[rec->slot_index].name);
            }
        }
        int preferred_slot = sync_preferred_slot_for_id(&cfg, rec->id);
        if (preferred_slot >= 0) {
            json_object_set_number(io, "preferred_slot", preferred_slot + 1);
        }
        json_array_append_value(arr, item);
    }

    JSON_Value *slots_v = json_value_init_array();
    JSON_Array *slots_arr = json_array(slots_v);
    for (int slot = 0; slot < SYNC_MAX_SLOTS; slot++) {
        JSON_Value *slot_v = json_value_init_object();
        JSON_Object *so = json_object(slot_v);
        json_object_set_number(so, "slot", slot + 1);
        if (cfg.sync_slots[slot].name[0]) {
            json_object_set_string(so, "label", cfg.sync_slots[slot].name);
        }
        if (cfg.sync_slots[slot].prefer_id[0]) {
            json_object_set_string(so, "prefer_id",
                                   cfg.sync_slots[slot].prefer_id);
        }
        if (app->master.slot_assignees[slot][0]) {
            json_object_set_string(so, "assigned_id",
                                   app->master.slot_assignees[slot]);
        }
        json_array_append_value(slots_arr, slot_v);
    }
    pthread_mutex_unlock(&app->master.lock);

    json_object_set_value(ro, "slaves", arr_v);
    json_object_set_value(ro, "slots", slots_v);
    send_json(c, resp, 200, 1);
    json_value_free(resp);
    return 1;
}

static int h_sync_push(struct mg_connection *c, void *ud) {
    app_t *app = (app_t *)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    if (strcasecmp(cfg.sync_role, "master") != 0) {
        send_plain(c, 404, "not_found", 1);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(c);
    if (!ri || strcmp(ri->request_method, "POST") != 0) {
        send_plain(c, 405, "method_not_allowed", 1);
        return 1;
    }

    upload_t u = {0};
    if (read_body(c, &u) != 0) {
        if (u.body) free(u.body);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "body_read_failed");
        send_json(c, v, 400, 1);
        json_value_free(v);
        return 1;
    }

    JSON_Value *root = json_parse_string(u.body ? u.body : "{}");
    free(u.body);
    if (!root) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "bad_json");
        send_json(c, v, 400, 1);
        json_value_free(v);
        return 1;
    }

    JSON_Object *obj = json_object(root);

    typedef struct {
        char id[64];
        int slot_index;
        int has_slot;
    } slot_move_t;

    slot_move_t moves[SYNC_MAX_SLOTS];
    int move_count = 0;

    JSON_Value *moves_v = json_object_get_value(obj, "moves");
    if (moves_v && json_value_get_type(moves_v) == JSONArray) {
        JSON_Array *arr = json_value_get_array(moves_v);
        size_t cnt = json_array_get_count(arr);
        for (size_t i = 0; i < cnt && move_count < SYNC_MAX_SLOTS; i++) {
            JSON_Object *item = json_array_get_object(arr, i);
            if (!item) continue;
            const char *sid = json_object_get_string(item, "slave_id");
            if (!sid || !*sid) sid = json_object_get_string(item, "id");
            JSON_Value *slot_v = json_object_get_value(item, "slot");
            if (!sid || !*sid || !slot_v) continue;
            double slot_num = 0.0;
            int has_slot = 0;
            int slot_index = -1;
            JSON_Value_Type t = json_value_get_type(slot_v);
            if (t == JSONNull) {
                has_slot = 1;
                slot_index = -1;
            } else if (t == JSONNumber) {
                slot_num = json_value_get_number(slot_v);
                int slot_int = (int)slot_num;
                if ((double)slot_int != slot_num) continue;
                if (slot_int <= 0) {
                    slot_index = -1;
                } else if (slot_int > SYNC_MAX_SLOTS) {
                    continue;
                } else {
                    slot_index = slot_int - 1;
                }
                has_slot = 1;
            } else {
                continue;
            }
            strncpy(moves[move_count].id, sid, sizeof(moves[move_count].id) - 1);
            moves[move_count].id[sizeof(moves[move_count].id) - 1] = '\0';
            moves[move_count].slot_index = slot_index;
            moves[move_count].has_slot = has_slot;
            move_count++;
        }
    } else {
        const char *sid = json_object_get_string(obj, "slave_id");
        if (!sid || !*sid) sid = json_object_get_string(obj, "id");
        JSON_Value *slot_v = json_object_get_value(obj, "slot");
        if (sid && *sid && slot_v) {
            double slot_num = 0.0;
            int slot_index = -1;
            int has_slot = 0;
            JSON_Value_Type t = json_value_get_type(slot_v);
            if (t == JSONNull) {
                has_slot = 1;
                slot_index = -1;
            } else if (t == JSONNumber) {
                slot_num = json_value_get_number(slot_v);
                int slot_int = (int)slot_num;
                if ((double)slot_int == slot_num) {
                    if (slot_int <= 0) {
                        slot_index = -1;
                    } else if (slot_int <= SYNC_MAX_SLOTS) {
                        slot_index = slot_int - 1;
                    } else {
                        slot_index = -2;
                    }
                    has_slot = 1;
                }
            }
            if (has_slot && slot_index >= -1) {
                strncpy(moves[0].id, sid, sizeof(moves[0].id) - 1);
                moves[0].id[sizeof(moves[0].id) - 1] = '\0';
                moves[0].slot_index = slot_index;
                moves[0].has_slot = has_slot;
                move_count = 1;
            }
        }
    }

    typedef struct {
        int slot_index;
    } replay_slot_request_t;

    typedef struct {
        char id[64];
    } replay_id_request_t;

    typedef struct {
        char id[64];
    } delete_request_t;

    replay_slot_request_t replay_slot_requests[SYNC_MAX_SLOTS];
    int replay_slot_count = 0;
    replay_id_request_t replay_id_requests[SYNC_MAX_SLOTS];
    int replay_id_count = 0;
    delete_request_t delete_requests[SYNC_MAX_SLAVES];
    int delete_count = 0;
    memset(delete_requests, 0, sizeof(delete_requests));

    JSON_Value *replay_slots_v = json_object_get_value(obj, "replay_slots");
    if (replay_slots_v) {
        JSON_Value_Type t = json_value_get_type(replay_slots_v);
        if (t == JSONArray) {
            JSON_Array *arr = json_value_get_array(replay_slots_v);
            size_t cnt = json_array_get_count(arr);
            for (size_t i = 0; i < cnt && replay_slot_count < SYNC_MAX_SLOTS; i++) {
                JSON_Value *slot_v = json_array_get_value(arr, i);
                if (!slot_v || json_value_get_type(slot_v) != JSONNumber) continue;
                double slot_num = json_value_get_number(slot_v);
                int slot_int = (int)slot_num;
                if ((double)slot_int != slot_num) continue;
                if (slot_int <= 0 || slot_int > SYNC_MAX_SLOTS) continue;
                replay_slot_requests[replay_slot_count++].slot_index = slot_int - 1;
            }
        } else if (t == JSONNumber && replay_slot_count < SYNC_MAX_SLOTS) {
            double slot_num = json_value_get_number(replay_slots_v);
            int slot_int = (int)slot_num;
            if ((double)slot_int == slot_num &&
                slot_int > 0 && slot_int <= SYNC_MAX_SLOTS) {
                replay_slot_requests[replay_slot_count++].slot_index = slot_int - 1;
            }
        }
    }

    JSON_Value *replay_ids_v = json_object_get_value(obj, "replay_ids");
    if (replay_ids_v) {
        JSON_Value_Type t = json_value_get_type(replay_ids_v);
        if (t == JSONArray) {
            JSON_Array *arr = json_value_get_array(replay_ids_v);
            size_t cnt = json_array_get_count(arr);
            for (size_t i = 0; i < cnt && replay_id_count < SYNC_MAX_SLOTS; i++) {
                const char *sid = json_array_get_string(arr, i);
                if (!sid || !*sid) continue;
                strncpy(replay_id_requests[replay_id_count].id, sid,
                        sizeof(replay_id_requests[replay_id_count].id) - 1);
                replay_id_requests[replay_id_count]
                    .id[sizeof(replay_id_requests[replay_id_count].id) - 1] = '\0';
                replay_id_count++;
            }
        } else if (t == JSONString && replay_id_count < SYNC_MAX_SLOTS) {
            const char *sid = json_value_get_string(replay_ids_v);
            if (sid && *sid) {
                strncpy(replay_id_requests[replay_id_count].id, sid,
                        sizeof(replay_id_requests[replay_id_count].id) - 1);
                replay_id_requests[replay_id_count]
                    .id[sizeof(replay_id_requests[replay_id_count].id) - 1] = '\0';
                replay_id_count++;
            }
        }
    }

    JSON_Value *delete_ids_v = json_object_get_value(obj, "delete_ids");
    if (delete_ids_v) {
        JSON_Value_Type t = json_value_get_type(delete_ids_v);
        if (t == JSONArray) {
            JSON_Array *arr = json_value_get_array(delete_ids_v);
            size_t cnt = json_array_get_count(arr);
            for (size_t i = 0; i < cnt && delete_count < SYNC_MAX_SLAVES; i++) {
                const char *sid = json_array_get_string(arr, i);
                if (!sid || !*sid) continue;
                strncpy(delete_requests[delete_count].id, sid,
                        sizeof(delete_requests[delete_count].id) - 1);
                delete_requests[delete_count]
                    .id[sizeof(delete_requests[delete_count].id) - 1] = '\0';
                delete_count++;
            }
        } else if (t == JSONString && delete_count < SYNC_MAX_SLAVES) {
            const char *sid = json_value_get_string(delete_ids_v);
            if (sid && *sid) {
                strncpy(delete_requests[delete_count].id, sid,
                        sizeof(delete_requests[delete_count].id) - 1);
                delete_requests[delete_count]
                    .id[sizeof(delete_requests[delete_count].id) - 1] = '\0';
                delete_count++;
            }
        }
    }
    if (delete_count < SYNC_MAX_SLAVES) {
        const char *single_delete = json_object_get_string(obj, "delete_id");
        if (single_delete && *single_delete) {
            strncpy(delete_requests[delete_count].id, single_delete,
                    sizeof(delete_requests[delete_count].id) - 1);
            delete_requests[delete_count]
                .id[sizeof(delete_requests[delete_count].id) - 1] = '\0';
            delete_count++;
        }
    }

    int has_replay_requests = (replay_slot_count > 0 || replay_id_count > 0);

    if (move_count == 0 && !has_replay_requests && delete_count == 0) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "no_moves_provided");
        send_json(c, v, 400, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    typedef struct {
        int slot;
        char id[64];
        int generation;
    } slot_snapshot_t;

    slot_snapshot_t snapshot[SYNC_MAX_SLOTS];
    int snapshot_count = 0;
    int error_code = 0;
    char error_reason[64]; error_reason[0] = '\0';
    char error_id[64]; error_id[0] = '\0';
    int error_slot = 0;
    int replay_mask[SYNC_MAX_SLOTS];
    memset(replay_mask, 0, sizeof(replay_mask));
    int replayed_slots = 0;

    pthread_mutex_lock(&app->master.lock);
    sync_master_prune_locked(&app->master, &cfg);

    char deleted_ids[SYNC_MAX_SLAVES][64];
    int deleted_count = 0;
    memset(deleted_ids, 0, sizeof(deleted_ids));
    for (int i = 0; i < delete_count; i++) {
        if (!delete_requests[i].id[0]) continue;
        int already_listed = 0;
        for (int j = 0; j < deleted_count; j++) {
            if (strcmp(deleted_ids[j], delete_requests[i].id) == 0) {
                already_listed = 1;
                break;
            }
        }
        if (already_listed) continue;
        if (sync_master_delete_record_locked(&app->master,
                                             delete_requests[i].id)) {
            strncpy(deleted_ids[deleted_count], delete_requests[i].id,
                    sizeof(deleted_ids[deleted_count]) - 1);
            deleted_ids[deleted_count]
                [sizeof(deleted_ids[deleted_count]) - 1] = '\0';
            deleted_count++;
        }
    }

    char planned[SYNC_MAX_SLOTS][64];
    for (int i = 0; i < SYNC_MAX_SLOTS; i++) {
        strncpy(planned[i], app->master.slot_assignees[i], sizeof(planned[i]) - 1);
        planned[i][sizeof(planned[i]) - 1] = '\0';
    }

    for (int i = 0; i < move_count && !error_code; i++) {
        if (!moves[i].has_slot) continue;
        if (moves[i].slot_index >= SYNC_MAX_SLOTS) {
            error_code = 400;
            strncpy(error_reason, "slot_out_of_range", sizeof(error_reason) - 1);
            error_reason[sizeof(error_reason) - 1] = '\0';
            error_slot = moves[i].slot_index + 1;
            break;
        }
        sync_slave_record_t *rec =
            sync_master_find_record(&app->master, moves[i].id, 0);
        if (!rec) {
            error_code = 404;
            strncpy(error_reason, "slave_not_found", sizeof(error_reason) - 1);
            error_reason[sizeof(error_reason) - 1] = '\0';
            strncpy(error_id, moves[i].id, sizeof(error_id) - 1);
            error_id[sizeof(error_id) - 1] = '\0';
            break;
        }
        for (int s = 0; s < SYNC_MAX_SLOTS; s++) {
            if (planned[s][0] && strcmp(planned[s], rec->id) == 0) {
                planned[s][0] = '\0';
            }
        }
        if (moves[i].slot_index >= 0) {
            strncpy(planned[moves[i].slot_index], rec->id,
                    sizeof(planned[moves[i].slot_index]) - 1);
            planned[moves[i].slot_index][sizeof(planned[moves[i].slot_index]) - 1] = '\0';
        }
    }

    if (!error_code) {
        for (int i = 0; i < replay_slot_count; i++) {
            int slot_index = replay_slot_requests[i].slot_index;
            if (slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) continue;
            if (!planned[slot_index][0]) {
                error_code = 409;
                strncpy(error_reason, "slot_unassigned", sizeof(error_reason) - 1);
                error_reason[sizeof(error_reason) - 1] = '\0';
                error_slot = slot_index + 1;
                break;
            }
            replay_mask[slot_index] = 1;
        }
    }

    if (!error_code) {
        for (int i = 0; i < replay_id_count; i++) {
            int found_slot = -1;
            for (int s = 0; s < SYNC_MAX_SLOTS; s++) {
                if (planned[s][0] &&
                    strcmp(planned[s], replay_id_requests[i].id) == 0) {
                    found_slot = s;
                    break;
                }
            }
            if (found_slot < 0) {
                error_code = 404;
                strncpy(error_reason, "replay_slave_not_found",
                        sizeof(error_reason) - 1);
                error_reason[sizeof(error_reason) - 1] = '\0';
                strncpy(error_id, replay_id_requests[i].id,
                        sizeof(error_id) - 1);
                error_id[sizeof(error_id) - 1] = '\0';
                break;
            }
            replay_mask[found_slot] = 1;
        }
    }

    if (!error_code) {
        for (int slot = 0; slot < SYNC_MAX_SLOTS; slot++) {
            const char *new_id = planned[slot][0] ? planned[slot] : NULL;
            sync_master_apply_slot_assignment_locked(&app->master, &cfg, slot,
                                                     new_id);
        }

        for (int slot = 0; slot < SYNC_MAX_SLOTS; slot++) {
            if (!replay_mask[slot]) continue;
            if (!app->master.slot_assignees[slot][0]) continue;
            sync_master_force_slot_replay_locked(&app->master, slot);
            replayed_slots++;
        }

        for (int slot = 0; slot < SYNC_MAX_SLOTS; slot++) {
            if (!app->master.slot_assignees[slot][0]) continue;
            snapshot[snapshot_count].slot = slot;
            strncpy(snapshot[snapshot_count].id,
                    app->master.slot_assignees[slot],
                    sizeof(snapshot[snapshot_count].id) - 1);
            snapshot[snapshot_count].id[sizeof(snapshot[snapshot_count].id) - 1] = '\0';
            snapshot[snapshot_count].generation =
                app->master.slot_generation[slot];
            snapshot_count++;
        }
    }

    pthread_mutex_unlock(&app->master.lock);

    if (error_code) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        const char *reason = error_reason[0] ? error_reason : "invalid_request";
        json_object_set_string(o, "error", reason);
        if (error_id[0]) json_object_set_string(o, "id", error_id);
        if (error_slot > 0) json_object_set_number(o, "slot", error_slot);
        send_json(c, v, error_code, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    JSON_Value *resp = json_value_init_object();
    JSON_Object *ro = json_object(resp);
    json_object_set_string(ro, "status", "updated");
    json_object_set_number(ro, "moves", move_count);
    json_object_set_number(ro, "replayed_slots", replayed_slots);
    json_object_set_number(ro, "deleted", deleted_count);

    if (deleted_count > 0) {
        JSON_Value *deleted_v = json_value_init_array();
        JSON_Array *deleted_arr = json_array(deleted_v);
        for (int i = 0; i < deleted_count; i++) {
            json_array_append_string(deleted_arr, deleted_ids[i]);
        }
        json_object_set_value(ro, "deleted_ids", deleted_v);
    }

    JSON_Value *assign_v = json_value_init_array();
    JSON_Array *assignments = json_array(assign_v);
    for (int i = 0; i < snapshot_count; i++) {
        JSON_Value *item = json_value_init_object();
        JSON_Object *io = json_object(item);
        json_object_set_number(io, "slot", snapshot[i].slot + 1);
        json_object_set_string(io, "slave_id", snapshot[i].id);
        json_object_set_number(io, "generation", snapshot[i].generation);
        if (snapshot[i].slot >= 0 && snapshot[i].slot < SYNC_MAX_SLOTS &&
            cfg.sync_slots[snapshot[i].slot].name[0]) {
            json_object_set_string(io, "slot_label",
                                   cfg.sync_slots[snapshot[i].slot].name);
        }
        json_array_append_value(assignments, item);
    }
    json_object_set_value(ro, "assignments", assign_v);

    send_json(c, resp, 200, 1);
    json_value_free(resp);
    json_value_free(root);
    return 1;
}

static int h_sync_bind(struct mg_connection *c, void *ud) {
    app_t *app = (app_t *)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    if (strcasecmp(cfg.sync_role, "slave") != 0 || !cfg.sync_allow_bind) {
        send_plain(c, 404, "not_found", 1);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(c);
    if (!ri || strcmp(ri->request_method, "POST") != 0) {
        send_plain(c, 405, "method_not_allowed", cfg.ui_public);
        return 1;
    }

    upload_t u = {0};
    if (read_body(c, &u) != 0) {
        if (u.body) free(u.body);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "body_read_failed");
        send_json(c, v, 400, cfg.ui_public);
        json_value_free(v);
        return 1;
    }

    JSON_Value *root = json_parse_string(u.body ? u.body : "{}");
    free(u.body);
    if (!root) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "bad_json");
        send_json(c, v, 400, cfg.ui_public);
        json_value_free(v);
        return 1;
    }

    JSON_Object *obj = json_object(root);
    const char *master_url_in = json_object_get_string(obj, "master_url");
    const char *master_id_in = json_object_get_string(obj, "master_id");
    JSON_Value *interval_v = json_object_get_value(obj, "register_interval_s");

    if ((!master_url_in || !*master_url_in) && (!master_id_in || !*master_id_in)) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "missing_master_reference");
        send_json(c, v, 400, cfg.ui_public);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    char normalized_master[256];
    const char *candidate = (master_id_in && *master_id_in) ? master_id_in : master_url_in;
    if (sync_normalize_master_reference(candidate, normalized_master,
                                        sizeof(normalized_master)) != 0) {
        if (candidate != master_url_in && master_url_in && *master_url_in &&
            sync_normalize_master_reference(master_url_in, normalized_master,
                                            sizeof(normalized_master)) == 0) {
            /* fallback succeeded */
        } else {
            JSON_Value *v = json_value_init_object();
            JSON_Object *o = json_object(v);
            json_object_set_string(o, "error", "invalid_master_reference");
            send_json(c, v, 400, cfg.ui_public);
            json_value_free(v);
            json_value_free(root);
            return 1;
        }
    }

    int new_interval = cfg.sync_register_interval_s;
    if (interval_v && json_value_get_type(interval_v) == JSONNumber) {
        new_interval = (int)json_value_get_number(interval_v);
        if (new_interval <= 0) new_interval = cfg.sync_register_interval_s;
    }

    pthread_mutex_lock(&app->cfg_lock);
    strncpy(app->base_cfg.sync_master_url, normalized_master,
            sizeof(app->base_cfg.sync_master_url) - 1);
    app->base_cfg.sync_master_url[sizeof(app->base_cfg.sync_master_url) - 1] = '\0';
    app->base_cfg.sync_register_interval_s = new_interval;
    app_rebuild_config_locked(app);
    app->active_override_generation = 0;
    pthread_mutex_unlock(&app->cfg_lock);
    sync_slave_reset_tracking(&app->slave);

    JSON_Value *resp = json_value_init_object();
    JSON_Object *ro = json_object(resp);
    json_object_set_string(ro, "status", "bound");
    json_object_set_string(ro, "master_url", normalized_master);
    json_object_set_number(ro, "register_interval_s", new_interval);
    send_json(c, resp, 200, cfg.ui_public);
    json_value_free(resp);
    json_value_free(root);
    return 1;
}
void sync_append_capabilities(const config_t *cfg, JSON_Array *caps_arr) {
    if (!cfg || !caps_arr) return;
    if (!cfg->sync_role[0]) return;
    const char *sync_cap = (strcasecmp(cfg->sync_role, "master") == 0)
                               ? "sync-master"
                               : "sync-slave";
    json_array_append_string(caps_arr, sync_cap);
}

JSON_Value *sync_build_status_json(const config_t *cfg, sync_slave_state_t *state) {
    if (!cfg || !cfg->sync_role[0]) return NULL;
    JSON_Value *sync_v = json_value_init_object();
    JSON_Object *so = json_object(sync_v);
    json_object_set_string(so, "role", cfg->sync_role);
    if (cfg->sync_id[0]) json_object_set_string(so, "id", cfg->sync_id);
    json_object_set_number(so, "allow_bind", cfg->sync_allow_bind ? 1 : 0);
    if (strcasecmp(cfg->sync_role, "slave") == 0) {
        if (cfg->sync_master_url[0]) {
            json_object_set_string(so, "master_url", cfg->sync_master_url);
        }
        json_object_set_number(so, "register_interval_s",
                               cfg->sync_register_interval_s);
        json_object_set_number(so, "last_received_generation",
                               sync_slave_get_last_received(state));
        json_object_set_number(so, "applied_generation",
                               sync_slave_get_applied_generation(state));
        int cur_slot = sync_slave_get_current_slot(state);
        if (cur_slot > 0) {
            json_object_set_number(so, "current_slot", cur_slot);
            char slot_label_buf[64];
            sync_slave_get_current_slot_label(state, slot_label_buf,
                                              sizeof(slot_label_buf));
            if (slot_label_buf[0]) {
                json_object_set_string(so, "current_slot_label", slot_label_buf);
            }
        }
    }
    return sync_v;
}

void sync_register_http_handlers(struct mg_context *ctx, app_t *app) {
    if (!ctx) return;
    mg_set_request_handler(ctx, "/sync/register", h_sync_register, app);
    mg_set_request_handler(ctx, "/sync/slaves", h_sync_slaves, app);
    mg_set_request_handler(ctx, "/sync/push", h_sync_push, app);
    mg_set_request_handler(ctx, "/sync/bind", h_sync_bind, app);
}

int sync_slave_start_thread(app_t *app) {
    if (!app) return -1;
    pthread_mutex_lock(&app->slave.lock);
    app->slave.stop = 0;
    if (app->slave.running) {
        pthread_mutex_unlock(&app->slave.lock);
        return 0;
    }
    if (pthread_create(&app->slave.thread, NULL, sync_slave_thread_main, app) == 0) {
        app->slave.running = 1;
        pthread_mutex_unlock(&app->slave.lock);
        return 0;
    }
    pthread_mutex_unlock(&app->slave.lock);
    fprintf(stderr, "WARN: failed to start sync slave thread\n");
    return -1;
}

void sync_slave_stop_thread(sync_slave_state_t *state) {
    if (!state) return;
    pthread_mutex_lock(&state->lock);
    state->stop = 1;
    pthread_mutex_unlock(&state->lock);
    if (state->running) {
        pthread_join(state->thread, NULL);
        pthread_mutex_lock(&state->lock);
        state->running = 0;
        pthread_mutex_unlock(&state->lock);
    }
}

