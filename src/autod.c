/*
autod.c — lightweight HTTP control plane (CivetWeb, NO AUTH), with optional LAN scanner

gcc -Os -std=c11 -Wall -Wextra -DNO_SSL -DNO_CGI -DNO_FILES \
    autod.c scan.c parson.c civetweb.c -o autod -pthread
strip autod
*/

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#include <strings.h>
#include <poll.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include "civetweb.h"
#include "parson.h"
#include "scan.h"        // <— NEW

#if !defined(_WIN32)
extern char *realpath(const char *path, char *resolved_path);
#endif

static volatile sig_atomic_t g_stop=0;
static void on_signal(int s){ (void)s; g_stop=1; }

/* helper */
static inline void set_num2(JSON_Object *o, const char *key, double x) {
    char buf[32];
    long long q = (long long)(x * 100.0 + (x >= 0 ? 0.5 : -0.5));
    snprintf(buf, sizeof(buf), "%.2f", q / 100.0);
    json_object_set_string(o, key, buf);
}

static inline void arr_append_num2(JSON_Array *a, double x) {
    char buf[32];
    long long q = (long long)(x * 100.0 + (x >= 0 ? 0.5 : -0.5));
    snprintf(buf, sizeof(buf), "%.2f", q / 100.0);
    json_array_append_string(a, buf);
}

/* ----------------------- Config (no auth) ----------------------- */
#define STARTUP_MAX_EXEC 16
#define SYNC_MAX_SLOTS 10
#define SYNC_SLOT_MAX_COMMANDS 16

typedef struct {
    char name[64];
    int command_count;
    struct { char json[512]; } commands[SYNC_SLOT_MAX_COMMANDS];
} sync_slot_config_t;

typedef struct {
    /* server */
    int  port;
    char bind_addr[64];
    int  enable_scan;      /* 0/1 — default off */

    /* sync */
    char sync_role[16];
    char sync_master_url[256];
    char sync_id[64];
    int  sync_register_interval_s;
    int  sync_allow_bind;
    int  sync_slot_retention_s;

    sync_slot_config_t sync_slots[SYNC_MAX_SLOTS];

    /* scan */
    scan_extra_subnet_t extra_subnets[SCAN_MAX_EXTRA_SUBNETS];
    unsigned            extra_subnet_count;

    /* exec */
    char interpreter[128];
    int  exec_timeout_ms;
    int  max_output_bytes;

    /* startup exec */
    int  startup_exec_count;
    struct { char json[512]; } startup_exec[STARTUP_MAX_EXEC];

    /* caps */
    char device[64];
    char role[64];
    char version[32];
    char caps[256];         /* comma-separated */
    int  include_net_info;  /* 0/1 */

    /* announce SSE */
    struct { char name[64]; char url[192]; } sse[16];
    int  sse_count;

    /* ui */
    char ui_path[256];
    int  serve_ui;
    int  ui_public;
} config_t;

static void trim(char *s) {
    if (!s) return;
    size_t l = strlen(s), i = 0;
    while (i < l && isspace((unsigned char)s[i])) i++;
    if (i) memmove(s, s + i, l - i + 1);
    l = strlen(s);
    while (l > 0 && isspace((unsigned char)s[l - 1])) s[--l] = '\0';
}

static void cfg_defaults(config_t *c) {
    memset(c, 0, sizeof(*c));
    c->port = 8080;
    strncpy(c->bind_addr, "0.0.0.0", sizeof(c->bind_addr)-1);
    c->enable_scan = 0;
    c->sync_register_interval_s = 30;
    c->sync_allow_bind = 1;
    c->sync_slot_retention_s = 0;
    c->extra_subnet_count = 0;

    strncpy(c->interpreter, "/usr/bin/exec-handler.sh", sizeof(c->interpreter)-1);
    c->exec_timeout_ms = 5000;
    c->max_output_bytes = 65536;

    c->include_net_info = 1;
    c->sse_count = 0;
    c->serve_ui = 0;
    c->ui_public = 1;
}

static int cfg_has_cap(const config_t *cfg, const char *cap) {
    if (!cfg || !cap || !*cap || !cfg->caps[0]) return 0;
    char tmp[sizeof(cfg->caps)];
    strncpy(tmp, cfg->caps, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(tmp, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        trim(tok);
        if (*tok && strcasecmp(tok, cap) == 0) return 1;
    }
    return 0;
}

static int parse_extra_subnet(const char *value, scan_extra_subnet_t *out) {
    if (!value || !*value || !out) return -1;
    char copy[64];
    strncpy(copy, value, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';

    char *slash = strchr(copy, '/');
    if (!slash) return -1;
    *slash = '\0';

    char *ip = copy;
    char *prefix = slash + 1;
    trim(ip);
    trim(prefix);
    if (!*ip || !*prefix) return -1;

    char *end = NULL;
    long pre = strtol(prefix, &end, 10);
    if (!end || *end != '\0' || pre < 0 || pre > 32) return -1;
    if (pre == 0) return -1; // avoid scanning entire IPv4 space

    struct in_addr ip4;
    if (inet_pton(AF_INET, ip, &ip4) != 1) return -1;

    uint32_t addr = ntohl(ip4.s_addr);
    uint32_t mask = (pre == 0) ? 0u : (pre == 32 ? 0xffffffffu : (uint32_t)(0xffffffffu << (32 - pre)));
    out->netmask = mask;
    out->network = (mask == 0xffffffffu) ? addr : (addr & mask);
    return 0;
}

static int parse_ini(const char *path, config_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512], sect[64] = "";
    while (fgets(line, sizeof(line), f)) {
        char *p = line; trim(p);
        if (!*p || *p==';' || *p=='#') continue;
        if (*p=='[') { char *r=strchr(p,']'); if(r){*r='\0'; strncpy(sect,p+1,sizeof(sect)-1); sect[sizeof(sect)-1]='\0';} continue; }
        char *eq = strchr(p,'='); if(!eq) continue; *eq='\0';
        char *k=p, *v=eq+1; trim(k); trim(v);

        if (strcmp(sect,"server")==0) {
            if (!strcmp(k,"port")) cfg->port=atoi(v);
            else if (!strcmp(k,"bind")) strncpy(cfg->bind_addr,v,sizeof(cfg->bind_addr)-1);
            else if (!strcmp(k,"enable_scan")) cfg->enable_scan=atoi(v);

        } else if (strcmp(sect,"exec")==0) {
            if (!strcmp(k,"interpreter")) strncpy(cfg->interpreter,v,sizeof(cfg->interpreter)-1);
            else if (!strcmp(k,"timeout_ms")) cfg->exec_timeout_ms=atoi(v);
            else if (!strcmp(k,"max_output_bytes")) cfg->max_output_bytes=atoi(v);

        } else if (strcmp(sect,"caps")==0) {
            if (!strcmp(k,"device"))  strncpy(cfg->device,v,sizeof(cfg->device)-1);
            else if (!strcmp(k,"role"))    strncpy(cfg->role,v,sizeof(cfg->role)-1);
            else if (!strcmp(k,"version")) strncpy(cfg->version,v,sizeof(cfg->version)-1);
            else if (!strcmp(k,"caps"))    strncpy(cfg->caps,v,sizeof(cfg->caps)-1);
            else if (!strcmp(k,"include_net_info")) cfg->include_net_info=atoi(v);

        } else if (strcmp(sect,"announce")==0) {
            if (!strcmp(k,"sse") && cfg->sse_count<16) {
                char copy[256]; strncpy(copy,v,sizeof(copy)-1); copy[sizeof(copy)-1]='\0';
                char *at=strchr(copy,'@'); int idx=cfg->sse_count;
                if (at) { *at='\0'; trim(copy); trim(at+1);
                    strncpy(cfg->sse[idx].name,copy,sizeof(cfg->sse[idx].name)-1);
                    strncpy(cfg->sse[idx].url, at+1,sizeof(cfg->sse[idx].url)-1);
                } else {
                    snprintf(cfg->sse[idx].name,sizeof(cfg->sse[idx].name),"sse%d", idx+1);
                    strncpy(cfg->sse[idx].url, copy, sizeof(cfg->sse[idx].url)-1);
                }
                cfg->sse[idx].name[sizeof(cfg->sse[idx].name)-1]='\0';
                cfg->sse[idx].url [sizeof(cfg->sse[idx].url )-1]='\0';
                cfg->sse_count++;
            }

        } else if (strcmp(sect,"scan")==0) {
            if ((!strcmp(k,"extra_subnet") || !strcmp(k,"subnet")) && cfg->extra_subnet_count < SCAN_MAX_EXTRA_SUBNETS) {
                scan_extra_subnet_t sn = {0};
                if (parse_extra_subnet(v, &sn) == 0) {
                    cfg->extra_subnets[cfg->extra_subnet_count++] = sn;
                } else {
                    fprintf(stderr, "WARN: ignoring invalid extra_subnet '%s'\n", v);
                }
            } else if (!strcmp(k,"extra_subnet") || !strcmp(k,"subnet")) {
                fprintf(stderr, "WARN: extra_subnet capacity reached (%u)\n", SCAN_MAX_EXTRA_SUBNETS);
            }

        } else if (strcmp(sect,"ui")==0) {
            if (!strcmp(k,"ui_path"))   strncpy(cfg->ui_path,v,sizeof(cfg->ui_path)-1);
            else if (!strcmp(k,"serve_ui"))  cfg->serve_ui=atoi(v);
            else if (!strcmp(k,"ui_public")) cfg->ui_public=atoi(v);
        } else if (strcmp(sect,"startup")==0) {
            if ((!strcmp(k,"exec") || !strcmp(k,"command")) &&
                cfg->startup_exec_count < STARTUP_MAX_EXEC) {
                int idx = cfg->startup_exec_count++;
                strncpy(cfg->startup_exec[idx].json, v,
                        sizeof(cfg->startup_exec[idx].json) - 1);
                cfg->startup_exec[idx].json[sizeof(cfg->startup_exec[idx].json) - 1] = '\0';
            } else if ((!strcmp(k,"exec") || !strcmp(k,"command")) &&
                       cfg->startup_exec_count >= STARTUP_MAX_EXEC) {
                fprintf(stderr,
                        "WARN: startup exec capacity reached (%d)\n",
                        STARTUP_MAX_EXEC);
            }
        } else if (strcmp(sect,"sync")==0) {
            if (!strcmp(k,"role")) strncpy(cfg->sync_role, v, sizeof(cfg->sync_role) - 1);
            else if (!strcmp(k,"master_url")) strncpy(cfg->sync_master_url, v, sizeof(cfg->sync_master_url) - 1);
            else if (!strcmp(k,"id")) strncpy(cfg->sync_id, v, sizeof(cfg->sync_id) - 1);
            else if (!strcmp(k,"register_interval_s")) cfg->sync_register_interval_s = atoi(v);
            else if (!strcmp(k,"allow_bind")) cfg->sync_allow_bind = atoi(v);
            else if (!strcmp(k,"slot_retention_s")) cfg->sync_slot_retention_s = atoi(v);
        } else if (strncmp(sect, "sync.slot", 9) == 0) {
            int slot_index = atoi(sect + 9);
            if (slot_index <= 0 || slot_index > SYNC_MAX_SLOTS) {
                fprintf(stderr,
                        "WARN: ignoring sync slot section '%s' (index out of range)\n",
                        sect);
                continue;
            }
            sync_slot_config_t *slot = &cfg->sync_slots[slot_index - 1];
            if (!strcmp(k, "name")) {
                strncpy(slot->name, v, sizeof(slot->name) - 1);
                slot->name[sizeof(slot->name) - 1] = '\0';
            } else if ((!strcmp(k, "exec") || !strcmp(k, "command"))) {
                if (slot->command_count >= SYNC_SLOT_MAX_COMMANDS) {
                    fprintf(stderr,
                            "WARN: sync slot %d command capacity reached (%d)\n",
                            slot_index, SYNC_SLOT_MAX_COMMANDS);
                    continue;
                }
                JSON_Value *tmp = json_parse_string(v);
                if (!tmp || json_value_get_type(tmp) != JSONObject) {
                    fprintf(stderr,
                            "WARN: ignoring invalid sync slot %d command '%s'\n",
                            slot_index, v);
                    if (tmp) json_value_free(tmp);
                    continue;
                }
                json_value_free(tmp);
                int idx = slot->command_count++;
                strncpy(slot->commands[idx].json, v,
                        sizeof(slot->commands[idx].json) - 1);
                slot->commands[idx].json[sizeof(slot->commands[idx].json) - 1] = '\0';
            }
        }
    }
    fclose(f);
    return 0;
}

static void fill_scan_config(const config_t *cfg, scan_config_t *scfg) {
    if (!cfg || !scfg) return;
    memset(scfg, 0, sizeof(*scfg));
    scfg->port = cfg->port;
    if (cfg->role[0])    strncpy(scfg->role,    cfg->role,    sizeof(scfg->role)-1);
    if (cfg->device[0])  strncpy(scfg->device,  cfg->device,  sizeof(scfg->device)-1);
    if (cfg->version[0]) strncpy(scfg->version, cfg->version, sizeof(scfg->version)-1);
    if (cfg->sync_role[0]) strncpy(scfg->sync_role, cfg->sync_role, sizeof(scfg->sync_role) - 1);
    if (cfg->sync_id[0])   strncpy(scfg->sync_id,   cfg->sync_id,   sizeof(scfg->sync_id) - 1);
    scfg->extra_subnet_count = cfg->extra_subnet_count;
    if (scfg->extra_subnet_count > SCAN_MAX_EXTRA_SUBNETS)
        scfg->extra_subnet_count = SCAN_MAX_EXTRA_SUBNETS;
    if (scfg->extra_subnet_count > 0) {
        memcpy(scfg->extra_subnets, cfg->extra_subnets,
               scfg->extra_subnet_count * sizeof(scan_extra_subnet_t));
    }
}

static int log_civet_message(const struct mg_connection *conn, const char *message) {
    (void)conn;
    if (message && *message) {
        size_t len = strlen(message);
        if (len && message[len - 1] == '\n') {
            fprintf(stderr, "civetweb: %s", message);
        } else {
            fprintf(stderr, "civetweb: %s\n", message);
        }
        fflush(stderr);
    }
    return 1;
}

/* ----------------------- Runtime helpers ----------------------- */
static void get_request_host_only(struct mg_connection *c, char *out, size_t outlen) {
    const char *host = mg_get_header(c, "Host");
    if (!host || !*host) { strncpy(out, "127.0.0.1", outlen-1); out[outlen-1]='\0'; return; }
    const char *colon = strchr(host, ':');
    size_t n = colon ? (size_t)(colon - host) : strlen(host);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, host, n); out[n] = '\0';
}

static void substitute_ip_placeholder(struct mg_connection *c,
                                      const char *in, char *out, size_t outlen) {
    char hostonly[128]; get_request_host_only(c, hostonly, sizeof(hostonly));
    const char *p = strstr(in, "http://IP");
    const char *q = strstr(in, "{IP}");
    if (!p && !q) { strncpy(out, in, outlen-1); out[outlen-1]='\0'; return; }
    if (p) {
        const char *after = in + strlen("http://IP");
        int n = snprintf(out, outlen, "http://%s%s", hostonly, after);
        if (n < 0 || (size_t)n >= outlen) out[outlen-1] = '\0';
        return;
    }
    if (q) {
        size_t prefix = (size_t)(q - in);
        snprintf(out, outlen, "%.*s%s%s", (int)prefix, in, hostonly, q + 4);
        return;
    }
}

static inline long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000LL + ts.tv_nsec/1000000LL;
}

static void json_add_runtime(JSON_Object *o) {
    FILE *f=fopen("/proc/uptime","r");
    if(f){
        double up=0;
        if (fscanf(f, "%lf", &up) == 1) set_num2(o, "uptime_s", up);
        fclose(f);
    }
    f=fopen("/proc/loadavg","r");
    if(f){
        double a,b,c;
        if(fscanf(f,"%lf %lf %lf",&a,&b,&c)==3){
            JSON_Value *arr = json_value_init_array();
            JSON_Array *ar = json_array(arr);
            arr_append_num2(ar, a);
            arr_append_num2(ar, b);
            arr_append_num2(ar, c);
            json_object_set_value(o, "loadavg", arr);
        }
        fclose(f);
    }
    f=fopen("/proc/meminfo","r");
    if(f){
        char k[64]; long v;
        while(fscanf(f,"%63[^:]: %ld kB\n",k,&v)==2){
            if(!strcmp(k,"MemFree"))     json_object_set_number(o,"memfree_kb",(double)v);
            if(!strcmp(k,"MemAvailable"))json_object_set_number(o,"memavail_kb",(double)v);
        }
        fclose(f);
    }
    json_object_set_number(o,"ts_unix",(double)time(NULL));
}

static void json_add_ifaddrs(JSON_Object *o) {
    JSON_Value *arr=json_value_init_array(); JSON_Array *a=json_array(arr);
    struct ifaddrs *ifaddr; if(getifaddrs(&ifaddr)==0){
        for(struct ifaddrs *ifa=ifaddr; ifa; ifa=ifa->ifa_next){
            if(!ifa->ifa_addr) continue;
            if(ifa->ifa_addr->sa_family!=AF_INET) continue;
            char buf[INET_ADDRSTRLEN]; struct sockaddr_in *sin=(struct sockaddr_in*)ifa->ifa_addr;
            if(!inet_ntop(AF_INET,&sin->sin_addr,buf,sizeof(buf))) continue;
            JSON_Value *oif=json_value_init_object(); JSON_Object *oi=json_object(oif);
            json_object_set_string(oi,"if",ifa->ifa_name);
            json_object_set_string(oi,"ip",buf);
            json_array_append_value(a,oif);
        }
        freeifaddrs(ifaddr);
    }
    json_object_set_value(o,"ifaddrs",arr);
}

#define SYNC_MAX_SLAVES 64

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

typedef struct {
    char host[128];
    int port;
    char path[256];
} http_url_t;

/* ----------------------- Exec runner ----------------------- */
typedef struct { char *body; size_t len; } upload_t;

enum { MAX_BODY_BYTES = 262144 }; /* 256 KiB guard */

static void close_pipe_pair(int pipefd[2]) {
    if (pipefd[0] >= 0) { close(pipefd[0]); pipefd[0] = -1; }
    if (pipefd[1] >= 0) { close(pipefd[1]); pipefd[1] = -1; }
}

static void drain_exec_pipe(int fd, char *buf, int *written, int max_bytes) {
    if (fd < 0) return;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0 && !(flags & O_NONBLOCK)) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    char tmp[1024];
    for (;;) {
        ssize_t r = read(fd, tmp, sizeof(tmp));
        if (r > 0) {
            if (buf && written) {
                int space = max_bytes - *written;
                if (space < 0) space = 0;
                int copy = (int)r;
                if (copy > space) copy = space;
                if (copy > 0) {
                    memcpy(buf + *written, tmp, (size_t)copy);
                    *written += copy;
                }
            }
            continue;
        }
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        }
        break;
    }
}

static void drain_exec_pipes(int out_fd, int err_fd,
                             char *buf_out, int *wout,
                             char *buf_err, int *werr,
                             int max_bytes) {
    drain_exec_pipe(out_fd, buf_out, wout, max_bytes);
    drain_exec_pipe(err_fd, buf_err, werr, max_bytes);
}

static int run_exec(const config_t *cfg, const char *path, JSON_Array *args,
                    int timeout_ms, int max_bytes,
                    int *rc_out, long long *elapsed_ms,
                    char **out_stdout, char **out_stderr)
{
    int out_pipe[2] = { -1, -1 }, err_pipe[2] = { -1, -1 };
    char *buf_out = NULL, *buf_err = NULL;
    pid_t pid = -1;
    long long t0 = 0;

    if (pipe(out_pipe) < 0) goto fail_before_fork;
    if (pipe(err_pipe) < 0) goto fail_before_fork;

    t0 = now_ms();
    pid = fork();
    if (pid < 0) goto fail_before_fork;

    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        size_t narg = args ? json_array_get_count(args) : 0;
        size_t ac = 2 + narg + 1;
        char **argv = calloc(ac, sizeof(char*));
        if (!argv) _exit(127);
        argv[0] = (char*)cfg->interpreter;
        argv[1] = (char*)path;
        for (size_t i=0;i<narg;i++) argv[2+i] = (char*)json_array_get_string(args, i);
        argv[2+narg] = NULL;
        execv(cfg->interpreter, argv);
        dprintf(STDERR_FILENO, "execv failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* parent */
    close(out_pipe[1]); out_pipe[1] = -1;
    close(err_pipe[1]); err_pipe[1] = -1;
    buf_out = malloc(max_bytes + 1);
    buf_err = malloc(max_bytes + 1);
    if (!buf_out || !buf_err) goto fail_after_fork;
    int wout = 0, werr = 0;

    struct pollfd pfds[2] = {
        { .fd = out_pipe[0], .events = POLLIN },
        { .fd = err_pipe[0], .events = POLLIN }
    };

    int remain = timeout_ms;
    int status = 0;
    int child_done = 0;

    while (remain > 0) {
        int pr = poll(pfds, 2, remain);
        long long t = now_ms();

        if (pr < 0) {
            if (errno == EINTR) { remain = timeout_ms - (int)(t - t0); continue; }
            break;
        }

        if (pr > 0) {
            if (pfds[0].revents & POLLIN) {
                char tmp[1024]; int r = read(out_pipe[0], tmp, sizeof(tmp));
                if (r > 0) { int c = r; if (wout + c > max_bytes) c = max_bytes - wout; if (c > 0) { memcpy(buf_out + wout, tmp, c); wout += c; } }
                if (r == 0) pfds[0].events = 0;
            }
            if (pfds[1].revents & POLLIN) {
                char tmp[1024]; int r = read(err_pipe[0], tmp, sizeof(tmp));
                if (r > 0) { int c = r; if (werr + c > max_bytes) c = max_bytes - werr; if (c > 0) { memcpy(buf_err + werr, tmp, c); werr += c; } }
                if (r == 0) pfds[1].events = 0;
            }
        }

        pid_t wp = waitpid(pid, &status, WNOHANG);
        if (wp == pid) { child_done = 1; break; }

        if (!(pfds[0].events || pfds[1].events)) {
            wp = waitpid(pid, &status, WNOHANG);
            if (wp == pid) child_done = 1;
            break;
        }

        remain = timeout_ms - (int)(t - t0);
    }

    drain_exec_pipes(out_pipe[0], err_pipe[0], buf_out, &wout, buf_err, &werr, max_bytes);

    close_pipe_pair(out_pipe);
    close_pipe_pair(err_pipe);

    int rc = 0;
    if (!child_done) {
        pid_t wp = waitpid(pid, &status, WNOHANG);
        if (wp == pid) {
            child_done = 1;
        } else {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            rc = 124;
        }
    }

    if (child_done) {
        if (WIFEXITED(status)) rc = WEXITSTATUS(status);
        else rc = 128;
    }

    *rc_out = rc;
    *elapsed_ms = now_ms() - t0;
    buf_out[wout] = '\0';
    buf_err[werr] = '\0';
    *out_stdout = buf_out;
    *out_stderr = buf_err;
    return 0;

fail_after_fork:
    if (buf_out) { free(buf_out); buf_out = NULL; }
    if (buf_err) { free(buf_err); buf_err = NULL; }
    close_pipe_pair(out_pipe);
    close_pipe_pair(err_pipe);
    if (pid > 0) {
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    return -1;

fail_before_fork:
    close_pipe_pair(out_pipe);
    close_pipe_pair(err_pipe);
    return -1;
}

/* ----------------------- CivetWeb helpers ----------------------- */

static const char *reason_phrase_for_status(int code) {
    switch (code) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default:  return NULL;
    }
}

static void add_common_headers_extra(struct mg_connection *c, int code, const char *ctype,
                                     size_t clen, int cors_public, const char *extra) {
    const char *reason = reason_phrase_for_status(code);
    if (reason) {
        mg_printf(c, "HTTP/1.1 %d %s\r\n", code, reason);
    } else {
        mg_printf(c, "HTTP/1.1 %d\r\n", code);
    }
    mg_printf(c, "Content-Type: %s\r\n", ctype ? ctype : "application/octet-stream");
    mg_printf(c, "Content-Length: %zu\r\n", clen);
    if (cors_public) {
        mg_printf(c, "Access-Control-Allow-Origin: *\r\n");
        mg_printf(c, "Vary: Origin\r\n");
    }
    if (extra && *extra) {
        mg_printf(c, "%s", extra);
    }
    mg_printf(c, "Cache-Control: no-store\r\n");
    mg_printf(c, "Connection: close\r\n\r\n");
}

static void add_common_headers(struct mg_connection *c, int code, const char *ctype,
                               size_t clen, int cors_public) {
    add_common_headers_extra(c, code, ctype, clen, cors_public, NULL);
}

static void add_cors_options(struct mg_connection *c) {
    mg_printf(c,
      "HTTP/1.1 204 No Content\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
      "Access-Control-Allow-Headers: Content-Type\r\n"
      "Access-Control-Max-Age: 600\r\n"
      "Content-Length: 0\r\n"
      "Connection: close\r\n\r\n");
}

static const char *guess_mime_type(const char *path) {
    if (!path) return "application/octet-stream";
    const char *dot = strrchr(path, '.');
    if (!dot || !dot[1]) return "application/octet-stream";
    dot++;
    if (!strcasecmp(dot, "html") || !strcasecmp(dot, "htm")) {
        return "text/html; charset=utf-8";
    } else if (!strcasecmp(dot, "css")) {
        return "text/css; charset=utf-8";
    } else if (!strcasecmp(dot, "js")) {
        return "application/javascript; charset=utf-8";
    } else if (!strcasecmp(dot, "json")) {
        return "application/json; charset=utf-8";
    } else if (!strcasecmp(dot, "svg")) {
        return "image/svg+xml";
    } else if (!strcasecmp(dot, "png")) {
        return "image/png";
    } else if (!strcasecmp(dot, "jpg") || !strcasecmp(dot, "jpeg")) {
        return "image/jpeg";
    } else if (!strcasecmp(dot, "gif")) {
        return "image/gif";
    } else if (!strcasecmp(dot, "webp")) {
        return "image/webp";
    } else if (!strcasecmp(dot, "mp4") || !strcasecmp(dot, "m4v")) {
        return "video/mp4";
    } else if (!strcasecmp(dot, "webm")) {
        return "video/webm";
    } else if (!strcasecmp(dot, "wasm")) {
        return "application/wasm";
    } else if (!strcasecmp(dot, "txt")) {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

static int read_body(struct mg_connection *c, upload_t *u) {
    u->body = NULL;
    u->len  = 0;

    const char *cl = mg_get_header(c, "Content-Length");
    size_t need = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (need == 0) return 0;
    if (need > MAX_BODY_BYTES) return -1;

    char *buf = (char*)malloc(need + 1);
    if (!buf) return -1;

    size_t got = 0;
    while (got < need) {
        int r = mg_read(c, buf + got, (int)(need - got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    if (got != need) { free(buf); return -1; }
    buf[got] = '\0';
    u->body = buf;
    u->len  = got;
    return 0;
}

static void send_json(struct mg_connection *c, JSON_Value *v, int code, int cors_public) {
    char *s = json_serialize_to_string(v);
    size_t n = s ? strlen(s) : 0;
    add_common_headers(c, code, "application/json; charset=utf-8", n, cors_public);
    if (n) mg_write(c, s, (int)n);
    if (s) json_free_serialized_string(s);
}

static void send_plain(struct mg_connection *c, int code, const char *msg, int cors_public) {
    const char *text = msg ? msg : "";
    size_t n = strlen(text);
    add_common_headers(c, code, "text/plain; charset=utf-8", n, cors_public);
    if (n) mg_write(c, text, (int)n);
}

static int format_http_date(time_t when, char *buf, size_t buf_sz) {
    if (!buf || buf_sz == 0) return -1;
#if defined(_WIN32)
    struct tm tmp;
    if (gmtime_s(&tmp, &when) != 0) return -1;
#else
    struct tm tmp;
    if (!gmtime_r(&when, &tmp)) return -1;
#endif
    return strftime(buf, buf_sz, "%a, %d %b %Y %H:%M:%S GMT", &tmp) ? 0 : -1;
}

/* ----------------------- HTTP Handlers ----------------------- */
typedef struct {
    config_t cfg;
    config_t base_cfg;
    struct mg_context *ctx;
    pthread_mutex_t cfg_lock;
    JSON_Value *active_overrides;
    int active_override_generation;
    sync_master_state_t master;
    sync_slave_state_t slave;
} app_t;

static void ensure_sync_id(config_t *cfg) {
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

static void sync_caps_from_json_value(const JSON_Value *value, char *dest, size_t dest_sz) {
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
            size_t len = strlen(s);
            if (len + (pos ? 1 : 0) >= dest_sz - pos) break;
            if (pos) dest[pos++] = ',';
            memcpy(dest + pos, s, len);
            pos += len;
            dest[pos] = '\0';
        }
    }
}

static void app_rebuild_config_locked(app_t *app) {
    if (!app) return;
    config_t merged = app->base_cfg;
    ensure_sync_id(&merged);
    app->cfg = merged;
}

static void app_config_snapshot(app_t *app, config_t *out) {
    if (!app || !out) return;
    pthread_mutex_lock(&app->cfg_lock);
    *out = app->cfg;
    pthread_mutex_unlock(&app->cfg_lock);
}

static void sync_slave_init_state(sync_slave_state_t *state) {
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

static void sync_master_init_state(sync_master_state_t *state) {
    if (!state) return;
    pthread_mutex_init(&state->lock, NULL);
    memset(state->records, 0, sizeof(state->records));
    memset(state->slot_generation, 0, sizeof(state->slot_generation));
    memset(state->slot_assignees, 0, sizeof(state->slot_assignees));
}

static void sync_slave_set_applied_generation(app_t *app, int generation) {
    if (!app) return;
    pthread_mutex_lock(&app->slave.lock);
    app->slave.applied_generation = generation;
    pthread_mutex_unlock(&app->slave.lock);
}

static int sync_slave_get_applied_generation(app_t *app) {
    if (!app) return 0;
    pthread_mutex_lock(&app->slave.lock);
    int g = app->slave.applied_generation;
    pthread_mutex_unlock(&app->slave.lock);
    return g;
}

static void sync_slave_set_last_received(app_t *app, int generation) {
    if (!app) return;
    pthread_mutex_lock(&app->slave.lock);
    app->slave.last_received_generation = generation;
    pthread_mutex_unlock(&app->slave.lock);
}

static int sync_slave_get_last_received(app_t *app) {
    if (!app) return 0;
    pthread_mutex_lock(&app->slave.lock);
    int g = app->slave.last_received_generation;
    pthread_mutex_unlock(&app->slave.lock);
    return g;
}

static void sync_slave_set_current_slot(app_t *app, int slot, const char *label) {
    if (!app) return;
    pthread_mutex_lock(&app->slave.lock);
    app->slave.current_slot = slot;
    if (label && *label) {
        strncpy(app->slave.current_slot_label, label,
                sizeof(app->slave.current_slot_label) - 1);
        app->slave.current_slot_label[sizeof(app->slave.current_slot_label) - 1] = '\0';
    } else {
        app->slave.current_slot_label[0] = '\0';
    }
    pthread_mutex_unlock(&app->slave.lock);
}

static int sync_slave_get_current_slot(app_t *app) {
    if (!app) return -1;
    pthread_mutex_lock(&app->slave.lock);
    int slot = app->slave.current_slot;
    pthread_mutex_unlock(&app->slave.lock);
    return slot;
}

static void sync_slave_get_current_slot_label(app_t *app, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!app) return;
    pthread_mutex_lock(&app->slave.lock);
    if (app->slave.current_slot_label[0]) {
        strncpy(out, app->slave.current_slot_label, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
    pthread_mutex_unlock(&app->slave.lock);
}

static void sync_slave_reset_tracking(app_t *app) {
    if (!app) return;
    pthread_mutex_lock(&app->slave.lock);
    app->slave.applied_generation = 0;
    app->slave.last_received_generation = 0;
    app->slave.current_slot = -1;
    app->slave.current_slot_label[0] = '\0';
    pthread_mutex_unlock(&app->slave.lock);
    pthread_mutex_lock(&app->cfg_lock);
    app->active_override_generation = 0;
    pthread_mutex_unlock(&app->cfg_lock);
}

static sync_slave_record_t *sync_master_find_record(sync_master_state_t *state,
                                                    const char *id, int create);
static int sync_master_mark_slot_generation(sync_master_state_t *state,
                                            int slot_index);

static void sync_master_release_slot_locked(sync_master_state_t *state,
                                            int slot_index) {
    if (!state || slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) return;
    if (!state->slot_assignees[slot_index][0]) return;
    sync_slave_record_t *rec =
        sync_master_find_record(state, state->slot_assignees[slot_index], 0);
    if (rec) {
        rec->slot_index = -1;
        rec->last_ack_generation = 0;
    }
    state->slot_assignees[slot_index][0] = '\0';
    sync_master_mark_slot_generation(state, slot_index);
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

static int sync_master_assign_slot_locked(sync_master_state_t *state,
                                          sync_slave_record_t *rec,
                                          int slot_index) {
    if (!state || !rec || slot_index < 0 || slot_index >= SYNC_MAX_SLOTS) {
        return -1;
    }

    if (sync_master_slot_matches(state, slot_index, rec->id)) {
        rec->slot_index = slot_index;
        rec->last_ack_generation = 0;
        if (state->slot_generation[slot_index] <= 0) {
            state->slot_generation[slot_index] = 1;
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
        if (prev) {
            prev->slot_index = -1;
            prev->last_ack_generation = 0;
        }
    }

    strncpy(state->slot_assignees[slot_index], rec->id,
            sizeof(state->slot_assignees[slot_index]) - 1);
    state->slot_assignees[slot_index][sizeof(state->slot_assignees[slot_index]) - 1] = '\0';
    rec->slot_index = slot_index;
    rec->last_ack_generation = 0;
    return sync_master_mark_slot_generation(state, slot_index);
}

static int sync_master_auto_assign_slot_locked(sync_master_state_t *state,
                                               sync_slave_record_t *rec,
                                               const config_t *cfg) {
    (void)cfg;
    if (!state || !rec) return -1;

    if (rec->slot_index >= 0 && rec->slot_index < SYNC_MAX_SLOTS) {
        if (!sync_master_slot_matches(state, rec->slot_index, rec->id)) {
            (void)sync_master_assign_slot_locked(state, rec, rec->slot_index);
        } else if (state->slot_generation[rec->slot_index] <= 0) {
            state->slot_generation[rec->slot_index] = 1;
        }
        return rec->slot_index;
    }

    for (int i = 0; i < SYNC_MAX_SLOTS; i++) {
        if (sync_master_slot_matches(state, i, rec->id)) {
            (void)sync_master_assign_slot_locked(state, rec, i);
            return i;
        }
    }

    for (int i = 0; i < SYNC_MAX_SLOTS; i++) {
        if (state->slot_assignees[i][0]) continue;
        (void)sync_master_assign_slot_locked(state, rec, i);
        return i;
    }
    return -1;
}

static void sync_master_apply_slot_assignment_locked(sync_master_state_t *state,
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
        if (rec) {
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
                trim(tok);
                if (*tok) json_array_append_string(arr, tok);
            }
            json_object_set_value(obj, "caps", caps);
        }
        json_object_set_number(obj, "ack_generation", sync_slave_get_applied_generation(app));

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
        sync_slave_set_current_slot(app, slot_number, label_checked);

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
                sync_slave_set_last_received(app, generation);
                sync_slave_set_applied_generation(app, generation);
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

static int h_options_all(struct mg_connection *c, void *ud){
    (void)ud;
    const struct mg_request_info *ri = mg_get_request_info(c);
    if (strcmp(ri->request_method, "OPTIONS") != 0) return 0;
    add_cors_options(c);
    return 1;
}

static void run_startup_exec_sequence(app_t *app) {
    if (!app) return;

    config_t cfg; app_config_snapshot(app, &cfg);
    if (cfg.startup_exec_count <= 0) return;

    fprintf(stderr, "running %d startup exec command(s)\n",
            cfg.startup_exec_count);

    for (int i = 0; i < cfg.startup_exec_count; i++) {
        const char *raw = cfg.startup_exec[i].json;
        if (!raw[0]) continue;

        JSON_Value *cmd = json_parse_string(raw);
        if (!cmd || json_value_get_type(cmd) != JSONObject) {
            fprintf(stderr,
                    "startup exec[%d]: ignored malformed payload '%s'\n",
                    i + 1, raw);
            if (cmd) json_value_free(cmd);
            continue;
        }

        JSON_Object *obj = json_object(cmd);
        const char *path = json_object_get_string(obj, "path");
        JSON_Array *args = json_object_get_array(obj, "args");
        if (!path || !*path) {
            fprintf(stderr,
                    "startup exec[%d]: missing path in payload '%s'\n",
                    i + 1, raw);
            json_value_free(cmd);
            continue;
        }

        char *out = NULL;
        char *err = NULL;
        int rc = 0;
        long long elapsed = 0;
        int r = run_exec(&cfg, path, args, cfg.exec_timeout_ms,
                         cfg.max_output_bytes, &rc, &elapsed, &out, &err);
        if (r == 0) {
            fprintf(stderr,
                    "startup exec[%d]: %s rc=%d elapsed=%lldms\n",
                    i + 1, path, rc, elapsed);
            if (out && *out) {
                fprintf(stderr, "  stdout: %s\n", out);
            }
            if (err && *err) {
                fprintf(stderr, "  stderr: %s\n", err);
            }
        } else {
            fprintf(stderr,
                    "startup exec[%d]: failed to run %s\n",
                    i + 1, path);
        }
        if (out) free(out);
        if (err) free(err);
        json_value_free(cmd);
    }
}

static int h_health(struct mg_connection *c, void *ud){
    (void)ud;
    JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
    json_object_set_string(o,"status","ok");
    send_json(c, v, 200, 1);
    json_value_free(v);
    return 1;
}

static int stream_file(struct mg_connection *c, const char *path, int cors_public, int json_on_missing){
    const struct mg_request_info *ri = mg_get_request_info(c);
    const char *method = (ri && ri->request_method) ? ri->request_method : "";
    int is_head = (strcmp(method, "HEAD") == 0);
    if (!is_head && strcmp(method, "GET") != 0) {
        send_plain(c, 405, "method_not_allowed", cors_public);
        return 1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (json_on_missing) {
            JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
            json_object_set_string(o,"error","ui_not_found");
            send_json(c, v, 404, cors_public);
            json_value_free(v);
        } else {
            send_plain(c, 404, "not_found", cors_public);
        }
        return 1;
    }

    struct stat st;
    if (fstat(fd,&st)!=0 || !S_ISREG(st.st_mode)){
        close(fd);
        send_plain(c, 404, "not_found", cors_public);
        return 1;
    }

    const char *ctype = guess_mime_type(path);
    char extra[192];
    extra[0] = '\0';
    char http_date[64];
    if (format_http_date(st.st_mtime, http_date, sizeof(http_date)) == 0) {
        int n = snprintf(extra, sizeof(extra), "Last-Modified: %s\r\n", http_date);
        if (n < 0 || n >= (int)sizeof(extra)) extra[0] = '\0';
    }

    add_common_headers_extra(c, 200, ctype, (size_t)st.st_size, cors_public,
                             extra[0] ? extra : NULL);

    if (is_head) {
        close(fd);
        return 1;
    }

    off_t off=0; char buf[64*1024];
    while (off < st.st_size) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        mg_write(c, buf, (size_t)r);
        off += r;
    }
    close(fd);
    return 1;
}

static int h_media(struct mg_connection *c, void *ud) {
    app_t *app = (app_t *)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    if (!cfg_has_cap(&cfg, "dvr")) {
        send_plain(c, 404, "not_found", cfg.ui_public);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(c);
    if (!ri || !ri->request_method) return 0;

    int is_head = (strcmp(ri->request_method, "HEAD") == 0);
    if (!is_head && strcmp(ri->request_method, "GET") != 0) {
        send_plain(c, 405, "method_not_allowed", cfg.ui_public);
        return 1;
    }

    const char *uri = ri->local_uri ? ri->local_uri : ri->request_uri;
    if (!uri) return 0;

    const char *prefix = "/media/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) {
        if (!strcmp(uri, "/media") || !strcmp(uri, "/media/")) {
            send_plain(c, 404, "not_found", cfg.ui_public);
            return 1;
        }
        return 0;
    }

    const char *rel = uri + prefix_len;
    while (*rel == '/') rel++;
    if (!*rel) {
        send_plain(c, 404, "not_found", cfg.ui_public);
        return 1;
    }

    char decoded[PATH_MAX];
    int dec_len = mg_url_decode(rel, (int)strlen(rel), decoded, (int)sizeof(decoded), 0);
    if (dec_len <= 0 || dec_len >= (int)sizeof(decoded)) {
        send_plain(c, 400, "bad_request", cfg.ui_public);
        return 1;
    }
    decoded[dec_len] = '\0';

    const char *base = getenv("DVR_MEDIA_DIR");
    if (!base || !*base) base = "/media";

    char base_real[PATH_MAX];
    if (!realpath(base, base_real)) {
        send_plain(c, 404, "media_unavailable", cfg.ui_public);
        return 1;
    }

    char joined[PATH_MAX];
    if (snprintf(joined, sizeof(joined), "%s/%s", base_real, decoded) >= (int)sizeof(joined)) {
        send_plain(c, 400, "path_too_long", cfg.ui_public);
        return 1;
    }

    char resolved[PATH_MAX];
    if (!realpath(joined, resolved)) {
        send_plain(c, 404, "not_found", cfg.ui_public);
        return 1;
    }

    size_t base_len = strlen(base_real);
    if (strncmp(resolved, base_real, base_len) != 0 ||
        (resolved[base_len] != '\0' && resolved[base_len] != '/')) {
        send_plain(c, 403, "forbidden", cfg.ui_public);
        return 1;
    }

    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        send_plain(c, 404, "not_found", cfg.ui_public);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        send_plain(c, 404, "not_found", cfg.ui_public);
        return 1;
    }

    const char *ctype = "application/octet-stream";
    const char *dot = strrchr(resolved, '.');
    if (dot && (!strcasecmp(dot, ".mp4") || !strcasecmp(dot, ".m4v"))) {
        ctype = "video/mp4";
    }

    char extra[192];
    extra[0] = '\0';
    char http_date[64];
    if (format_http_date(st.st_mtime, http_date, sizeof(http_date)) == 0) {
        int n = snprintf(extra, sizeof(extra), "Last-Modified: %s\r\n", http_date);
        if (n < 0 || n >= (int)sizeof(extra)) extra[0] = '\0';
    }

    add_common_headers_extra(c, 200, ctype, (size_t)st.st_size, cfg.ui_public,
                             extra[0] ? extra : NULL);

    if (is_head) {
        close(fd);
        return 1;
    }

    off_t off = 0;
    char buf[64 * 1024];
    while (off < st.st_size) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        mg_write(c, buf, (size_t)r);
        off += r;
    }
    close(fd);
    return 1;
}

static int h_root(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    if(!cfg.serve_ui || !cfg.ui_path[0]){
        JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
        json_object_set_string(o,"error","no_ui");
        send_json(c, v, 404, cfg.ui_public);
        json_value_free(v);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(c);
    const char *method = (ri && ri->request_method) ? ri->request_method : "";
    int is_head = (strcmp(method, "HEAD") == 0);
    if (!is_head && strcmp(method, "GET") != 0) {
        send_plain(c, 405, "method_not_allowed", cfg.ui_public);
        return 1;
    }

    const char *req_uri = (ri && ri->local_uri) ? ri->local_uri :
                          (ri && ri->request_uri) ? ri->request_uri : "/";
    if (!req_uri) req_uri = "/";

    char decoded_uri[PATH_MAX];
    int dec = mg_url_decode(req_uri, (int)strlen(req_uri),
                            decoded_uri, (int)sizeof(decoded_uri), 0);
    if (dec <= 0 || dec >= (int)sizeof(decoded_uri)) {
        send_plain(c, 400, "bad_request", cfg.ui_public);
        return 1;
    }
    decoded_uri[dec] = '\0';
    const char *uri = decoded_uri;

    const char *basename = cfg.ui_path;
    const char *slash = strrchr(basename, '/');
    if (slash && slash[1]) basename = slash + 1;

    if (!strcmp(uri, "/") ||
        (basename && *basename && uri[0]=='/' && strcmp(uri + 1, basename) == 0)) {
        return stream_file(c, cfg.ui_path, cfg.ui_public, 1);
    }

    const char *rel = uri;
    while (*rel == '/') rel++;
    if (!*rel) {
        return stream_file(c, cfg.ui_path, cfg.ui_public, 1);
    }

    char rel_copy[PATH_MAX];
    strncpy(rel_copy, rel, sizeof(rel_copy) - 1);
    rel_copy[sizeof(rel_copy) - 1] = '\0';

    char tmp[PATH_MAX];
    strncpy(tmp, rel_copy, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, "..")) {
            send_plain(c, 403, "forbidden", cfg.ui_public);
            return 1;
        }
    }

    char base_dir[PATH_MAX];
    strncpy(base_dir, cfg.ui_path, sizeof(base_dir) - 1);
    base_dir[sizeof(base_dir) - 1] = '\0';
    char *last = strrchr(base_dir, '/');
    if (last) {
        if (last == base_dir) {
            last[1] = '\0';
        } else {
            *last = '\0';
        }
    } else {
        snprintf(base_dir, sizeof(base_dir), ".");
    }

    char base_real[PATH_MAX];
    if (!realpath(base_dir, base_real)) {
        send_plain(c, 404, "not_found", cfg.ui_public);
        return 1;
    }

    char joined[PATH_MAX];
    if (snprintf(joined, sizeof(joined), "%s/%s", base_real, rel_copy) >= (int)sizeof(joined)) {
        send_plain(c, 400, "path_too_long", cfg.ui_public);
        return 1;
    }

    char resolved[PATH_MAX];
    if (realpath(joined, resolved)) {
        size_t base_len = strlen(base_real);
        if (strncmp(resolved, base_real, base_len) != 0 ||
            (resolved[base_len] != '\0' && resolved[base_len] != '/')) {
            send_plain(c, 403, "forbidden", cfg.ui_public);
            return 1;
        }
        return stream_file(c, resolved, cfg.ui_public, 0);
    }

    return stream_file(c, joined, cfg.ui_public, 0);
}

static int h_caps(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
    if(cfg.device[0])  json_object_set_string(o,"device",cfg.device);
    if(cfg.role[0])    json_object_set_string(o,"role",cfg.role);
    if(cfg.version[0]) json_object_set_string(o,"version",cfg.version);

    JSON_Value *caps_val = NULL;
    JSON_Array *caps_arr = NULL;
    if(cfg.caps[0]){
        caps_val = json_value_init_array();
        caps_arr = json_array(caps_val);
        char tmp[256]; strncpy(tmp,cfg.caps,sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
        char *tok,*save=NULL; for(tok=strtok_r(tmp,",",&save); tok; tok=strtok_r(NULL,",",&save)){ trim(tok); if(*tok) json_array_append_string(caps_arr,tok); }
    }

    if (cfg.sync_role[0]) {
        const char *sync_cap = (strcasecmp(cfg.sync_role, "master") == 0) ? "sync-master" : "sync-slave";
        if (!caps_arr) {
            caps_val = json_value_init_array();
            caps_arr = json_array(caps_val);
        }
        json_array_append_string(caps_arr, sync_cap);
    }

    if (caps_arr) {
        json_object_set_value(o,"caps",caps_val);
    }

    json_add_runtime(o);
    if(cfg.include_net_info) json_add_ifaddrs(o);
    json_object_set_number(o,"port",cfg.port);

    if (cfg.sse_count>0){
        JSON_Value *a=json_value_init_array(); JSON_Array *ar=json_array(a);
        for(int i=0;i<cfg.sse_count;i++){
            JSON_Value *e=json_value_init_object(); JSON_Object *eo=json_object(e);
            json_object_set_string(eo,"name",cfg.sse[i].name);
            char resolved[256];
            substitute_ip_placeholder(c, cfg.sse[i].url, resolved, sizeof(resolved));
            json_object_set_string(eo,"url", resolved);
            json_array_append_value(ar,e);
        }
        json_object_set_value(o,"sse",a);
    }
    if(cfg.serve_ui && cfg.ui_path[0]){
        JSON_Value *ui=json_value_init_object(); JSON_Object *uo=json_object(ui);
        json_object_set_string(uo,"path",cfg.ui_path);
        json_object_set_number(uo,"public",cfg.ui_public);
        json_object_set_value(o,"ui",ui);
    }
    json_object_set_number(o,"scan_feature_enabled", cfg.enable_scan ? 1 : 0);

    if (cfg.sync_role[0]) {
        JSON_Value *sync_v = json_value_init_object();
        JSON_Object *so = json_object(sync_v);
        json_object_set_string(so, "role", cfg.sync_role);
        if (cfg.sync_id[0]) json_object_set_string(so, "id", cfg.sync_id);
        json_object_set_number(so, "allow_bind", cfg.sync_allow_bind ? 1 : 0);
        json_object_set_number(so, "active_override_generation", app->active_override_generation);
        if (strcasecmp(cfg.sync_role, "slave") == 0) {
            if (cfg.sync_master_url[0]) json_object_set_string(so, "master_url", cfg.sync_master_url);
            json_object_set_number(so, "register_interval_s", cfg.sync_register_interval_s);
            json_object_set_number(so, "last_received_generation", sync_slave_get_last_received(app));
            json_object_set_number(so, "applied_generation", sync_slave_get_applied_generation(app));
            int cur_slot = sync_slave_get_current_slot(app);
            if (cur_slot > 0) {
                json_object_set_number(so, "slot", cur_slot);
                char slot_label_buf[64];
                slot_label_buf[0] = '\0';
                sync_slave_get_current_slot_label(app, slot_label_buf,
                                                  sizeof(slot_label_buf));
                if (slot_label_buf[0]) {
                    json_object_set_string(so, "slot_label", slot_label_buf);
                }
            }
        }
        json_object_set_value(o, "sync", sync_v);
    }

    int cors = cfg.ui_public;
    send_json(c, v, 200, cors);
    json_value_free(v);
    return 1;
}

static int h_exec(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    upload_t u={0};
    int rb = read_body(c, &u);
    if (rb != 0) {
        if (u.body) { free(u.body); u.body = NULL; }
        JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
        int status = 400;
        const char *err = "body_read_failed";
        const char *cl = mg_get_header(c, "Content-Length");
        if (cl && *cl) {
            size_t need = (size_t)strtoul(cl, NULL, 10);
            if (need > (size_t)MAX_BODY_BYTES) {
                status = 413;
                err = "body_too_large";
            }
        }
        json_object_set_string(o,"error",err);
        send_json(c, v, status, 1);
        json_value_free(v);
        return 1;
    }
    JSON_Value *root=json_parse_string(u.body?u.body:"{}");
    free(u.body);
    if(!root){
        JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
        json_object_set_string(o,"error","bad_json");
        send_json(c, v, 400, 1); json_value_free(v); return 1;
    }
    JSON_Object *o=json_object(root);
    const char *path=json_object_get_string(o,"path");
    JSON_Array  *args=json_object_get_array(o,"args");
    if(!path){
        JSON_Value *v=json_value_init_object(); JSON_Object *oo=json_object(v);
        json_object_set_string(oo,"error","missing_path");
        send_json(c, v, 400, 1); json_value_free(v); json_value_free(root); return 1;
    }
    int rc=0; long long elapsed=0; char *out=NULL,*err=NULL;
    int exec_r=run_exec(&cfg, path, args, cfg.exec_timeout_ms, cfg.max_output_bytes, &rc,&elapsed,&out,&err);
    JSON_Value *resp=json_value_init_object(); JSON_Object *or=json_object(resp);
    if(exec_r==0){
        json_object_set_number(or,"rc",rc);
        json_object_set_number(or,"elapsed_ms",(double)elapsed);
        json_object_set_string(or,"stdout", out?out:"");
        json_object_set_string(or,"stderr", err?err:"");
        free(out); free(err);
        send_json(c, resp, 200, 1);
    } else {
        json_object_set_string(or,"error","exec_failed");
        send_json(c, resp, 500, 1);
    }
    json_value_free(resp); json_value_free(root); return 1;
}

static int h_udp(struct mg_connection *c, void *ud) {
    (void)ud;
    const struct mg_request_info *ri = mg_get_request_info(c);
    if (!ri || strcmp(ri->request_method, "POST") != 0) {
        send_plain(c, 405, "method_not_allowed", 1);
        return 1;
    }

    upload_t u = {0};
    int rb = read_body(c, &u);
    if (rb != 0) {
        if (u.body) { free(u.body); u.body = NULL; }
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        int status = 400;
        const char *err = "body_read_failed";
        const char *cl = mg_get_header(c, "Content-Length");
        if (cl && *cl) {
            size_t need = (size_t)strtoul(cl, NULL, 10);
            if (need > (size_t)MAX_BODY_BYTES) {
                status = 413;
                err = "body_too_large";
            }
        }
        json_object_set_string(o, "error", err);
        send_json(c, v, status, 1);
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
    JSON_Value *host_v = json_object_get_value(obj, "host");
    JSON_Value *port_v = json_object_get_value(obj, "port");
    JSON_Value *payload_v = json_object_get_value(obj, "payload");
    JSON_Value *payload_b64_v = json_object_get_value(obj, "payload_base64");

    const char *host = (host_v && json_value_get_type(host_v) == JSONString)
                       ? json_value_get_string(host_v)
                       : NULL;
    double port_d = (port_v && json_value_get_type(port_v) == JSONNumber)
                    ? json_value_get_number(port_v)
                    : -1.0;
    int port = (int)port_d;

    int has_payload = (payload_v && json_value_get_type(payload_v) == JSONString) ? 1 : 0;
    int has_payload_b64 = (payload_b64_v && json_value_get_type(payload_b64_v) == JSONString) ? 1 : 0;

    if (!host || !*host || port_d < 1.0 || port_d > 65535.0 || (double)port != port_d ||
        (!has_payload && !has_payload_b64) || (has_payload && has_payload_b64)) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "invalid_request");
        send_json(c, v, 400, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    const unsigned char *data = NULL;
    unsigned char *tmp = NULL;
    size_t data_len = 0;

    if (has_payload_b64) {
        const char *payload_b64 = json_value_get_string(payload_b64_v);
        size_t src_len = payload_b64 ? strlen(payload_b64) : 0;
        size_t dst_cap = ((src_len / 4) + 1) * 3;
        if (dst_cap == 0) dst_cap = 1;
        tmp = (unsigned char *)malloc(dst_cap);
        if (!tmp) {
            JSON_Value *v = json_value_init_object();
            JSON_Object *o = json_object(v);
            json_object_set_string(o, "error", "oom");
            send_json(c, v, 500, 1);
            json_value_free(v);
            json_value_free(root);
            return 1;
        }
        size_t out_len = dst_cap;
        if (src_len == 0) {
            out_len = 0;
        } else {
            if (mg_base64_decode(payload_b64, src_len, tmp, &out_len) != -1) {
                free(tmp);
                JSON_Value *v = json_value_init_object();
                JSON_Object *o = json_object(v);
                json_object_set_string(o, "error", "invalid_base64");
                send_json(c, v, 400, 1);
                json_value_free(v);
                json_value_free(root);
                return 1;
            }
        }
        data = tmp;
        data_len = out_len;
    } else {
        const char *payload = json_value_get_string(payload_v);
        data = (const unsigned char *)(payload ? payload : "");
        data_len = strlen((const char *)data);
    }

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, portbuf, &hints, &res);
    if (gai != 0) {
        if (tmp) free(tmp);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "resolve_failed");
        const char *detail = gai_strerror(gai);
        if (detail && *detail) json_object_set_string(o, "detail", detail);
        send_json(c, v, 502, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    int sent_ok = 0;
    ssize_t sent_bytes = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        ssize_t r = sendto(fd, data, data_len, 0, ai->ai_addr, ai->ai_addrlen);
        int saved_errno = errno;
        close(fd);
        if (r >= 0) {
            sent_ok = 1;
            sent_bytes = r;
            break;
        }
        errno = saved_errno;
    }
    freeaddrinfo(res);

    if (!sent_ok) {
        int saved_errno = errno;
        if (tmp) free(tmp);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "send_failed");
        if (saved_errno) {
            json_object_set_string(o, "detail", strerror(saved_errno));
        }
        send_json(c, v, 502, 1);
        json_value_free(v);
        json_value_free(root);
        errno = saved_errno;
        return 1;
    }

    JSON_Value *resp = json_value_init_object();
    JSON_Object *or = json_object(resp);
    json_object_set_string(or, "status", "sent");
    json_object_set_number(or, "bytes_sent", (double)(sent_bytes >= 0 ? sent_bytes : 0));
    json_object_set_number(or, "payload_length", (double)data_len);
    json_object_set_string(or, "host", host);
    json_object_set_number(or, "port", (double)port);
    send_json(c, resp, 200, 1);
    json_value_free(resp);

    if (tmp) free(tmp);
    json_value_free(root);
    return 1;
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

    if (ack_generation > 0) {
        if (ack_generation > rec->last_ack_generation) {
            rec->last_ack_generation = ack_generation;
        }
    }
    assigned_slot = sync_master_auto_assign_slot_locked(&app->master, rec, &cfg);
    if (assigned_slot >= 0) {
        slot_generation = app->master.slot_generation[assigned_slot];
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
        json_array_append_value(arr, item);
    }
    pthread_mutex_unlock(&app->master.lock);

    json_object_set_value(ro, "slaves", arr_v);
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

    replay_slot_request_t replay_slot_requests[SYNC_MAX_SLOTS];
    int replay_slot_count = 0;
    replay_id_request_t replay_id_requests[SYNC_MAX_SLOTS];
    int replay_id_count = 0;

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

    int has_replay_requests = (replay_slot_count > 0 || replay_id_count > 0);

    if (move_count == 0 && !has_replay_requests) {
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
            sync_master_apply_slot_assignment_locked(&app->master, slot, new_id);
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
    pthread_mutex_unlock(&app->cfg_lock);
    sync_slave_reset_tracking(app);

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

/* ----------------------- /nodes endpoint (via scan.*) ----------------------- */
static int h_nodes(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
    const struct mg_request_info *ri = mg_get_request_info(c);

    if (!strcmp(ri->request_method, "POST")) {
        if (!cfg.enable_scan) {
            JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
            json_object_set_string(o,"error","scan_disabled");
            send_json(c, v, 400, 1); json_value_free(v); return 1;
        }

        if (scan_is_running()) {
            scan_status_t st; scan_get_status(&st);
            JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
            json_object_set_string(o,"rescan","already_running");
            json_object_set_number(o,"scanning", st.scanning);
            json_object_set_number(o,"targets",  st.targets);
            json_object_set_number(o,"done",     st.done);
            json_object_set_number(o,"progress_pct", st.progress_pct);
            json_object_set_number(o,"last_started",  st.last_started);
            json_object_set_number(o,"last_finished", st.last_finished);
            send_json(c, v, 202, 1); json_value_free(v); return 1;
        }

        scan_config_t scfg; fill_scan_config(&cfg, &scfg);
        (void)scan_start_async(&scfg);

        scan_status_t st; scan_get_status(&st);
        JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
        json_object_set_string(o,"rescan","started");
        json_object_set_number(o,"scanning", st.scanning);
        json_object_set_number(o,"targets",  st.targets);
        json_object_set_number(o,"done",     st.done);
        json_object_set_number(o,"progress_pct", st.progress_pct);
        json_object_set_number(o,"last_started",  st.last_started);
        json_object_set_number(o,"last_finished", st.last_finished);
        send_json(c, v, 202, 1);
        json_value_free(v);
        return 1;
    }

    // GET
    scan_node_t nodes[SCAN_MAX_NODES];
    int n = scan_get_nodes(nodes, SCAN_MAX_NODES);
    scan_status_t st; scan_get_status(&st);

    JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
    JSON_Value *arrv=json_value_init_array(); JSON_Array *arr=json_array(arrv);

    for (int i=0;i<n;i++){
        JSON_Value *nv=json_value_init_object(); JSON_Object *no=json_object(nv);
        json_object_set_string(no,"ip", nodes[i].ip);
        json_object_set_number(no,"port", nodes[i].port);
        if (nodes[i].role[0])    json_object_set_string(no,"role", nodes[i].role);
        if (nodes[i].device[0])  json_object_set_string(no,"device", nodes[i].device);
        if (nodes[i].version[0]) json_object_set_string(no,"version", nodes[i].version);
        json_object_set_number(no,"last_seen", nodes[i].last_seen);
        json_array_append_value(arr, nv);
    }

    json_object_set_value(o,"nodes", arrv);
    json_object_set_number(o,"scan_feature_enabled", cfg.enable_scan ? 1 : 0);
    json_object_set_number(o,"scanning", st.scanning);
    json_object_set_number(o,"targets",  st.targets);
    json_object_set_number(o,"done",     st.done);
    json_object_set_number(o,"progress_pct", st.progress_pct);
    json_object_set_number(o,"last_started",  st.last_started);
    json_object_set_number(o,"last_finished", st.last_finished);

    send_json(c, v, 200, 1);
    json_value_free(v);
    return 1;
}

/* ----------------------- main ----------------------- */

int main(int argc, char **argv){
    const char *cfgpath = "./autod.conf";
    for (int i=1; i<argc; i++) {
        if (argv[i][0] != '-') { cfgpath = argv[i]; }
    }

    app_t app; memset(&app, 0, sizeof(app));
    pthread_mutex_init(&app.cfg_lock, NULL);
    sync_master_init_state(&app.master);
    sync_slave_init_state(&app.slave);
    app.active_overrides = NULL;
    app.active_override_generation = 0;

    cfg_defaults(&app.base_cfg);
    if (parse_ini(cfgpath, &app.base_cfg) < 0) {
        fprintf(stderr, "WARN: could not read %s, using defaults\n", cfgpath);
    }

    pthread_mutex_lock(&app.cfg_lock);
    app.cfg = app.base_cfg;
    ensure_sync_id(&app.cfg);
    pthread_mutex_unlock(&app.cfg_lock);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    config_t cfg_snapshot; app_config_snapshot(&app, &cfg_snapshot);

    /* CivetWeb options */
    char portbuf[16]; snprintf(portbuf, sizeof(portbuf), "%d", cfg_snapshot.port);
    char lp[96];
    if (strcmp(cfg_snapshot.bind_addr,"0.0.0.0")==0) snprintf(lp, sizeof(lp), "%s", portbuf);
    else snprintf(lp, sizeof(lp), "%s:%s", cfg_snapshot.bind_addr, portbuf);

    const char *options[] = {
        "listening_ports", lp,
        "enable_keep_alive", "yes",
        "num_threads", "2",
        NULL
    };

    struct mg_callbacks cbs; memset(&cbs, 0, sizeof(cbs));
    cbs.log_message = log_civet_message;
    struct mg_init_data init = {0};
    init.callbacks = &cbs;
    init.user_data = &app;
    init.configuration_options = options;

    struct mg_error_data err = {0};
    char errbuf[256];
    err.text = errbuf;
    err.text_buffer_size = sizeof(errbuf);
    errbuf[0] = '\0';

    app.ctx = mg_start2(&init, &err);
    if(!app.ctx){
        if (err.code != MG_ERROR_DATA_CODE_OK) {
            fprintf(stderr,"ERROR: mg_start failed: %s (code=%u sub=%u)\n",
                    errbuf[0] ? errbuf : "unknown error", err.code, err.code_sub);
        } else {
            fprintf(stderr,"ERROR: mg_start failed\n");
        }
        return 1;
    }

    /* Install handlers */
    mg_set_request_handler(app.ctx, "/health",  h_health,        &app);
    mg_set_request_handler(app.ctx, "/caps",    h_caps,          &app);
    mg_set_request_handler(app.ctx, "/exec",    h_exec,          &app);
    mg_set_request_handler(app.ctx, "/udp",     h_udp,           &app);
    mg_set_request_handler(app.ctx, "/nodes",   h_nodes,         &app);
    mg_set_request_handler(app.ctx, "/media",   h_media,         &app);
    mg_set_request_handler(app.ctx, "/sync/register", h_sync_register, &app);
    mg_set_request_handler(app.ctx, "/sync/slaves",   h_sync_slaves,   &app);
    mg_set_request_handler(app.ctx, "/sync/push",     h_sync_push,     &app);
    mg_set_request_handler(app.ctx, "/sync/bind",     h_sync_bind,     &app);
    mg_set_request_handler(app.ctx, "/",        h_root,    &app);

    /* CORS preflight */
    mg_set_request_handler(app.ctx, "**", h_options_all, &app);

    fprintf(stderr,"autod listening on %s:%d (scan %s)\n",
            cfg_snapshot.bind_addr, cfg_snapshot.port, cfg_snapshot.enable_scan?"ENABLED":"disabled");

    // ---- Scanner: seed + optional autostart
    scan_init();
    scan_config_t scfg; fill_scan_config(&cfg_snapshot, &scfg);
    scan_seed_self_nodes(&scfg);
    if (cfg_snapshot.enable_scan) (void)scan_start_async(&scfg);

    if (strcasecmp(cfg_snapshot.sync_role, "slave") == 0) {
        pthread_mutex_lock(&app.slave.lock);
        app.slave.stop = 0;
        if (!app.slave.running) {
            if (pthread_create(&app.slave.thread, NULL, sync_slave_thread_main, &app) == 0) {
                app.slave.running = 1;
            } else {
                fprintf(stderr, "WARN: failed to start sync slave thread\n");
            }
        }
        pthread_mutex_unlock(&app.slave.lock);
    }

    run_startup_exec_sequence(&app);

    while(!g_stop) sleep(1);
    pthread_mutex_lock(&app.slave.lock);
    app.slave.stop = 1;
    pthread_mutex_unlock(&app.slave.lock);
    if (app.slave.running) {
        pthread_join(app.slave.thread, NULL);
    }
    mg_stop(app.ctx);
    return 0;
}
