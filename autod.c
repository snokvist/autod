/*
autod.c — lightweight HTTP control plane (CivetWeb, NO AUTH), with optional LAN scanner

Build example (IPv4, no TLS/CGI/file-server):
    gcc -Os -std=c11 -Wall -Wextra -DNO_SSL -DNO_CGI -DNO_FILES \
        autod.c parson.c civetweb.c -o autod -pthread
    strip autod

Deps next to this file:
    parson.c / parson.h
    civetweb.c / civetweb.h
    md5.inl match.inl response.inl handle_form.inl openssl_dl.inl (from CivetWeb)
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


#ifdef USE_SDL2_GUI
#include <SDL2/SDL.h>

#ifdef USE_SDL2_GUI
#ifdef USE_SDL2_TTF
#include <SDL_ttf.h>
static TTF_Font *g_font = NULL;  // <-- file-scope, shared with draw_text()
#endif

static void draw_text(SDL_Renderer *R, int x, int y, const char *s) {
    if (!g_font || !s || !*s) return;
    SDL_Color col = {255,255,255,255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font, s, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(R, surf);
    if (tex) {
        SDL_Rect dst = {x, y, surf->w, surf->h};
        SDL_RenderCopy(R, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
}
#endif
#endif


static volatile sig_atomic_t g_stop=0;
static void on_signal(int s){ (void)s; g_stop=1; }


/* ----------------------- Config (no auth) ----------------------- */
typedef struct {
    /* server */
    int  port;
    char bind_addr[64];
    int  enable_scan;      /* NEW: 0/1 — default off */

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

    /* announce SSE (kept for future; not used by core) */
    struct { char name[64]; char url[192]; } sse[16];
    int  sse_count;

    /* ui */
    char ui_path[256];      /* HTML file to serve at "/" */
    int  serve_ui;          /* 0/1 */
    int  ui_public;         /* 0 adds no CORS; 1 adds Access-Control-Allow-Origin: * */
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
    c->enable_scan = 0; /* default OFF */

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
/* ---- helper: get "host" from Host header (no port), fallback to 127.0.0.1 ---- */
static void get_request_host_only(struct mg_connection *c, char *out, size_t outlen) {
    const char *host = mg_get_header(c, "Host"); /* e.g. "192.168.86.30:55667" or "cam.local:55667" */
    if (!host || !*host) { strncpy(out, "127.0.0.1", outlen-1); out[outlen-1]='\0'; return; }
    const char *colon = strchr(host, ':');
    size_t n = colon ? (size_t)(colon - host) : strlen(host);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, host, n); out[n] = '\0';
}

/* ---- helper: if url contains "http://IP" or "{IP}", substitute with req host ---- */
static void substitute_ip_placeholder(struct mg_connection *c,
                                      const char *in, char *out, size_t outlen) {
    char hostonly[128]; get_request_host_only(c, hostonly, sizeof(hostonly));
    const char *p = strstr(in, "http://IP");
    const char *q = strstr(in, "{IP}");
    if (!p && !q) {
        /* nothing to do */
        strncpy(out, in, outlen-1); out[outlen-1]='\0';
        return;
    }
    if (p) {
        /* "http://IP" -> "http://<host>" */
        const char *after = in + strlen("http://IP");
        int n = snprintf(out, outlen, "http://%s%s", hostonly, after);
        if (n < 0 || (size_t)n >= outlen) out[outlen-1] = '\0';
        return;
    }
    if (q) {
        /* "{IP}" token anywhere */
        size_t prefix = (size_t)(q - in);
        size_t suffix = strlen(q + 4);
        if (prefix + strlen(hostonly) + suffix + 1 > outlen) {
            /* truncate safely */
        }
        snprintf(out, outlen, "%.*s%s%s", (int)prefix, in, hostonly, q + 4);
        return;
    }
}






static inline long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (long long)ts.tv_sec*1000LL + ts.tv_nsec/1000000LL;
}

static void json_add_runtime(JSON_Object *o) {
    FILE *f=fopen("/proc/uptime","r"); if(f){ double up=0; if(fscanf(f,"%lf",&up)==1) json_object_set_number(o,"uptime_s",up); fclose(f); }
    f=fopen("/proc/loadavg","r"); if(f){ double a,b,c; if(fscanf(f,"%lf %lf %lf",&a,&b,&c)==3){ JSON_Value *arr=json_value_init_array(); JSON_Array *ar=json_array(arr); json_array_append_number(ar,a); json_array_append_number(ar,b); json_array_append_number(ar,c); json_object_set_value(o,"loadavg",arr);} fclose(f);}
    f=fopen("/proc/meminfo","r"); if(f){ char k[64]; long v; while(fscanf(f,"%63[^:]: %ld kB\n",k,&v)==2){ if(!strcmp(k,"MemFree")) json_object_set_number(o,"memfree_kb",v); if(!strcmp(k,"MemAvailable")) json_object_set_number(o,"memavail_kb",v);} fclose(f); }
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
typedef struct { char *body; size_t len; } upload_t;

static void add_common_headers(struct mg_connection *c, int code, const char *ctype, size_t clen, int cors_public) {
    mg_printf(c, "HTTP/1.1 %d OK\r\n", code);
    mg_printf(c, "Content-Type: %s\r\n", ctype ? ctype : "application/octet-stream");
    mg_printf(c, "Content-Length: %zu\r\n", clen);
    if (cors_public) mg_printf(c, "Access-Control-Allow-Origin: *\r\n");
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
    const char *cl = mg_get_header(c, "Content-Length");
    size_t need = cl ? (size_t)strtoul(cl, NULL, 10) : 0;
    if (!need) { u->body=NULL; u->len=0; return 0; }
    char *buf = (char*)malloc(need+1);
    if (!buf) return -1;
    size_t got = 0;
    while (got < need) {
        int r = mg_read(c, buf+got, (int)(need-got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    buf[got] = '\0';
    u->body = buf;
    u->len = got;
    return (got==need)?0:-1;
}

static void send_json(struct mg_connection *c, JSON_Value *root, int status, int cors_public) {
    char *s=json_serialize_to_string(root); size_t len=strlen(s);
    add_common_headers(c, status, "application/json", len, cors_public);
    mg_write(c, s, len);
    json_free_serialized_string(s);
}

/* ----------------------- HTTP Handlers ----------------------- */
typedef struct { config_t cfg; struct mg_context *ctx; } app_t;

static int h_options_all(struct mg_connection *c, void *ud){
    (void)ud;
    const struct mg_request_info *ri = mg_get_request_info(c);
    if (strcmp(ri->request_method, "OPTIONS") != 0) return 0; /* not handled */
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
    /* Optional: expose whether scanning is even enabled */
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

static int h_control(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    upload_t u={0};
    if (read_body(c, &u) != 0) return 0;
    JSON_Value *root=json_parse_string(u.body?u.body:"{}");
    free(u.body);

    JSON_Object *o=root?json_object(root):NULL;
    const char *key=o?json_object_get_string(o,"key"):NULL;
    JSON_Value *valv=o?json_object_get_value(o,"value"):NULL;

    JSON_Value *resp=json_value_init_object(); JSON_Object *or=json_object(resp);
    if(key && valv){
        char *sv=json_serialize_to_string(valv);
        JSON_Value *argsv=json_value_init_array(); JSON_Array *args=json_array(argsv);
        json_array_append_string(args,key); json_array_append_string(args, sv?sv:"");
        if(sv) json_free_serialized_string(sv);

        int rc=0; long long elapsed=0; char *out=NULL,*err=NULL;
        int r=run_exec(&app->cfg, "/control", args, app->cfg.exec_timeout_ms, app->cfg.max_output_bytes, &rc,&elapsed,&out,&err);
        if(r==0){
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
        json_value_free(argsv);
    } else {
        json_object_set_string(or,"error","missing_key_or_value");
        send_json(c, resp, 400, 1);
    }
    if(root) json_value_free(root);
    json_value_free(resp);
    return 1;
}

/* ----------------------- Tiny HTTP client for scanner ----------------------- */
static int tcp_connect_nb(const char *ip, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) { close(fd); return -1; }

    int r = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (r == 0) return fd; /* connected immediately */

    if (errno == EINPROGRESS) {
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        r = poll(&p, 1, timeout_ms);
        if (r == 1 && (p.revents & POLLOUT)) {
            int err=0; socklen_t elen=sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen)==0 && err==0) return fd;
        }
    }
    close(fd);
    return -1;
}

static int http_get_simple(const char *ip, int port, const char *path,
                           char *buf, size_t buflen, int timeout_ms) {
    int fd = tcp_connect_nb(ip, port, timeout_ms);
    if (fd < 0) return -1;

    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                     path, ip);
    if (write(fd, req, n) != n) { close(fd); return -1; }

    /* Read until close or buffer full (non-blocking + poll) */
    size_t w = 0;
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, timeout_ms);
        if (pr <= 0) break;
        if (p.revents & POLLIN) {
            ssize_t r = read(fd, buf + w, (buflen - 1) - w);
            if (r > 0) { w += (size_t)r; if (w >= buflen - 1) break; }
            else break;
        } else break;
    }
    buf[w] = '\0';
    close(fd);

    /* Quick check for 200 */
    if (strncmp(buf, "HTTP/1.1 200", 12) != 0 && strncmp(buf, "HTTP/1.0 200", 12) != 0)
        return -2;
    return 0;
}

static const char* http_body_ptr(const char *resp) {
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? (p + 4) : NULL;
}

/* ----------------------- Nodes cache & scanner ----------------------- */
typedef struct {
    char ip[16];          /* "192.168.86.25" */
    int  port;            /* autod port (same as ours) */
    char role[64];        /* from /caps.role */
    char device[64];      /* from /caps.device */
    char version[32];     /* from /caps.version */
    double last_seen;     /* time(NULL) */
} node_info_t;

#define MAX_NODES 512
static node_info_t g_nodes[MAX_NODES];
static int         g_nodes_count = 0;
static pthread_mutex_t g_nodes_mx = PTHREAD_MUTEX_INITIALIZER;

/* scan state */
static volatile int g_scan_in_progress = 0;
static volatile unsigned g_scan_total = 0;
static volatile unsigned g_scan_done  = 0;
static volatile double  g_last_started = 0.0;
static volatile double  g_last_finished = 0.0;

static void scan_state_reset(unsigned total) {
    __sync_lock_test_and_set(&g_scan_total, total);
    __sync_lock_test_and_set(&g_scan_done, 0);
    g_last_started = (double)time(NULL);
    g_last_finished = 0.0;
}

static void scan_state_finish(void) {
    g_last_finished = (double)time(NULL);
}

static void nodes_reset(void) {
    pthread_mutex_lock(&g_nodes_mx);
    g_nodes_count = 0;
    pthread_mutex_unlock(&g_nodes_mx);
}

static void nodes_upsert(const node_info_t *ni) {
    pthread_mutex_lock(&g_nodes_mx);
    for (int i=0;i<g_nodes_count;i++) {
        if (strcmp(g_nodes[i].ip, ni->ip)==0 && g_nodes[i].port==ni->port) {
            g_nodes[i] = *ni; /* replace */
            pthread_mutex_unlock(&g_nodes_mx);
            return;
        }
    }
    if (g_nodes_count < MAX_NODES) g_nodes[g_nodes_count++] = *ni;
    pthread_mutex_unlock(&g_nodes_mx);
}

static int is_link_local(const char *ip) { return strncmp(ip, "169.254.", 8) == 0; }

static inline int progress_pct(void) {
    if (g_scan_total == 0) return 0;
    return (int)((100ULL * g_scan_done) / g_scan_total);  // integer truncation
}

static void add_self_nodes(const config_t *cfg) {
    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr)!=0) return;
    for (struct ifaddrs *ifa=ifaddr; ifa; ifa=ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        char ip[16];
        if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip))) continue;
        if (!strcmp(ip, "127.0.0.1")) continue; /* skip loopback here */
        node_info_t self = (node_info_t){0};
        strncpy(self.ip, ip, sizeof(self.ip)-1);
        self.port = cfg->port;
        if (cfg->role[0])    strncpy(self.role, cfg->role, sizeof(self.role)-1);
        if (cfg->device[0])  strncpy(self.device, cfg->device, sizeof(self.device)-1);
        if (cfg->version[0]) strncpy(self.version, cfg->version, sizeof(self.version)-1);
        self.last_seen = (double)time(NULL);
        nodes_upsert(&self);
    }
    freeifaddrs(ifaddr);
}

static unsigned count_iface_targets(const char *ip, const char *mask, uint32_t self_a) {
    struct in_addr ip4, m4;
    if (inet_pton(AF_INET, ip, &ip4)!=1 || inet_pton(AF_INET, mask, &m4)!=1) return 0;
    uint32_t a = ntohl(ip4.s_addr);
    uint32_t m = ntohl(m4.s_addr);
    if (m == 0xffffffffu) return 0; /* /32, nothing to scan */
    uint32_t net = a & m;
    uint32_t bcast = net | (~m);
    if (bcast <= net) return 0;
    unsigned cnt = 0;
    for (uint32_t h = net+1; h < bcast; h++) {
        if (h == a || h == self_a) continue;
        cnt++;
    }
    return cnt;
}

static void scan_iface(const char *ip, const char *mask, int port, uint32_t self_a) {
    struct in_addr ip4, m4;
    if (inet_pton(AF_INET, ip, &ip4)!=1 || inet_pton(AF_INET, mask, &m4)!=1) return;
    uint32_t a = ntohl(ip4.s_addr);
    uint32_t m = ntohl(m4.s_addr);
    if (m == 0xffffffffu) return; /* /32, nothing to scan */
    uint32_t net = a & m;
    uint32_t bcast = net | (~m);
    if (bcast <= net) return;

    /* guard huge subnets */
    if ((bcast - net + 1) > 1024) return;

    char resp[8192];
    for (uint32_t h = net+1; h < bcast; h++) {
        if (h == a || h == self_a) { __sync_add_and_fetch(&g_scan_done, 1); continue; }
        struct in_addr t; t.s_addr = htonl(h);
        char tip[16]; if (!inet_ntop(AF_INET, &t, tip, sizeof(tip))) { __sync_add_and_fetch(&g_scan_done, 1); continue; }
        if (is_link_local(tip)) { __sync_add_and_fetch(&g_scan_done, 1); continue; }

        /* /health quick check */
        int r = http_get_simple(tip, port, "/health", resp, sizeof(resp), 250);
        if (r == 0) {
            /* /caps for metadata */
            r = http_get_simple(tip, port, "/caps", resp, sizeof(resp), 400);
            if (r == 0) {
                const char *body = http_body_ptr(resp);
                if (body) {
                    JSON_Value *v = json_parse_string(body);
                    if (v) {
                        JSON_Object *o = json_object(v);
                        node_info_t ni = (node_info_t){0};
                        strncpy(ni.ip, tip, sizeof(ni.ip)-1);
                        ni.port = port;
                        const char *role = json_object_get_string(o, "role");
                        const char *device = json_object_get_string(o, "device");
                        const char *ver = json_object_get_string(o, "version");
                        if (role)   strncpy(ni.role, role, sizeof(ni.role)-1);
                        if (device) strncpy(ni.device, device, sizeof(ni.device)-1);
                        if (ver)    strncpy(ni.version, ver, sizeof(ni.version)-1);
                        ni.last_seen = (double)time(NULL);
                        nodes_upsert(&ni);
                        json_value_free(v);
                    }
                }
            }
        }
        __sync_add_and_fetch(&g_scan_done, 1);
    }
}

typedef struct { config_t *cfg; } scan_ctx_t;

static void *scan_thread(void *arg) {
    scan_ctx_t *sc = (scan_ctx_t*)arg;
    if (__sync_lock_test_and_set(&g_scan_in_progress, 1)) { free(sc); return NULL; } /* already running */

    /* compute total targets first (for progress), then run */
    unsigned total = 0;
    struct ifaddrs *ifaddr;
    uint32_t self_a = 0;
    /* capture first non-loopback address as "self_a" to avoid double counting — optional */
    if (getifaddrs(&ifaddr)==0) {
        for (struct ifaddrs *ifa=ifaddr; ifa; ifa=ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_netmask) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            char ip[16], mask[16];
            if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip))) continue;
            if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_netmask)->sin_addr, mask, sizeof(mask))) continue;
            if (!strcmp(ip, "127.0.0.1") || is_link_local(ip)) continue;

            if (self_a == 0) {
                struct in_addr tmp; inet_pton(AF_INET, ip, &tmp);
                self_a = ntohl(tmp.s_addr);
            }
            total += count_iface_targets(ip, mask, self_a);
        }
        freeifaddrs(ifaddr);
    }
    if (total > 1024) total = 1024; /* safety cap */

    /* reset state + cache */
    scan_state_reset(total);
    nodes_reset();
    add_self_nodes(sc->cfg);

    /* actual scan pass */
    if (getifaddrs(&ifaddr)==0) {
        for (struct ifaddrs *ifa=ifaddr; ifa; ifa=ifa->ifa_next) {
            if (!ifa->ifa_addr || !ifa->ifa_netmask) continue;
            if (ifa->ifa_addr->sa_family != AF_INET) continue;
            char ip[16], mask[16];
            if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip))) continue;
            if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_netmask)->sin_addr, mask, sizeof(mask))) continue;
            if (!strcmp(ip, "127.0.0.1") || is_link_local(ip)) continue;
            scan_iface(ip, mask, sc->cfg->port, self_a);
        }
        freeifaddrs(ifaddr);
    }

    scan_state_finish();
    __sync_lock_release(&g_scan_in_progress);
    free(sc);
    return NULL;
}

static void start_scan_async(config_t *cfg) {
    pthread_t th;
    scan_ctx_t *sc = malloc(sizeof(*sc));
    if (!sc) return;
    sc->cfg = cfg;
    if (pthread_create(&th, NULL, scan_thread, sc)==0) pthread_detach(th);
    else free(sc);
}


#ifdef USE_SDL2_GUI
typedef struct {
    int count;
    node_info_t nodes[64]; /* cap what we draw */
    int scanning;
    unsigned targets, done;
    int progress_pct;      /* integer 0..100, stable */
} gui_snapshot_t;

/* reuse your progress_pct() already defined above */
static void take_gui_snapshot(gui_snapshot_t *s) {
    pthread_mutex_lock(&g_nodes_mx);
    int n = (g_nodes_count < 64) ? g_nodes_count : 64;
    s->count = n;
    for (int i=0; i<n; i++) s->nodes[i] = g_nodes[i];
    pthread_mutex_unlock(&g_nodes_mx);

    s->scanning = g_scan_in_progress ? 1 : 0;
    s->targets  = g_scan_total;
    s->done     = g_scan_done;
    s->progress_pct = progress_pct();
}
#endif



/* ----------------------- /nodes endpoint ----------------------- */
static int h_nodes(struct mg_connection *c, void *ud){
    app_t *app=(app_t*)ud;
    const struct mg_request_info *ri = mg_get_request_info(c);

    if (!strcmp(ri->request_method, "POST")) {
        if (!app->cfg.enable_scan) {
            JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
            json_object_set_string(o,"error","scan_disabled");
            send_json(c, v, 400, 1); json_value_free(v); return 1;
        }
        if (g_scan_in_progress) {
            JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
            json_object_set_string(o,"rescan","already_running");
            json_object_set_number(o,"scanning", 1);
            json_object_set_number(o,"targets", g_scan_total);
            json_object_set_number(o,"done", g_scan_done);
            double prog = (g_scan_total > 0) ? ((double)g_scan_done / (double)g_scan_total) : 0.0;
            if (prog > 1.0) prog = 1.0;

            json_object_set_number(o, "progress_pct", progress_pct());
            json_object_set_number(o,"last_started", g_last_started);
            json_object_set_number(o,"last_finished", g_last_finished);
            send_json(c, v, 202, 1); json_value_free(v); return 1;
        }
        start_scan_async(&app->cfg);
        JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
        json_object_set_string(o,"rescan","started");
        json_object_set_number(o,"scanning", 1);
        json_object_set_number(o,"targets", g_scan_total);
        json_object_set_number(o,"done", g_scan_done);
        json_object_set_number(o,"progress_pct", 0);
        json_object_set_number(o,"last_started", g_last_started);
        json_object_set_number(o,"last_finished", g_last_finished);
        send_json(c, v, 202, 1);
        json_value_free(v);
        return 1;
    }

    /* GET: dump current cache + scan status */
    JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
    JSON_Value *arrv=json_value_init_array(); JSON_Array *arr=json_array(arrv);

    pthread_mutex_lock(&g_nodes_mx);
    for (int i=0;i<g_nodes_count;i++){
        JSON_Value *nv=json_value_init_object(); JSON_Object *no=json_object(nv);
        json_object_set_string(no,"ip", g_nodes[i].ip);
        json_object_set_number(no,"port", g_nodes[i].port);
        if (g_nodes[i].role[0])    json_object_set_string(no,"role", g_nodes[i].role);
        if (g_nodes[i].device[0])  json_object_set_string(no,"device", g_nodes[i].device);
        if (g_nodes[i].version[0]) json_object_set_string(no,"version", g_nodes[i].version);
        json_object_set_number(no,"last_seen", g_nodes[i].last_seen);
        json_array_append_value(arr, nv);
    }
    pthread_mutex_unlock(&g_nodes_mx);

    json_object_set_value(o,"nodes", arrv);
    json_object_set_number(o,"scan_feature_enabled", ((app->cfg.enable_scan)?1:0));

    int scanning = g_scan_in_progress ? 1 : 0;
    json_object_set_number(o,"scanning", scanning);
    json_object_set_number(o,"targets", g_scan_total);
    json_object_set_number(o,"done", g_scan_done);
    double prog = (g_scan_total>0) ? ((double)g_scan_done/(double)g_scan_total) : 0.0;
    if (prog > 1.0) prog = 1.0;
    json_object_set_number(o, "progress_pct", progress_pct());
    json_object_set_number(o,"last_started", g_last_started);
    json_object_set_number(o,"last_finished", g_last_finished);

    send_json(c, v, 200, 1);
    json_value_free(v);
    return 1;
}


#ifdef USE_SDL2_GUI
static volatile int g_gui_should_quit = 0;

static int gui_thread_main(void *arg) {
    config_t *cfg = (config_t*)arg;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 0;

    /* Hints to reduce whole-screen flashing with some X11 WMs */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

#ifdef USE_SDL2_TTF
if (TTF_Init() != 0) { SDL_Quit(); return 0; }
const char *fallbacks[] = {
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
    NULL
};
for (int i=0; fallbacks[i]; i++) {
    g_font = TTF_OpenFont(fallbacks[i], 14);
    if (g_font) break;
}
if (!g_font) { TTF_Quit(); SDL_Quit(); return 0; }
#endif

    SDL_Window *W = SDL_CreateWindow("autod - nodes",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        900, 600, SDL_WINDOW_SHOWN);
    if (!W) {
#ifdef USE_SDL2_TTF
        TTF_Quit();
#endif
        SDL_Quit();
        return 0;
    }

    SDL_Renderer *R = SDL_CreateRenderer(W, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!R) {
        SDL_DestroyWindow(W);
#ifdef USE_SDL2_TTF
        TTF_Quit();
#endif
        SDL_Quit();
        return 0;
    }
    SDL_RenderSetLogicalSize(R, 900, 600);

    /* --- layout derived from font height --- */
    int fh = 8; /* default for bitmap path */
#ifdef USE_SDL2_TTF
    fh = TTF_FontHeight(g_font);
    if (fh < 8) fh = 8;
#endif
    const int pad      = 8;
    const int header_h = 28;
    const int status_y = header_h + pad;        /* “Scan:” line */
    const int header_y = status_y + fh + pad;   /* column labels */
    const int sep_y    = header_y + fh + 2;     /* separator line (below labels) */
    const int row_y0   = sep_y + 12;            /* first row */
    const int row_h    = fh + 8;                /* row spacing */

    int sel = 0;                    /* selected row */
    Uint32 last_snap = 0;
    int trigger_enter = 0;          /* set by event, consumed after snapshot */

    while (!g_gui_should_quit && !g_stop) {
        /* 1) events */
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { g_gui_should_quit = 1; break; }
            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_q:
                    case SDLK_ESCAPE:
                        g_gui_should_quit = 1;
                        break;
                    case SDLK_UP:
                        if (sel > 0) sel--;
                        break;
                    case SDLK_DOWN:
                        sel++;
                        break;
                    case SDLK_SPACE:
                        if (cfg->enable_scan) start_scan_async(cfg);
                        break;
                    case SDLK_RETURN:
                        trigger_enter = 1; /* handle after we have a fresh snapshot */
                        break;
                }
            }
        }

        /* 2) snapshot current data (cheap, thread-safe) */
        gui_snapshot_t s = {0};
        Uint32 now = SDL_GetTicks();
        if (now - last_snap > 200) { take_gui_snapshot(&s); last_snap = now; }
        else { take_gui_snapshot(&s); }

        if (sel >= s.count) sel = s.count ? (s.count - 1) : 0;
        if (sel < 0) sel = 0;

        /* act on Enter with the fresh snapshot */
        if (trigger_enter) {
            trigger_enter = 0;
            if (sel < s.count) {
                fprintf(stderr, "ENTER on %s (role=%s)\n",
                        s.nodes[sel].ip,
                        s.nodes[sel].role[0] ? s.nodes[sel].role : "-");
                /* TODO: open details pane or fire /exec */
            }
        }

        /* 3) draw */
        SDL_SetRenderDrawColor(R, 18,22,26,255);
        SDL_RenderClear(R);

        /* header bar */
        SDL_Rect hdr = {0,0,900,header_h};
        SDL_SetRenderDrawColor(R, 32,38,44,255);
        SDL_RenderFillRect(R, &hdr);
        SDL_SetRenderDrawColor(R, 255,255,255,255);
        draw_text(R, pad, 10, "autod — Nodes (Up/Down=Select, Space=Rescan, Q/Esc=Quit)");

        /* status line */
        char st[256];
        const char *role   = (cfg->role[0]   ? cfg->role   : "-");
        const char *device = (cfg->device[0] ? cfg->device : "-");
        snprintf(st, sizeof(st),
                 "Scan: %s  %u/%u  %d%%  |  Port:%d  Role:%.24s  Device:%.24s",
                 s.scanning ? "RUNNING" : "idle",
                 s.done, s.targets, s.progress_pct,
                 cfg->port, role, device);
        draw_text(R, pad, status_y, st);

        /* table headers */
        draw_text(R, pad,   header_y, "IP");
        draw_text(R, 180,   header_y, "Role");
        draw_text(R, 360,   header_y, "Device");
        draw_text(R, 600,   header_y, "Version");

        /* separator line (NOW below the labels) */
        SDL_SetRenderDrawColor(R, 80,80,80,255);
        SDL_RenderDrawLine(R, pad, sep_y, 892, sep_y);

        /* rows */
        int y = row_y0;
        for (int i=0; i<s.count && i<64; i++, y += row_h) {
            if (i == sel) {
                SDL_Rect hi = {pad-2, y-2, 888, fh+6};
                SDL_SetRenderDrawColor(R, 24,28,34,255);
                SDL_RenderFillRect(R, &hi);
            }
            SDL_SetRenderDrawColor(R, 255,255,255,255);
            draw_text(R, pad,  y, s.nodes[i].ip);
            draw_text(R, 180,  y, s.nodes[i].role[0]?s.nodes[i].role:"-");
            draw_text(R, 360,  y, s.nodes[i].device[0]?s.nodes[i].device:"-");
            draw_text(R, 600,  y, s.nodes[i].version[0]?s.nodes[i].version:"-");
        }

        /* progress bar */
        SDL_Rect bar = {pad, 560, 884, 12};
        SDL_SetRenderDrawColor(R, 32,38,44,255);
        SDL_RenderFillRect(R, &bar);
        SDL_SetRenderDrawColor(R, 54,194,117,255);
        int w = (s.progress_pct * bar.w) / 100;
        SDL_Rect fill = {bar.x, bar.y, w, bar.h};
        SDL_RenderFillRect(R, &fill);

        SDL_RenderPresent(R);
        /* no SDL_Delay — vsync paces us */
    }

    SDL_DestroyRenderer(R);
    SDL_DestroyWindow(W);

#ifdef USE_SDL2_TTF
if (g_font) { TTF_CloseFont(g_font); g_font = NULL; }
TTF_Quit();
#endif
SDL_Quit();
    return 0;
}
#endif



/* ----------------------- main ----------------------- */

int main(int argc, char **argv){
int want_gui = 0;
const char *cfgpath = "./autod.conf";
for (int i=1; i<argc; i++) {
    if (!strcmp(argv[i], "--gui")) { want_gui = 1; continue; }
    /* first non-flag becomes cfg path */
    if (argv[i][0] != '-') { cfgpath = argv[i]; }
}
app_t app; cfg_defaults(&app.cfg);
if (parse_ini(cfgpath, &app.cfg) < 0)
    fprintf(stderr, "WARN: could not read %s, using defaults\n", cfgpath);

    signal(SIGINT,on_signal); signal(SIGTERM,on_signal);

    /* CivetWeb options */
    char portbuf[16]; snprintf(portbuf, sizeof(portbuf), "%d", app.cfg.port);
    char lp[96];
    if (strcmp(app.cfg.bind_addr,"0.0.0.0")==0) snprintf(lp, sizeof(lp), "%s", portbuf);
    else snprintf(lp, sizeof(lp), "%s:%s", app.cfg.bind_addr, portbuf);

    const char *options[] = {
        "listening_ports", lp,
        "enable_keep_alive", "yes",
        "num_threads", "4",
        NULL
    };

    struct mg_callbacks cbs; memset(&cbs, 0, sizeof(cbs));
    app.ctx = mg_start(&cbs, &app, options);
    if(!app.ctx){ fprintf(stderr,"ERROR: mg_start failed\n"); return 1; }

    /* Install handlers */
    mg_set_request_handler(app.ctx, "/health",  h_health,  &app);
    mg_set_request_handler(app.ctx, "/caps",    h_caps,    &app);
    mg_set_request_handler(app.ctx, "/exec",    h_exec,    &app);
    mg_set_request_handler(app.ctx, "/control", h_control, &app);
    mg_set_request_handler(app.ctx, "/nodes",   h_nodes,   &app);
    mg_set_request_handler(app.ctx, "/",        h_root,    &app);

    /* CORS preflight for any path */
    mg_set_request_handler(app.ctx, "**", h_options_all, &app);

    fprintf(stderr,"autod listening on %s:%d (scan %s)\n",
            app.cfg.bind_addr, app.cfg.port, app.cfg.enable_scan?"ENABLED":"disabled");


#ifdef USE_SDL2_GUI
if (want_gui) {
    SDL_Thread *th = SDL_CreateThread(gui_thread_main, "autod_gui", &app.cfg);
    (void)th;
}
#endif


    /* Seed cache (self) always; kickoff scan only if enabled */
    add_self_nodes(&app.cfg);
    if (app.cfg.enable_scan) start_scan_async(&app.cfg);

    while(!g_stop) sleep(1);
    mg_stop(app.ctx);
    return 0;
}
