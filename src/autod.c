/*
autod.c — lightweight HTTP control plane (CivetWeb, NO AUTH), with optional LAN scanner

With GUI
gcc -Os -std=c11 -Wall -Wextra -DNO_SSL -DNO_CGI -DNO_FILES -DUSE_SDL2_GUI -DUSE_SDL2_TTF \
    autod.c scan.c parson.c civetweb.c \
    -o autod -pthread \
    `pkg-config --libs --cflags sdl2` `pkg-config --libs --cflags SDL2_ttf`
strip autod

No GUI compile
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
#include <poll.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include "civetweb.h"
#include "parson.h"
#include "scan.h"        // <— NEW

#ifdef USE_SDL2_GUI
#include "autod_gui.h"
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
typedef struct {
    /* server */
    int  port;
    char bind_addr[64];
    int  enable_scan;      /* 0/1 — default off */

    /* exec */
    char interpreter[128];
    int  exec_timeout_ms;
    int  max_output_bytes;

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

    strncpy(c->interpreter, "/usr/bin/exec-handler.sh", sizeof(c->interpreter)-1);
    c->exec_timeout_ms = 5000;
    c->max_output_bytes = 65536;

    c->include_net_info = 1;
    c->sse_count = 0;
    c->serve_ui = 0;
    c->ui_public = 1;
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

        } else if (strcmp(sect,"ui")==0) {
            if (!strcmp(k,"ui_path"))   strncpy(cfg->ui_path,v,sizeof(cfg->ui_path)-1);
            else if (!strcmp(k,"serve_ui"))  cfg->serve_ui=atoi(v);
            else if (!strcmp(k,"ui_public")) cfg->ui_public=atoi(v);
        }
    }
    fclose(f);
    return 0;
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

/* ----------------------- Exec runner ----------------------- */
typedef struct { char *body; size_t len; } upload_t;

static int run_exec(const config_t *cfg, const char *path, JSON_Array *args,
                    int timeout_ms, int max_bytes,
                    int *rc_out, long long *elapsed_ms,
                    char **out_stdout, char **out_stderr)
{
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0 || pipe(err_pipe) < 0) return -1;

    long long t0 = now_ms();
    pid_t pid = fork();
    if (pid < 0) return -1;

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
    close(out_pipe[1]); close(err_pipe[1]);
    char *buf_out = malloc(max_bytes + 1);
    char *buf_err = malloc(max_bytes + 1);
    if (!buf_out || !buf_err) { if (buf_out) free(buf_out); if (buf_err) free(buf_err); return -1; }
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

    close(out_pipe[0]); close(err_pipe[0]);

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
}

/* ----------------------- CivetWeb helpers ----------------------- */

static void add_common_headers(struct mg_connection *c, int code, const char *ctype,
                               size_t clen, int cors_public) {
    mg_printf(c, "HTTP/1.1 %d OK\r\n", code);
    mg_printf(c, "Content-Type: %s\r\n", ctype ? ctype : "application/octet-stream");
    mg_printf(c, "Content-Length: %zu\r\n", clen);
    if (cors_public) {
        mg_printf(c, "Access-Control-Allow-Origin: *\r\n");
        mg_printf(c, "Vary: Origin\r\n");
    }
    mg_printf(c, "Cache-Control: no-store\r\n");
    mg_printf(c, "Connection: close\r\n\r\n");
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

static int read_body(struct mg_connection *c, upload_t *u) {
    const size_t MAX_BODY = 262144; /* 256 KiB guard */
    u->body = NULL;
    u->len  = 0;

    const char *cl = mg_get_header(c, "Content-Length");
    size_t need = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (need == 0) return 0;
    if (need > MAX_BODY) return -1;

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

/* ----------------------- HTTP Handlers ----------------------- */
typedef struct { config_t cfg; struct mg_context *ctx; } app_t;

static int h_options_all(struct mg_connection *c, void *ud){
    (void)ud;
    const struct mg_request_info *ri = mg_get_request_info(c);
    if (strcmp(ri->request_method, "OPTIONS") != 0) return 0;
    add_cors_options(c);
    return 1;
}

static int h_health(struct mg_connection *c, void *ud){
    (void)ud;
    JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
    json_object_set_string(o,"status","ok");
    send_json(c, v, 200, 1);
    json_value_free(v);
    return 1;
}

static int stream_file(struct mg_connection *c, const char *path, int cors_public){
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
        json_object_set_string(o,"error","ui_not_found");
        send_json(c, v, 404, cors_public);
        json_value_free(v);
        return 1;
    }
    struct stat st; if (fstat(fd,&st)!=0){ close(fd); return 0; }
    add_common_headers(c, 200, "text/html; charset=utf-8", (size_t)st.st_size, cors_public);
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

static int h_root(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    if(!app->cfg.serve_ui || !app->cfg.ui_path[0]){
        JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
        json_object_set_string(o,"error","no_ui");
        send_json(c, v, 404, app->cfg.ui_public);
        json_value_free(v);
        return 1;
    }
    return stream_file(c, app->cfg.ui_path, app->cfg.ui_public);
}

static int h_caps(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud; const config_t *cfg=&app->cfg;
    JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
    if(cfg->device[0])  json_object_set_string(o,"device",cfg->device);
    if(cfg->role[0])    json_object_set_string(o,"role",cfg->role);
    if(cfg->version[0]) json_object_set_string(o,"version",cfg->version);
    if(cfg->caps[0]){
        JSON_Value *a=json_value_init_array(); JSON_Array *ar=json_array(a);
        char tmp[256]; strncpy(tmp,cfg->caps,sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
        char *tok,*save=NULL; for(tok=strtok_r(tmp,",",&save); tok; tok=strtok_r(NULL,",",&save)){ trim(tok); if(*tok) json_array_append_string(ar,tok); }
        json_object_set_value(o,"caps",a);
    }
    json_add_runtime(o);
    if(cfg->include_net_info) json_add_ifaddrs(o);
    json_object_set_number(o,"port",cfg->port);

    if (cfg->sse_count>0){
        JSON_Value *a=json_value_init_array(); JSON_Array *ar=json_array(a);
        for(int i=0;i<cfg->sse_count;i++){
            JSON_Value *e=json_value_init_object(); JSON_Object *eo=json_object(e);
            json_object_set_string(eo,"name",cfg->sse[i].name);
            char resolved[256];
            substitute_ip_placeholder(c, cfg->sse[i].url, resolved, sizeof(resolved));
            json_object_set_string(eo,"url", resolved);
            json_array_append_value(ar,e);
        }
        json_object_set_value(o,"sse",a);
    }
    if(cfg->serve_ui && cfg->ui_path[0]){
        JSON_Value *ui=json_value_init_object(); JSON_Object *uo=json_object(ui);
        json_object_set_string(uo,"path",cfg->ui_path);
        json_object_set_number(uo,"public",cfg->ui_public);
        json_object_set_value(o,"ui",ui);
    }
    json_object_set_number(o,"scan_feature_enabled", cfg->enable_scan ? 1 : 0);

    send_json(c, v, 200, cfg->ui_public);
    json_value_free(v);
    return 1;
}

static int h_exec(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    upload_t u={0};
    if (read_body(c, &u) != 0) return 0;
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
    int exec_r=run_exec(&app->cfg, path, args, app->cfg.exec_timeout_ms, app->cfg.max_output_bytes, &rc,&elapsed,&out,&err);
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

/* ----------------------- /nodes endpoint (via scan.*) ----------------------- */
static int h_nodes(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    const struct mg_request_info *ri = mg_get_request_info(c);

    if (!strcmp(ri->request_method, "POST")) {
        if (!app->cfg.enable_scan) {
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

        scan_config_t scfg = {0};
        scfg.port = app->cfg.port;
        if (app->cfg.role[0])    strncpy(scfg.role,    app->cfg.role,    sizeof(scfg.role)-1);
        if (app->cfg.device[0])  strncpy(scfg.device,  app->cfg.device,  sizeof(scfg.device)-1);
        if (app->cfg.version[0]) strncpy(scfg.version, app->cfg.version, sizeof(scfg.version)-1);

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
    json_object_set_number(o,"scan_feature_enabled", app->cfg.enable_scan ? 1 : 0);
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

#ifdef USE_SDL2_GUI
static void gui_fill_snapshot(autod_gui_snapshot_t *s, void *user) {
    (void)user;
    scan_node_t nodes[SCAN_MAX_NODES];
    int n = scan_get_nodes(nodes, SCAN_MAX_NODES);
    s->count = (n < 64) ? n : 64;
    for (int i=0; i<s->count; i++) {
        strncpy(s->nodes[i].ip,      nodes[i].ip,      sizeof(s->nodes[i].ip)-1);
        strncpy(s->nodes[i].role,    nodes[i].role,    sizeof(s->nodes[i].role)-1);
        strncpy(s->nodes[i].device,  nodes[i].device,  sizeof(s->nodes[i].device)-1);
        strncpy(s->nodes[i].version, nodes[i].version, sizeof(s->nodes[i].version)-1);
    }
    scan_status_t st; scan_get_status(&st);
    s->scanning     = st.scanning;
    s->targets      = st.targets;
    s->done         = st.done;
    s->progress_pct = st.progress_pct;
}
#endif /* USE_SDL2_GUI */

/* ----------------------- main ----------------------- */

int main(int argc, char **argv){
 #ifdef USE_SDL2_GUI
    int want_gui = 0;
 #endif

    const char *cfgpath = "./autod.conf";
    for (int i=1; i<argc; i++) {
    #ifdef USE_SDL2_GUI
        if (!strcmp(argv[i], "--gui")) { want_gui = 1; continue; }
    #endif
        if (argv[i][0] != '-') { cfgpath = argv[i]; }
    }

    app_t app; memset(&app, 0, sizeof(app));
    cfg_defaults(&app.cfg);
    if (parse_ini(cfgpath, &app.cfg) < 0) {
        fprintf(stderr, "WARN: could not read %s, using defaults\n", cfgpath);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    /* CivetWeb options */
    char portbuf[16]; snprintf(portbuf, sizeof(portbuf), "%d", app.cfg.port);
    char lp[96];
    if (strcmp(app.cfg.bind_addr,"0.0.0.0")==0) snprintf(lp, sizeof(lp), "%s", portbuf);
    else snprintf(lp, sizeof(lp), "%s:%s", app.cfg.bind_addr, portbuf);

    const char *options[] = {
        "listening_ports", lp,
        "enable_keep_alive", "yes",
        "num_threads", "2",
        NULL
    };

    struct mg_callbacks cbs; memset(&cbs, 0, sizeof(cbs));
    app.ctx = mg_start(&cbs, &app, options);
    if(!app.ctx){ fprintf(stderr,"ERROR: mg_start failed\n"); return 1; }

    /* Install handlers */
    mg_set_request_handler(app.ctx, "/health",  h_health,  &app);
    mg_set_request_handler(app.ctx, "/caps",    h_caps,    &app);
    mg_set_request_handler(app.ctx, "/exec",    h_exec,    &app);
    mg_set_request_handler(app.ctx, "/nodes",   h_nodes,   &app);
    mg_set_request_handler(app.ctx, "/",        h_root,    &app);

    /* CORS preflight */
    mg_set_request_handler(app.ctx, "**", h_options_all, &app);

    fprintf(stderr,"autod listening on %s:%d (scan %s)\n",
            app.cfg.bind_addr, app.cfg.port, app.cfg.enable_scan?"ENABLED":"disabled");

    // ---- Scanner: seed + optional autostart
    scan_init();
    scan_config_t scfg = {0};
    scfg.port = app.cfg.port;
    if (app.cfg.role[0])    strncpy(scfg.role,    app.cfg.role,    sizeof(scfg.role)-1);
    if (app.cfg.device[0])  strncpy(scfg.device,  app.cfg.device,  sizeof(scfg.device)-1);
    if (app.cfg.version[0]) strncpy(scfg.version, app.cfg.version, sizeof(scfg.version)-1);


    scan_seed_self_nodes(&scfg);
    if (app.cfg.enable_scan) (void)scan_start_async(&scfg);

#ifdef USE_SDL2_GUI
    if (want_gui) {
        autod_gui_config_t gcfg = {0};
        gcfg.port = app.cfg.port;
        if (app.cfg.role[0])   strncpy(gcfg.role,   app.cfg.role,   sizeof(gcfg.role)-1);
        if (app.cfg.device[0]) strncpy(gcfg.device, app.cfg.device, sizeof(gcfg.device)-1);
        (void)autod_gui_start(&gcfg, gui_fill_snapshot, NULL);
    }
#endif

    while(!g_stop) sleep(1);
    mg_stop(app.ctx);
    return 0;
}
