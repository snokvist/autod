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

    /* scan */
    scan_extra_subnet_t extra_subnets[SCAN_MAX_EXTRA_SUBNETS];
    unsigned            extra_subnet_count;

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
    const config_t *cfg = &app->cfg;
    if (!cfg_has_cap(cfg, "dvr")) {
        send_plain(c, 404, "not_found", cfg->ui_public);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(c);
    if (!ri || !ri->request_method) return 0;

    int is_head = (strcmp(ri->request_method, "HEAD") == 0);
    if (!is_head && strcmp(ri->request_method, "GET") != 0) {
        send_plain(c, 405, "method_not_allowed", cfg->ui_public);
        return 1;
    }

    const char *uri = ri->local_uri ? ri->local_uri : ri->request_uri;
    if (!uri) return 0;

    const char *prefix = "/media/";
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) {
        if (!strcmp(uri, "/media") || !strcmp(uri, "/media/")) {
            send_plain(c, 404, "not_found", cfg->ui_public);
            return 1;
        }
        return 0;
    }

    const char *rel = uri + prefix_len;
    while (*rel == '/') rel++;
    if (!*rel) {
        send_plain(c, 404, "not_found", cfg->ui_public);
        return 1;
    }

    char decoded[PATH_MAX];
    int dec_len = mg_url_decode(rel, (int)strlen(rel), decoded, (int)sizeof(decoded), 0);
    if (dec_len <= 0 || dec_len >= (int)sizeof(decoded)) {
        send_plain(c, 400, "bad_request", cfg->ui_public);
        return 1;
    }
    decoded[dec_len] = '\0';

    const char *base = getenv("DVR_MEDIA_DIR");
    if (!base || !*base) base = "/media";

    char base_real[PATH_MAX];
    if (!realpath(base, base_real)) {
        send_plain(c, 404, "media_unavailable", cfg->ui_public);
        return 1;
    }

    char joined[PATH_MAX];
    if (snprintf(joined, sizeof(joined), "%s/%s", base_real, decoded) >= (int)sizeof(joined)) {
        send_plain(c, 400, "path_too_long", cfg->ui_public);
        return 1;
    }

    char resolved[PATH_MAX];
    if (!realpath(joined, resolved)) {
        send_plain(c, 404, "not_found", cfg->ui_public);
        return 1;
    }

    size_t base_len = strlen(base_real);
    if (strncmp(resolved, base_real, base_len) != 0 ||
        (resolved[base_len] != '\0' && resolved[base_len] != '/')) {
        send_plain(c, 403, "forbidden", cfg->ui_public);
        return 1;
    }

    int fd = open(resolved, O_RDONLY);
    if (fd < 0) {
        send_plain(c, 404, "not_found", cfg->ui_public);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        send_plain(c, 404, "not_found", cfg->ui_public);
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

    add_common_headers_extra(c, 200, ctype, (size_t)st.st_size, cfg->ui_public,
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
    if(!app->cfg.serve_ui || !app->cfg.ui_path[0]){
        JSON_Value *v=json_value_init_object(); JSON_Object *o=json_object(v);
        json_object_set_string(o,"error","no_ui");
        send_json(c, v, 404, app->cfg.ui_public);
        json_value_free(v);
        return 1;
    }

    const struct mg_request_info *ri = mg_get_request_info(c);
    const char *method = (ri && ri->request_method) ? ri->request_method : "";
    int is_head = (strcmp(method, "HEAD") == 0);
    if (!is_head && strcmp(method, "GET") != 0) {
        send_plain(c, 405, "method_not_allowed", app->cfg.ui_public);
        return 1;
    }

    const char *req_uri = (ri && ri->local_uri) ? ri->local_uri :
                          (ri && ri->request_uri) ? ri->request_uri : "/";
    if (!req_uri) req_uri = "/";

    char decoded_uri[PATH_MAX];
    int dec = mg_url_decode(req_uri, (int)strlen(req_uri),
                            decoded_uri, (int)sizeof(decoded_uri), 0);
    if (dec <= 0 || dec >= (int)sizeof(decoded_uri)) {
        send_plain(c, 400, "bad_request", app->cfg.ui_public);
        return 1;
    }
    decoded_uri[dec] = '\0';
    const char *uri = decoded_uri;

    const char *basename = app->cfg.ui_path;
    const char *slash = strrchr(basename, '/');
    if (slash && slash[1]) basename = slash + 1;

    if (!strcmp(uri, "/") ||
        (basename && *basename && uri[0]=='/' && strcmp(uri + 1, basename) == 0)) {
        return stream_file(c, app->cfg.ui_path, app->cfg.ui_public, 1);
    }

    const char *rel = uri;
    while (*rel == '/') rel++;
    if (!*rel) {
        return stream_file(c, app->cfg.ui_path, app->cfg.ui_public, 1);
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
            send_plain(c, 403, "forbidden", app->cfg.ui_public);
            return 1;
        }
    }

    char base_dir[PATH_MAX];
    strncpy(base_dir, app->cfg.ui_path, sizeof(base_dir) - 1);
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
        send_plain(c, 404, "not_found", app->cfg.ui_public);
        return 1;
    }

    char joined[PATH_MAX];
    if (snprintf(joined, sizeof(joined), "%s/%s", base_real, rel_copy) >= (int)sizeof(joined)) {
        send_plain(c, 400, "path_too_long", app->cfg.ui_public);
        return 1;
    }

    char resolved[PATH_MAX];
    if (realpath(joined, resolved)) {
        size_t base_len = strlen(base_real);
        if (strncmp(resolved, base_real, base_len) != 0 ||
            (resolved[base_len] != '\0' && resolved[base_len] != '/')) {
            send_plain(c, 403, "forbidden", app->cfg.ui_public);
            return 1;
        }
        return stream_file(c, resolved, app->cfg.ui_public, 0);
    }

    return stream_file(c, joined, app->cfg.ui_public, 0);
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

        scan_config_t scfg; fill_scan_config(&app->cfg, &scfg);
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
    mg_set_request_handler(app.ctx, "/health",  h_health,  &app);
    mg_set_request_handler(app.ctx, "/caps",    h_caps,    &app);
    mg_set_request_handler(app.ctx, "/exec",    h_exec,    &app);
    mg_set_request_handler(app.ctx, "/udp",     h_udp,     &app);
    mg_set_request_handler(app.ctx, "/nodes",   h_nodes,   &app);
    mg_set_request_handler(app.ctx, "/media",   h_media,   &app);
    mg_set_request_handler(app.ctx, "/",        h_root,    &app);

    /* CORS preflight */
    mg_set_request_handler(app.ctx, "**", h_options_all, &app);

    fprintf(stderr,"autod listening on %s:%d (scan %s)\n",
            app.cfg.bind_addr, app.cfg.port, app.cfg.enable_scan?"ENABLED":"disabled");

    // ---- Scanner: seed + optional autostart
    scan_init();
    scan_config_t scfg; fill_scan_config(&app.cfg, &scfg);
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
