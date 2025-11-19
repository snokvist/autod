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
#include "autod.h"

#if !defined(_WIN32)
extern char *realpath(const char *path, char *resolved_path);
#endif

volatile sig_atomic_t g_stop=0;
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

    sync_cfg_defaults(c);
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

        if (sync_cfg_parse(cfg, sect, k, v)) {
            continue;
        } else if (strcmp(sect,"server")==0) {
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
        }
    }
    fclose(f);
    return 0;
}

void fill_scan_config(const config_t *cfg, scan_config_t *scfg) {
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

long long now_ms(void) {
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

int run_exec(const config_t *cfg, const char *path, JSON_Array *args,
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

static int h_options_all(struct mg_connection *c, void *ud) {
    (void)ud;
    add_cors_options(c);
    return 1;
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

int read_body(struct mg_connection *c, upload_t *u) {
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

void send_json(struct mg_connection *c, JSON_Value *v, int code, int cors_public) {
    char *s = json_serialize_to_string(v);
    size_t n = s ? strlen(s) : 0;
    add_common_headers(c, code, "application/json; charset=utf-8", n, cors_public);
    if (n) mg_write(c, s, (int)n);
    if (s) json_free_serialized_string(s);
}

void send_plain(struct mg_connection *c, int code, const char *msg, int cors_public) {
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
void app_rebuild_config_locked(app_t *app) {
    if (!app) return;
    config_t merged = app->base_cfg;
    sync_ensure_id(&merged);
    app->cfg = merged;
}

void app_config_snapshot(app_t *app, config_t *out) {
    if (!app || !out) return;
    pthread_mutex_lock(&app->cfg_lock);
    *out = app->cfg;
    pthread_mutex_unlock(&app->cfg_lock);
}

static void run_startup_exec_sequence(app_t *app) {
    if (!app) return;

    config_t cfg;
    app_config_snapshot(app, &cfg);
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
        if (!caps_arr) {
            caps_val = json_value_init_array();
            caps_arr = json_array(caps_val);
        }
        sync_append_capabilities(&cfg, caps_arr);
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

    JSON_Value *sync_v = sync_build_status_json(&cfg, &app->slave);
    if (sync_v) {
        JSON_Object *so = json_object(sync_v);
        json_object_set_number(so, "active_override_generation",
                               app->active_override_generation);
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

static int resolve_http_target(app_t *app, const config_t *cfg,
                               const char *sync_id, int slot_index,
                               const char *node_ip, int port_hint,
                               char *host_out, size_t host_sz, int *port_out,
                               char *resolved_sync_id, size_t resolved_sz,
                               char *err_code, size_t err_sz) {
    if (!app || !cfg || !host_out || !port_out || !err_code) return -1;
    host_out[0] = '\0';
    *port_out = 0;
    if (resolved_sync_id && resolved_sz > 0) resolved_sync_id[0] = '\0';
    err_code[0] = '\0';

    char target_sync_id[64];
    target_sync_id[0] = '\0';

    if (slot_index >= 0) {
        if (slot_index >= SYNC_MAX_SLOTS) {
            snprintf(err_code, err_sz, "%s", "invalid_slot");
            return -1;
        }
        pthread_mutex_lock(&app->master.lock);
        if (app->master.slot_assignees[slot_index][0]) {
            strncpy(target_sync_id, app->master.slot_assignees[slot_index],
                    sizeof(target_sync_id) - 1);
            target_sync_id[sizeof(target_sync_id) - 1] = '\0';
        }
        pthread_mutex_unlock(&app->master.lock);
        if (!target_sync_id[0]) {
            snprintf(err_code, err_sz, "%s", "slot_unassigned");
            return -1;
        }
    } else if (sync_id && *sync_id) {
        strncpy(target_sync_id, sync_id, sizeof(target_sync_id) - 1);
        target_sync_id[sizeof(target_sync_id) - 1] = '\0';
    }

    scan_node_t nodes[SCAN_MAX_NODES];
    int node_count = scan_get_nodes(nodes, SCAN_MAX_NODES);

    if (node_ip && *node_ip) {
        for (int i = 0; i < node_count; i++) {
            if (strcmp(nodes[i].ip, node_ip) != 0) continue;
            if (port_hint > 0 && nodes[i].port != port_hint) {
                snprintf(err_code, err_sz, "%s", "port_mismatch");
                return -1;
            }
            strncpy(host_out, nodes[i].ip, host_sz - 1);
            host_out[host_sz - 1] = '\0';
            *port_out = nodes[i].port;
            if (resolved_sync_id && nodes[i].sync_id[0]) {
                strncpy(resolved_sync_id, nodes[i].sync_id, resolved_sz - 1);
                resolved_sync_id[resolved_sz - 1] = '\0';
            }
            return 0;
        }
        snprintf(err_code, err_sz, "%s", "node_not_found");
        return -1;
    }

    if (target_sync_id[0]) {
        for (int i = 0; i < node_count; i++) {
            if (!nodes[i].sync_id[0]) continue;
            if (strcasecmp(nodes[i].sync_id, target_sync_id) != 0) continue;
            strncpy(host_out, nodes[i].ip, host_sz - 1);
            host_out[host_sz - 1] = '\0';
            *port_out = nodes[i].port;
            if (resolved_sync_id) {
                strncpy(resolved_sync_id, nodes[i].sync_id, resolved_sz - 1);
                resolved_sync_id[resolved_sz - 1] = '\0';
            }
            return 0;
        }
        snprintf(err_code, err_sz, "%s", "id_not_found");
        return -1;
    }

    snprintf(err_code, err_sz, "%s", "invalid_target");
    return -1;
}

static int h_http(struct mg_connection *c, void *ud) {
    app_t *app = (app_t *)ud;
    config_t cfg; app_config_snapshot(app, &cfg);
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
    const JSON_Value *sync_id_v = json_object_get_value(obj, "sync_id");
    const JSON_Value *slot_v = json_object_get_value(obj, "slot");
    const JSON_Value *node_ip_v = json_object_get_value(obj, "node_ip");
    const JSON_Value *port_v = json_object_get_value(obj, "port");
    const JSON_Value *path_v = json_object_get_value(obj, "path");
    const JSON_Value *method_v = json_object_get_value(obj, "method");
    const JSON_Value *headers_v = json_object_get_value(obj, "headers");
    const JSON_Value *body_v = json_object_get_value(obj, "body");
    const JSON_Value *body_b64_v = json_object_get_value(obj, "body_base64");
    const JSON_Value *tls_v = json_object_get_value(obj, "tls");
    const JSON_Value *timeout_v = json_object_get_value(obj, "timeout_ms");

    const char *sync_id = (sync_id_v && json_value_get_type(sync_id_v) == JSONString)
                          ? json_value_get_string(sync_id_v)
                          : NULL;
    const char *node_ip = (node_ip_v && json_value_get_type(node_ip_v) == JSONString)
                          ? json_value_get_string(node_ip_v)
                          : NULL;
    double slot_d = (slot_v && json_value_get_type(slot_v) == JSONNumber)
                    ? json_value_get_number(slot_v)
                    : -1.0;
    int slot_index = -1;
    if (slot_d >= 0.0) {
        slot_index = (int)slot_d - 1;
        if ((double)(slot_index + 1) != slot_d) slot_index = -2; // invalid sentinel
    }
    double port_d = (port_v && json_value_get_type(port_v) == JSONNumber)
                    ? json_value_get_number(port_v)
                    : -1.0;
    int port_hint = (int)port_d;
    if ((double)port_hint != port_d) port_hint = -1;
    const char *path = (path_v && json_value_get_type(path_v) == JSONString)
                       ? json_value_get_string(path_v)
                       : "/";
    const char *method = (method_v && json_value_get_type(method_v) == JSONString)
                         ? json_value_get_string(method_v)
                         : "GET";
    int use_tls = (tls_v && json_value_get_type(tls_v) == JSONBoolean)
                  ? json_value_get_boolean(tls_v)
                  : 0;
    double timeout_d = (timeout_v && json_value_get_type(timeout_v) == JSONNumber)
                       ? json_value_get_number(timeout_v)
                       : 5000.0;
    int timeout_ms = (int)timeout_d;

    int has_body = (body_v && json_value_get_type(body_v) == JSONString) ? 1 : 0;
    int has_body_b64 = (body_b64_v && json_value_get_type(body_b64_v) == JSONString) ? 1 : 0;
    int headers_obj = (headers_v && json_value_get_type(headers_v) == JSONObject) ? 1 : 0;

    int target_count = 0;
    if (sync_id && *sync_id) target_count++;
    if (node_ip && *node_ip) target_count++;
    if (slot_index >= 0) target_count++;

    if (slot_index == -2 || target_count != 1 ||
        !path || !*path || !method || !*method ||
        (has_body && has_body_b64)) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "invalid_request");
        send_json(c, v, 400, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

#ifdef NO_SSL
    if (use_tls) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "ssl_disabled");
        send_json(c, v, 400, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }
#else
    if (use_tls) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "tls_not_supported");
        send_json(c, v, 400, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }
#endif

    char target_host[64];
    int target_port = 0;
    char resolved_sync_id[64];
    char target_err[32];

    if (resolve_http_target(app, &cfg, sync_id, slot_index, node_ip, port_hint,
                             target_host, sizeof(target_host), &target_port,
                             resolved_sync_id, sizeof(resolved_sync_id),
                             target_err, sizeof(target_err)) != 0) {
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", target_err[0] ? target_err : "resolve_failed");
        send_json(c, v, 400, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    unsigned char *body_buf = NULL;
    const unsigned char *body_data = NULL;
    size_t body_len = 0;

    if (has_body_b64) {
        const char *body_b64 = json_value_get_string(body_b64_v);
        size_t src_len = body_b64 ? strlen(body_b64) : 0;
        size_t dst_cap = ((src_len / 4) + 1) * 3;
        if (dst_cap == 0) dst_cap = 1;
        body_buf = (unsigned char *)malloc(dst_cap);
        if (!body_buf) {
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
            if (mg_base64_decode(body_b64, src_len, body_buf, &out_len) != -1) {
                free(body_buf);
                JSON_Value *v = json_value_init_object();
                JSON_Object *o = json_object(v);
                json_object_set_string(o, "error", "invalid_base64");
                send_json(c, v, 400, 1);
                json_value_free(v);
                json_value_free(root);
                return 1;
            }
        }
        body_data = body_buf;
        body_len = out_len;
    } else if (has_body) {
        const char *body = json_value_get_string(body_v);
        body_data = (const unsigned char *)(body ? body : "");
        body_len = strlen((const char *)body_data);
    }

    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", target_port);
    struct addrinfo hints; memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICSERV;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(target_host, portbuf, &hints, &res);
    if (gai != 0) {
        if (body_buf) free(body_buf);
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

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv;
        if (timeout_ms < 1) timeout_ms = 1;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        int saved_errno = errno;
        close(fd);
        fd = -1;
        errno = saved_errno;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        int saved_errno = errno;
        if (body_buf) free(body_buf);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "connect_failed");
        if (saved_errno) json_object_set_string(o, "detail", strerror(saved_errno));
        send_json(c, v, 502, 1);
        json_value_free(v);
        json_value_free(root);
        errno = saved_errno;
        return 1;
    }

    char method_buf[16];
    snprintf(method_buf, sizeof(method_buf), "%s", method);
    for (size_t i = 0; i < strlen(method_buf); i++) {
        method_buf[i] = (char)toupper((unsigned char)method_buf[i]);
    }

    int has_content_length = 0;
    if (headers_obj) {
        size_t hc = json_object_get_count(json_object(headers_v));
        for (size_t i = 0; i < hc; i++) {
            const char *hn = json_object_get_name(json_object(headers_v), i);
            const char *hv = json_object_get_string(json_object(headers_v), hn);
            if (!hn || !hv) continue;
            if (strcasecmp(hn, "Content-Length") == 0) has_content_length = 1;
        }
    }

    dprintf(fd, "%s %s HTTP/1.0\r\nHost: %s\r\n", method_buf, path, target_host);
    if (headers_obj) {
        size_t hc = json_object_get_count(json_object(headers_v));
        for (size_t i = 0; i < hc; i++) {
            const char *hn = json_object_get_name(json_object(headers_v), i);
            const char *hv = json_object_get_string(json_object(headers_v), hn);
            if (!hn || !hv) continue;
            dprintf(fd, "%s: %s\r\n", hn, hv);
        }
    }
    if (body_len > 0 && !has_content_length) {
        dprintf(fd, "Content-Length: %zu\r\n", body_len);
    }
    dprintf(fd, "Connection: close\r\n\r\n");
    if (body_len > 0) {
        (void)send(fd, body_data, body_len, 0);
    }

    size_t bufcap = 4096;
    size_t buflen = 0;
    char *resp_buf = (char *)malloc(bufcap + 1);
    if (!resp_buf) {
        if (body_buf) free(body_buf);
        close(fd);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "oom");
        send_json(c, v, 500, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    int recv_err = 0;
    while (1) {
        char chunk[2048];
        ssize_t r = recv(fd, chunk, sizeof(chunk), 0);
        if (r == 0) break;
        if (r < 0) { recv_err = errno ? errno : EIO; break; }
        if (buflen + (size_t)r + 1 > bufcap) {
            size_t ncap = bufcap * 2;
            while (buflen + (size_t)r + 1 > ncap) ncap *= 2;
            char *nb = (char *)realloc(resp_buf, ncap + 1);
            if (!nb) { recv_err = ENOMEM; break; }
            resp_buf = nb;
            bufcap = ncap;
        }
        memcpy(resp_buf + buflen, chunk, (size_t)r);
        buflen += (size_t)r;
    }
    close(fd);

    if (recv_err) {
        if (body_buf) free(body_buf);
        free(resp_buf);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "recv_failed");
        json_object_set_string(o, "detail", strerror(recv_err));
        send_json(c, v, 502, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }
    resp_buf[buflen] = '\0';

    size_t header_len = 0;
    size_t body_off = 0;
    const char *hdr_end = NULL;
    hdr_end = strstr(resp_buf, "\r\n\r\n");
    if (!hdr_end) hdr_end = strstr(resp_buf, "\n\n");
    if (hdr_end) {
        header_len = (size_t)(hdr_end - resp_buf);
        body_off = header_len + ((hdr_end[0] == '\r') ? 4 : 2);
    } else {
        header_len = buflen;
        body_off = buflen;
    }

    char *header_copy = (char *)malloc(header_len + 1);
    if (!header_copy) {
        if (body_buf) free(body_buf);
        free(resp_buf);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "oom");
        send_json(c, v, 500, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }
    memcpy(header_copy, resp_buf, header_len);
    header_copy[header_len] = '\0';

    int status_code = 0;
    char reason[128]; reason[0] = '\0';

    JSON_Value *headers_out_v = json_value_init_object();
    JSON_Object *headers_out = json_object(headers_out_v);

    char *saveptr = NULL;
    char *line = strtok_r(header_copy, "\r\n", &saveptr);
    if (line) {
        if (sscanf(line, "HTTP/%*s %d %127[\x20-\x7E]", &status_code, reason) < 1) {
            status_code = 0;
        }
        while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            char *val = colon + 1;
            trim(line);
            trim(val);
            if (*line && val) {
                json_object_set_string(headers_out, line, val);
            }
        }
    }

    const unsigned char *body_ptr = (const unsigned char *)(resp_buf + (body_off > buflen ? buflen : body_off));
    size_t resp_body_len = (body_off <= buflen) ? (buflen - body_off) : 0;

    size_t b64_cap = ((resp_body_len + 2) / 3) * 4 + 1;
    char *b64 = (char *)malloc(b64_cap);
    if (!b64) {
        if (body_buf) free(body_buf);
        free(resp_buf);
        free(header_copy);
        json_value_free(headers_out_v);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "oom");
        send_json(c, v, 500, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }
    size_t b64_len = b64_cap;
    if (resp_body_len == 0) {
        b64_len = 0;
        b64[0] = '\0';
    } else if (mg_base64_encode(body_ptr, resp_body_len, b64, &b64_len) != -1) {
        free(b64);
        if (body_buf) free(body_buf);
        free(resp_buf);
        free(header_copy);
        json_value_free(headers_out_v);
        JSON_Value *v = json_value_init_object();
        JSON_Object *o = json_object(v);
        json_object_set_string(o, "error", "encode_failed");
        send_json(c, v, 500, 1);
        json_value_free(v);
        json_value_free(root);
        return 1;
    }

    JSON_Value *resp = json_value_init_object();
    JSON_Object *or = json_object(resp);
    json_object_set_string(or, "status", "ok");
    json_object_set_number(or, "status_code", (double)status_code);
    json_object_set_string(or, "reason", reason);
    json_object_set_number(or, "body_length", (double)resp_body_len);
    json_object_set_string(or, "body_base64", b64);
    json_object_set_value(or, "headers", headers_out_v);
    json_object_set_string(or, "target_ip", target_host);
    json_object_set_number(or, "target_port", (double)target_port);
    if (resolved_sync_id[0]) {
        json_object_set_string(or, "sync_id", resolved_sync_id);
    }

    send_json(c, resp, 200, 1);

    free(b64);
    if (body_buf) free(body_buf);
    free(resp_buf);
    free(header_copy);
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
    sync_master_state_init(&app.master);
    sync_slave_state_init(&app.slave);
    app.active_overrides = NULL;
    app.active_override_generation = 0;

    cfg_defaults(&app.base_cfg);
    if (parse_ini(cfgpath, &app.base_cfg) < 0) {
        fprintf(stderr, "WARN: could not read %s, using defaults\n", cfgpath);
    }

    pthread_mutex_lock(&app.cfg_lock);
    app.cfg = app.base_cfg;
    sync_ensure_id(&app.cfg);
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
    mg_set_request_handler(app.ctx, "/http",    h_http,          &app);
    mg_set_request_handler(app.ctx, "/nodes",   h_nodes,         &app);
    mg_set_request_handler(app.ctx, "/media",   h_media,         &app);
    sync_register_http_handlers(app.ctx, &app);
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
        (void)sync_slave_start_thread(&app);
    }

    run_startup_exec_sequence(&app);

    while(!g_stop) sleep(1);
    sync_slave_stop_thread(&app.slave);
    mg_stop(app.ctx);
    return 0;
}
