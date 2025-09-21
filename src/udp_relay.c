/*
 * UDP Relay Manager — single epoll loop + tiny HTTP /api/v1 + /ui
 * -----------------------------------------------------------------------
 * This build:
 *  - Keeps /api/v1/status, /api/v1/config (GET/POST), and /api/v1/action/<verb> endpoints
 *    (set, append, append_range, clear, reset, clear_to).
 *  - Adds /api/v1/reload (GET/POST). POST body may include {"sync":true} to reload immediately.
 *  - Status JSON includes:
 *      * pkts_out_total (sum of per-dest pkts)
 *      * rx_drops (kernel-reported drops via SO_RXQ_OVFL)
 *      * trunc_drops (datagrams larger than bufsz; dropped, not forwarded)
 *  - Safe counter roll-over (halve when thresholds exceeded).
 *  - Fixed HTTP state handling (hc_find so UDP fds aren't misclassified).
 *  - /ui is served from an external file supplied via --ui <file>.
 *      * Auto-detects gzip by file extension (.gz/.gzip) or magic (0x1f,0x8b)
 *      * For gzip UI: adds Content-Encoding: gzip
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=gnu11 -o udp_relay udp_relay.c
 *
 * Runtime:
 *   ./udp_relay [--ui /path/to/ui.html[.gz]]
 *
 * Config file: /etc/udp_relay.conf
 *
 * INI format (no sections; '#' or ';' are comments):
 *   http_bind=127.0.0.1
 *   control_port=9000
 *   src_ip=0.0.0.0
 *   rcvbuf=1048576
 *   sndbuf=1048576
 *   bufsz=9000
 *   tos=0
 *   bind=5700:5600
 *   bind=5701
 *   bind=5702
 *   bind=5703
 *
 * UI-only metadata (optional; read-only by UI):
 *   dest_green=127.0.0.1:5600
 *   dest_blue=192.168.2.20:14550
 *   group_yellow=display|127.0.0.1:5600,127.0.0.1:5601
 *
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>   /* for strcasecmp/strcasestr */
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ------------------- tunables & constants ------------------- */

#define MAX_RELAYS      64
#define MAX_DESTS       128
#define MAX_BINDS       64
#define MAX_LINE        1024
#define MAX_EVENTS      128
#define MAX_HTTP_CONN   64
#define HTTP_BUF_MAX    65536
#define STATUS_CAP      8192
#define CFG_PATH        "/etc/udp_relay.conf"
#define CFG_TMP_PATH    "/etc/udp_relay.conf.tmp"

/* Counter roll-over thresholds: when any hits these, all are halved */
#define PKTS_ROLLOVER_LIMIT  ((uint64_t)1000000000ULL)  /* 1e9 pkts */
#define BYTES_ROLLOVER_LIMIT ((uint64_t)1ULL<<40)       /* ~1 TiB  */

/* ------------------- small utils ---------------------------- */

static inline uint64_t now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline char* trim(char *s){
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) e--;
    e[1] = '\0';
    return s;
}

static inline int parse_int_bounded(const char *s, int lo, int hi){
    if (!s || !*s) return -1;
    char *end=NULL; long v=strtol(s,&end,10);
    if (end==s || *end!='\0') return -1;
    if (v<lo || v>hi) return -1;
    return (int)v;
}

static inline int set_nonblock(int fd){
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl<0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* ------------------- config model --------------------------- */

struct dest {
    struct sockaddr_in addr;
    uint64_t pkts_out;
};

struct relay {
    int src_port;
    int fd;
    struct dest dests[MAX_DESTS];
    int dest_cnt;
    uint64_t pkts_in, bytes_in, bytes_out, send_errs, last_rx_ns;
    uint64_t rx_drops;    /* kernel-reported drops (SO_RXQ_OVFL accumulation) */
    uint64_t trunc_drops; /* datagrams > bufsz (MSG_TRUNC) — dropped instead of forwarded */
};

struct config {
    char http_bind[64];  /* default 127.0.0.1 */
    int  control_port;   /* default 9000 */
    char src_ip[64];     /* default 0.0.0.0 */
    int  rcvbuf, sndbuf; /* 0 = skip */
    int  bufsz;          /* default 9000 */
    int  tos;            /* 0 = skip */
    int  bind_count;
    char bind_lines[MAX_BINDS][MAX_LINE];
};

static struct config G;                    /* current config */
static struct relay REL[MAX_RELAYS];       /* active relays */
static int REL_N = 0;

static volatile sig_atomic_t WANT_RELOAD = 0;
static volatile sig_atomic_t WANT_EXIT   = 0;

static int EPFD = -1;                      /* epoll fd */
static int HTTP_LFD = -1;                  /* http listen fd */

/* ---- Optional UI asset loaded from file (via --ui) ---- */
static char   *UI_BUF = NULL;
static size_t  UI_LEN = 0;
static int     UI_IS_GZIP = 0;

/* ---- UDP receive buffer (global so sync reload can resize) ---- */
static char   *UDP_BUF = NULL;

/* Forward decl */
static int reload_from_disk(void);

/* ------------------- INI load/save -------------------------- */

static void cfg_defaults(struct config *c){
    memset(c, 0, sizeof(*c));
    snprintf(c->http_bind, sizeof(c->http_bind), "127.0.0.1");
    c->control_port = 9000;
    snprintf(c->src_ip, sizeof(c->src_ip), "0.0.0.0");
    c->rcvbuf=0; c->sndbuf=0; c->bufsz=9000; c->tos=0;
    c->bind_count=0;
}

static int load_file(const char *path, char **out, size_t *outlen){
    FILE *fp=fopen(path,"rb"); if(!fp) return -1;
    if (fseek(fp,0,SEEK_END)!=0){ fclose(fp); return -1; }
    long sz=ftell(fp); if(sz<0){ fclose(fp); return -1; }
    if (fseek(fp,0,SEEK_SET)!=0){ fclose(fp); return -1; }
    char *buf=malloc((size_t)sz+1); if(!buf){ fclose(fp); return -1; }
    size_t rd=fread(buf,1,(size_t)sz,fp);
    fclose(fp);
    if (rd!=(size_t)sz){ free(buf); return -1; }
    buf[sz]=0;
    *out=buf; if(outlen)*outlen=(size_t)sz;
    return 0;
}

static int save_file_atomic(const char *path_tmp, const char *path, const char *data, size_t len){
    FILE *fp=fopen(path_tmp,"wb"); if(!fp) return -1;
    if (fwrite(data,1,len,fp)!=len){ fclose(fp); return -1; }
    if (fflush(fp)!=0){ fclose(fp); return -1; }
    if (fsync(fileno(fp))!=0){ fclose(fp); return -1; }
    if (fclose(fp)!=0) return -1;
    if (rename(path_tmp, path)!=0) return -1;
    return 0;
}

static int load_ini_text(const char *text, struct config *c){
    cfg_defaults(c);
    char *dup=strdup(text); if(!dup) return -1;
    char *saveptr=NULL;
    for(char *line=strtok_r(dup,"\n",&saveptr); line; line=strtok_r(NULL,"\n",&saveptr)){
        char *s=trim(line);
        if(!*s || *s=='#' || *s==';') continue;
        char *eq=strchr(s,'=');
        if(!eq) continue;
        *eq=0;
        char *key=trim(s), *val=trim(eq+1);
        if(!strcmp(key,"http_bind")){
            snprintf(c->http_bind,sizeof(c->http_bind),"%s",val);
        } else if(!strcmp(key,"control_port")){
            int v=parse_int_bounded(val,1,65535); if(v>0) c->control_port=v;
        } else if(!strcmp(key,"src_ip")){
            snprintf(c->src_ip,sizeof(c->src_ip),"%s",val);
        } else if(!strcmp(key,"rcvbuf")){
            int v=parse_int_bounded(val,1024,64*1024*1024); if(v>0) c->rcvbuf=v;
        } else if(!strcmp(key,"sndbuf")){
            int v=parse_int_bounded(val,1024,64*1024*1024); if(v>0) c->sndbuf=v;
        } else if(!strcmp(key,"bufsz")){
            int v=parse_int_bounded(val,512,64*1024); if(v>0) c->bufsz=v;
        } else if(!strcmp(key,"tos")){
            int v=parse_int_bounded(val,0,255); if(v>=0) c->tos=v;
        } else if(!strcmp(key,"bind")){
            if(c->bind_count<MAX_BINDS){
                snprintf(c->bind_lines[c->bind_count++],MAX_LINE,"%s",val);
            }
        }
        /* UI-only keys are ignored by backend (dest_* / group_yellow) */
    }
    free(dup);
    return 0;
}

static int load_ini_file(struct config *c){
    char *txt=NULL; size_t len=0;
    if (load_file(CFG_PATH,&txt,&len)!=0) {
        cfg_defaults(c);
        return 0;
    }
    int rc=load_ini_text(txt,c);
    free(txt);
    return rc;
}

/* ------------------- relay helpers -------------------------- */

static inline int sockaddr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b){
    return a->sin_family==b->sin_family &&
           a->sin_port==b->sin_port &&
           a->sin_addr.s_addr==b->sin_addr.s_addr;
}

static int add_dest(struct relay *r, const char *ip, int port){
    if (r->dest_cnt >= MAX_DESTS) return -1;
    struct dest *d=&r->dests[r->dest_cnt];
    memset(d,0,sizeof(*d));
    d->addr.sin_family=AF_INET;
    d->addr.sin_port=htons(port);
    if (inet_pton(AF_INET, ip, &d->addr.sin_addr)!=1) return -1;
    d->pkts_out=0;
    r->dest_cnt++;
    return 0;
}

static int parse_dest_token(struct relay *r, const char *tok){
    char buf[128]; snprintf(buf,sizeof(buf),"%s",tok);
    char *s=trim(buf);
    char *ip_part=NULL, *port_part=s;
    char *colon=strchr(s,':');
    if (colon){ *colon=0; ip_part=s; port_part=colon+1; }
    const char *ip = ip_part ? ip_part : "127.0.0.1";
    char *dash=strchr(port_part,'-');
    if (dash){
        *dash=0;
        int a=parse_int_bounded(port_part,1,65535);
        int b=parse_int_bounded(dash+1,1,65535);
        if (a<0 || b<0) return -1;
        if (a>b){ int t=a; a=b; b=t; }
        for (int p=a; p<=b; p++){
            if (add_dest(r,ip,p)<0) break;
        }
        return 0;
    } else {
        int p=parse_int_bounded(port_part,1,65535);
        if (p<0) return -1;
        return add_dest(r,ip,p);
    }
}

static int parse_dest_list(struct relay *r, const char *list, bool replace){
    struct relay tmp={0};
    if (list && *list){
        char *dup=strdup(list); if(!dup) return -1;
        char *save=NULL;
        for(char *tok=strtok_r(dup,",",&save); tok; tok=strtok_r(NULL,",",&save)){
            if (parse_dest_token(&tmp, trim(tok))<0){ free(dup); return -1; }
        }
        free(dup);
    }
    if (replace){
        r->dest_cnt=0; /* stats preserved */
    }
    for (int i=0;i<tmp.dest_cnt && r->dest_cnt<MAX_DESTS;i++){
        r->dests[r->dest_cnt++] = tmp.dests[i];
    }
    return 0;
}

static int make_udp_socket(const char *bind_ip, int port, int rcvbuf, int sndbuf, int tos){
    int s=socket(AF_INET,SOCK_DGRAM,0);
    if (s<0) { perror("socket"); return -1; }
    int one=1;
    setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(s,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one));
#endif
#ifdef SO_RXQ_OVFL
    setsockopt(s,SOL_SOCKET,SO_RXQ_OVFL,&one,sizeof(one)); /* enable kernel drop count cmsg */
#endif
    if (rcvbuf>0) setsockopt(s,SOL_SOCKET,SO_RCVBUF,&rcvbuf,sizeof(rcvbuf));
    if (sndbuf>0) setsockopt(s,SOL_SOCKET,SO_SNDBUF,&sndbuf,sizeof(sndbuf));
#ifdef IP_TOS
    if (tos>0) setsockopt(s,IPPROTO_IP,IP_TOS,&tos,sizeof(tos));
#endif
    struct sockaddr_in a={0};
    a.sin_family=AF_INET;
    a.sin_port=htons(port);
    if (inet_pton(AF_INET, bind_ip, &a.sin_addr)!=1){
        fprintf(stderr,"Bad src_ip: %s\n", bind_ip);
        close(s); return -1;
    }
    if (bind(s,(struct sockaddr*)&a,sizeof(a))<0){
        perror("bind"); close(s); return -1;
    }
    if (set_nonblock(s)<0){ perror("fcntl"); close(s); return -1; }
    return s;
}

static void close_relays(void){
    for (int i=0;i<REL_N;i++){
        if (REL[i].fd>=0){
            epoll_ctl(EPFD, EPOLL_CTL_DEL, REL[i].fd, NULL);
            close(REL[i].fd);
        }
    }
    memset(REL,0,sizeof(REL));
    REL_N=0;
}

static int apply_config_relays(const struct config *c){
    close_relays();
    for (int i=0;i<c->bind_count;i++){
        if (REL_N >= MAX_RELAYS){ fprintf(stderr,"Too many binds\n"); break; }
        char line[MAX_LINE]; snprintf(line,sizeof(line),"%s", c->bind_lines[i]);
        char *sep=strchr(line,':');
        int sport=-1; char *list=NULL;
        if (sep){ *sep=0; sport=parse_int_bounded(trim(line),1,65535); list=trim(sep+1); }
        else sport=parse_int_bounded(trim(line),1,65535);
        if (sport<0){ fprintf(stderr,"Bad bind line: %s\n", c->bind_lines[i]); continue; }

        struct relay *r=&REL[REL_N];
        memset(r,0,sizeof(*r));
        r->src_port=sport;
        r->fd=make_udp_socket(c->src_ip, sport, c->rcvbuf, c->sndbuf, c->tos);
        if (r->fd<0){ fprintf(stderr,"Bind failed %d\n", sport); continue; }

        struct epoll_event ev={.events=EPOLLIN, .data.fd=r->fd};
        if (epoll_ctl(EPFD, EPOLL_CTL_ADD, r->fd, &ev)<0){ perror("epoll_ctl add udp"); close(r->fd); continue; }

        if (list && *list){
            if (parse_dest_list(r, list, true)<0){
                fprintf(stderr,"Bad dest list on %d, starting empty\n", sport);
                r->dest_cnt=0;
            }
        }
        fprintf(stderr,"Bound %d (dests=%d) on %s (bufsz=%d rcv=%d snd=%d tos=%d)\n",
                sport, r->dest_cnt, c->src_ip, c->bufsz, c->rcvbuf, c->sndbuf, c->tos);
        REL_N++;
    }
    return (REL_N>0)?0:-1;
}

/* ------------------- HTTP tiny server (nonblocking) ---------- */

struct http_conn {
    int fd;
    char *buf;
    size_t cap, len;
    size_t need;
    int    have_hdr;
};
static struct http_conn HC[MAX_HTTP_CONN];

static struct http_conn* hc_get(int fd){
    for (int i=0;i<MAX_HTTP_CONN;i++) if (HC[i].fd==fd) return &HC[i];
    for (int i=0;i<MAX_HTTP_CONN;i++) if (HC[i].fd==0){
        HC[i].fd=fd; HC[i].cap=4096; HC[i].len=0; HC[i].need=0; HC[i].have_hdr=0;
        HC[i].buf=malloc(HC[i].cap);
        return &HC[i];
    }
    return NULL;
}
static struct http_conn* hc_find(int fd){
    for (int i=0;i<MAX_HTTP_CONN;i++) if (HC[i].fd==fd) return &HC[i];
    return NULL;
}
static void hc_del(int fd){
    for (int i=0;i<MAX_HTTP_CONN;i++) if (HC[i].fd==fd){
        free(HC[i].buf); HC[i].buf=NULL; HC[i].fd=0; HC[i].cap=HC[i].len=HC[i].need=HC[i].have_hdr=0;
        epoll_ctl(EPFD, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        return;
    }
}

static int http_listen(const char *ip, int port){
    int s=socket(AF_INET,SOCK_STREAM,0); if(s<0){perror("socket");return -1;}
    int one=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(s,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one));
#endif
    struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_port=htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr)!=1){ fprintf(stderr,"Bad http_bind: %s\n", ip); close(s); return -1; }
    if (bind(s,(struct sockaddr*)&a,sizeof(a))<0){ perror("http bind"); close(s); return -1; }
    if (set_nonblock(s)<0){ perror("http nb"); close(s); return -1; }
    if (listen(s,16)<0){ perror("listen"); close(s); return -1; }
    return s;
}

/* ---- tiny JSON helpers ---- */

static inline void strip_query(char *path){
    char *q = strchr(path, '?');
    if (q) *q = '\0';
}

static inline int json_get_int(const char *body, const char *key, int defv){
    const char *p = strstr(body, key);
    if(!p) return defv;
    const char *col=strchr(p,':'); if(!col) return defv;
    col++;
    while (*col && isspace((unsigned char)*col)) col++;
    char tmp[32]={0}; int i=0;
    while (*col && (isdigit((unsigned char)*col) || *col=='-') && i<31) tmp[i++]=*col++;
    int v=parse_int_bounded(tmp,-2147483647,2147483647);
    return (v==-1)?defv:v;
}

static inline int json_extract_port(const char *b){ return json_get_int(b,"\"port\"", -1); }

static inline int json_get_bool(const char *body, const char *key, int def){
    const char *p = strstr(body, key);
    if (!p) return def;
    const char *col = strchr(p, ':'); if (!col) return def;
    col++;
    while (*col && isspace((unsigned char)*col)) col++;
    if (!strncasecmp(col, "true", 4)) return 1;
    if (!strncasecmp(col, "false", 5)) return 0;
    return def;
}

/* extract {"dest":"ip:port"} into ip/port; returns 0 on success */
static int json_extract_dest_token(const char *body, char *ip, size_t iplen, int *port){
    const char *k = strstr(body, "\"dest\"");
    if (!k) return -1;
    const char *colon = strchr(k, ':');
    if (!colon) return -1;
    const char *v1 = strchr(colon, '"');
    if (!v1) return -1;
    v1++;
    const char *v2 = strchr(v1, '"');
    if (!v2) return -1;

    char token[128] = {0};
    size_t n = (size_t)(v2 - v1);
    if (n >= sizeof(token)) n = sizeof(token) - 1;
    memcpy(token, v1, n);
    token[n] = 0;

    char *c = strchr(token, ':');
    if (!c) return -1;
    *c = 0;
    int p = parse_int_bounded(c + 1, 1, 65535);
    if (p < 0) return -1;

    snprintf(ip, iplen, "%s", token);
    *port = p;
    return 0;
}

/* For clear_to, also accept "ip" and "port" fields */
static int json_extract_ip_port(const char *body, char *ip, size_t iplen, int *port){
    const char *ki = strstr(body, "\"ip\"");
    const char *kp = strstr(body, "\"port\"");
    if (!ki || !kp) return -1;
    const char *q = strchr(ki, '"'); if(!q) return -1;
    q = strchr(q+1,'"'); if(!q) return -1;
    const char *q2 = strchr(q+1,'"'); if(!q2) return -1;
    size_t n=(size_t)(q2-(q+1)); if (n>=iplen) n=iplen-1;
    memcpy(ip,q+1,n); ip[n]=0;
    int p = json_get_int(body,"\"port\"", -1); if (p<=0) return -1;
    *port=p;
    return 0;
}

/* dests: ["9000","1.2.3.4:7000","7000-7005"] */
static int apply_set_like(int port, const char *body, bool replace){
    if (port<=0) return -1;
    struct relay *r=NULL; for (int i=0;i<REL_N;i++) if (REL[i].src_port==port){ r=&REL[i]; break; }
    if (!r) return -2;

    /* Extract array slice of dests */
    const char *key="\"dests\"";
    const char *k=strstr(body,key); if(!k) return -3;
    const char *lb=strchr(k,'['); if(!lb) return -3;
    const char *rb=strchr(lb,']'); if(!rb) return -3;
    size_t n=(size_t)(rb - (lb+1));
    char *arr=malloc(n+1); if(!arr) return -3;
    memcpy(arr, lb+1, n); arr[n]=0;

    struct relay tmp={0};
    char *s=arr;
    while (*s){
        while (*s && (isspace((unsigned char)*s) || *s==',')) s++;
        if (!*s) break;
        if (*s=='"'){
            s++; char *e=strchr(s,'"'); if(!e) break;
            *e=0;
            if (parse_dest_token(&tmp, s)<0){ free(arr); return -4; }
            s=e+1;
        } else {
            char *e=s; while(*e && *e!=',') e++;
            char sv=*e; *e=0;
            if (strlen(s)) if(parse_dest_token(&tmp,trim(s))<0){ *e=sv; free(arr); return -4; }
            *e=sv; s=e;
        }
    }
    free(arr);

    if (replace){
        r->dest_cnt=0; /* stats preserved */
    }
    for (int i=0;i<tmp.dest_cnt && r->dest_cnt<MAX_DESTS;i++)
        r->dests[r->dest_cnt++]=tmp.dests[i];
    return 0;
}

/* append_range: {"port":5801,"ip":"1.2.3.4","start":7000,"end":7005} (ip optional) */
static int apply_append_range(const char *body){
    int port=json_extract_port(body); if(port<=0) return -1;
    int start=json_get_int(body,"\"start\"", -1);
    int end  =json_get_int(body,"\"end\"", -1);
    if (start<=0 || end<=0) return -1;
    if (start>end){ int t=start; start=end; end=t; }

    char ip[64]="127.0.0.1";
    const char *k=strstr(body,"\"ip\"");
    if (k){
        const char *q=strchr(k,'"'); if(q){ q=strchr(q+1,'"'); if(q){ const char *q2=strchr(q+1,'"'); if(q2){
            size_t n=(size_t)(q2-(q+1)); if (n>0 && n<sizeof(ip)){ memcpy(ip,q+1,n); ip[n]=0; }
        }}}}
    struct relay *r=NULL; for (int i=0;i<REL_N;i++) if (REL[i].src_port==port){ r=&REL[i]; break; }
    if (!r) return -2;

    for (int p=start; p<=end && r->dest_cnt<MAX_DESTS; p++){
        if (add_dest(r, ip, p)<0) break;
    }
    return 0;
}

/* Remove one destination from one relay (atomic) */
static int apply_clear_to(const char *body){
    int port = json_extract_port(body);
    char ip[64]={0}; int dport=-1;
    if (port<=0) return -1;

    /* Accept either {"dest":"ip:port"} or {"ip":"..","port":..} */
    if (json_extract_dest_token(body, ip, sizeof(ip), &dport)!=0){
        if (json_extract_ip_port(body, ip, sizeof(ip), &dport)!=0) return -1;
    }

    struct relay *r=NULL; for (int i=0;i<REL_N;i++) if (REL[i].src_port==port){ r=&REL[i]; break; }
    if (!r) return -2;

    struct sockaddr_in target={0};
    target.sin_family=AF_INET;
    target.sin_port=htons(dport);
    if (inet_pton(AF_INET, ip, &target.sin_addr)!=1) return -1;

    int idx=-1;
    for (int j=0;j<r->dest_cnt;j++){
        if (sockaddr_equal(&r->dests[j].addr, &target)){ idx=j; break; }
    }
    if (idx<0) return -3;

    /* remove by swapping with last to keep O(1) */
    r->dests[idx] = r->dests[r->dest_cnt-1];
    r->dest_cnt--;
    return 0;
}

static void http_send(int fd, const char *fmt, ...){
    char buf[4096];
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(buf,sizeof(buf),fmt,ap);
    va_end(ap);
    if (n<0) return;
    (void)send(fd, buf, (size_t)n, 0);
}

/* ------------------- HTTP handlers -------------------------- */

/* Ensure status JSON ≤ STATUS_CAP (soft cap); we truncate if needed. */
static void http_handle_status(int fd){
    char out[STATUS_CAP+256]; size_t off=0;
    #define APPEND(fmt,...) do { \
        int _n = snprintf(out + off, sizeof(out) - off, fmt, ##__VA_ARGS__); \
        if (_n < 0) { _n = 0; } \
        size_t _avail = sizeof(out) - off; \
        if ((size_t)_n > _avail) { _n = (int)_avail; } \
        off += (size_t)_n; \
        if (off >= STATUS_CAP) { goto SEND; } \
    } while (0)

    APPEND("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n");
    APPEND("{\"relays\":[");
    for (int i=0;i<REL_N;i++){
        if (i) APPEND(",");
        struct relay *r=&REL[i];
        uint64_t pkts_out_total=0;
        for (int j=0;j<r->dest_cnt;j++) pkts_out_total += r->dests[j].pkts_out;
        APPEND("{\"port\":%d,\"pkts_in\":%" PRIu64 ",\"bytes_in\":%" PRIu64 ",\"bytes_out\":%" PRIu64 ",\"send_errs\":%" PRIu64 ",\"last_rx_ns\":%" PRIu64 ",\"rx_drops\":%" PRIu64 ",\"trunc_drops\":%" PRIu64 ",\"pkts_out_total\":%" PRIu64 ",\"dests\":[",
               r->src_port, r->pkts_in, r->bytes_in, r->bytes_out, r->send_errs, r->last_rx_ns, r->rx_drops, r->trunc_drops, pkts_out_total);
        for (int j=0;j<r->dest_cnt;j++){
            if (j) APPEND(",");
            char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&r->dests[j].addr.sin_addr,ip,sizeof(ip));
            APPEND("{\"ip\":\"%s\",\"port\":%d,\"pkts\":%" PRIu64 "}", ip,
                   ntohs(r->dests[j].addr.sin_port), r->dests[j].pkts_out);
        }
        APPEND("]}");
    }
    APPEND("]}\n");
SEND:
    (void)send(fd, out, off, 0);
    #undef APPEND
}

static void http_handle_get_config(int fd){
    char *txt=NULL; size_t len=0;
    if (load_file(CFG_PATH,&txt,&len)!=0){
        http_send(fd,"HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nmissing config\n");
        return;
    }
    http_send(fd,"HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n");
    (void)send(fd, txt, len, 0);
    free(txt);
}

static void http_handle_post_config(int fd, const char *body, size_t len){
    if (save_file_atomic(CFG_TMP_PATH, CFG_PATH, body, len)!=0){
        http_send(fd,"HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\npersist failed\n");
        return;
    }
    struct config newc;
    if (load_ini_text(body, &newc)!=0){
        http_send(fd,"HTTP/1.0 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nbad ini\n");
        return;
    }
    G = newc;
    apply_config_relays(&G);
    http_send(fd,"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}\n");
}

static void http_handle_action(int fd, const char *verb, const char *body){
    int rc=-1;
    if (!strcmp(verb,"set")){
        int port=json_extract_port(body);
        rc=apply_set_like(port, body, true);
    } else if (!strcmp(verb,"append")){
        int port=json_extract_port(body);
        rc=apply_set_like(port, body, false);
    } else if (!strcmp(verb,"append_range")){
        rc=apply_append_range(body);
    } else if (!strcmp(verb,"clear")){
        int port=json_extract_port(body);
        if (port>0){
            struct relay *r=NULL; for (int i=0;i<REL_N;i++) if (REL[i].src_port==port){ r=&REL[i]; break; }
            if (r){ r->dest_cnt=0; rc=0; }
        }
    } else if (!strcmp(verb,"reset")){
        int port=json_extract_port(body);
        if (port>0){
            struct relay *r=NULL; for (int i=0;i<REL_N;i++) if (REL[i].src_port==port){ r=&REL[i]; break; }
            if (r){ r->pkts_in=r->bytes_in=r->bytes_out=r->send_errs=0;
                    for(int j=0;j<r->dest_cnt;j++) r->dests[j].pkts_out=0;
                    rc=0; }
        }
    } else if (!strcmp(verb,"clear_to")){
        rc=apply_clear_to(body);
    } else {
        http_send(fd,"HTTP/1.0 404 Not Found\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nunknown verb\n");
        return;
    }
    if (rc==0) http_send(fd,"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}\n");
    else       http_send(fd,"HTTP/1.0 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nbad action\n");
}

/* /api/v1/reload: GET->enqueue, POST with {"sync":true}->reload now */
static void http_handle_reload(int fd, const char *method, const char *body){
    if (!strcmp(method,"POST") && body && json_get_bool(body, "\"sync\"", 0)){
        int rc = reload_from_disk();
        if (rc==0){
            http_send(fd,"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true,\"reloaded\":true}\n");
        } else {
            http_send(fd,"HTTP/1.0 500 Internal Server Error\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":false,\"error\":\"reload_failed\"}\n");
        }
    } else {
        WANT_RELOAD = 1;
        http_send(fd,"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true,\"queued\":true}\n");
    }
}

/* ------------------- UI route ------------------------------- */

static void http_handle_ui(int fd){
    if (!UI_BUF || UI_LEN==0){
        http_send(fd,"HTTP/1.0 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\nUI not configured. Start with --ui /path/to/ui.html[.gz]\n");
        return;
    }
    if (UI_IS_GZIP){
        http_send(fd,"HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Encoding: gzip\r\nConnection: close\r\n\r\n");
    } else {
        http_send(fd,"HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
    }
    (void)send(fd, UI_BUF, UI_LEN, 0);
}

/* ------------------- HTTP dispatcher ------------------------ */

static inline void handle_http_on_fd(int fd, uint32_t evs){
    struct http_conn *hc = hc_find(fd);
    if (!hc || hc->fd!=fd) return;
    if (evs & (EPOLLHUP|EPOLLERR)){ hc_del(fd); return; }
    if (!(evs & EPOLLIN)) return;

    char tmp[4096];
    while (1){
        ssize_t r=recv(fd,tmp,sizeof(tmp),0);
        if (r>0){
            if (hc->len + (size_t)r > HTTP_BUF_MAX){ hc_del(fd); break; }
            if (hc->len + (size_t)r > hc->cap){
                size_t ncap = hc->cap*2; if (ncap < hc->len+(size_t)r) ncap = hc->len+(size_t)r;
                if (ncap>HTTP_BUF_MAX) ncap=HTTP_BUF_MAX;
                char *nb=realloc(hc->buf,ncap); if(!nb){ hc_del(fd); break; }
                hc->buf=nb; hc->cap=ncap;
            }
            memcpy(hc->buf+hc->len, tmp, (size_t)r);
            hc->len += (size_t)r;

            char *hdr = hc->buf;
            char *hdr_end = strstr(hdr, "\r\n\r\n");
            if (!hdr_end) continue;

            char method[8]={0}, path[256]={0};
            if (sscanf(hdr,"%7s %255s",method,path)!=2){
                http_send(fd,"HTTP/1.0 400 Bad Request\r\nConnection: close\r\n\r\n");
                hc_del(fd); break;
            }

            strip_query(path);

            size_t clen=0;
            char *cl = strcasestr(hdr,"Content-Length:");
            if (cl) clen = (size_t)strtoul(cl+15,NULL,10);

            size_t hdrlen = (size_t)(hdr_end + 4 - hdr);
            size_t have_body = (hc->len > hdrlen) ? hc->len - hdrlen : 0;
            if (have_body < clen) continue;

            const char *body = hc->buf + hdrlen;

            if (!strcmp(method,"GET") && !strcmp(path,"/api/v1/status")){
                http_handle_status(fd);
            } else if (!strcmp(method,"GET") && !strcmp(path,"/api/v1/config")){
                http_handle_get_config(fd);
            } else if (!strcmp(method,"POST") && !strcmp(path,"/api/v1/config")){
                http_handle_post_config(fd, body, clen);
            } else if (!strcmp(method,"POST") && !strncmp(path,"/api/v1/action/",15)){
                const char *verb = path + 15;
                http_handle_action(fd, verb, body);
            } else if ((!strcmp(method,"GET") || !strcmp(method,"POST")) && !strcmp(path,"/api/v1/reload")){
                http_handle_reload(fd, method, body);
            } else if (!strcmp(method,"GET") &&
                       ( !strcmp(path,"/") || !strcmp(path,"/ui") || !strcmp(path,"/ui/") || !strcmp(path,"/ui/index.html") )){
                http_handle_ui(fd);
            } else if (!strcmp(method,"GET") && !strcmp(path,"/favicon.ico")){
                http_send(fd,"HTTP/1.0 204 No Content\r\nConnection: close\r\n\r\n");
            } else {
                http_send(fd,"HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n");
            }

            hc_del(fd);
            break;
        } else if (r==0){ hc_del(fd); break; }
        else { if (errno==EAGAIN||errno==EWOULDBLOCK) break; hc_del(fd); break; }
    }
}

/* ------------------- signal handlers ------------------------- */

static void sig_handler(int sig){
    if (sig==SIGHUP) WANT_RELOAD=1;
    else if (sig==SIGINT || sig==SIGTERM) WANT_EXIT=1;
}

/* ------------------- counter roll-over ----------------------- */

static void maybe_rollover_relay(struct relay *r){
    if (r->pkts_in > PKTS_ROLLOVER_LIMIT ||
        r->bytes_in > BYTES_ROLLOVER_LIMIT ||
        r->bytes_out > BYTES_ROLLOVER_LIMIT ||
        r->send_errs > PKTS_ROLLOVER_LIMIT)
    {
        r->pkts_in  >>= 1;
        r->bytes_in  >>= 1;
        r->bytes_out >>= 1;
        r->send_errs >>= 1;
        for (int j=0;j<r->dest_cnt;j++){
            r->dests[j].pkts_out >>= 1;
        }
    }
}

/* ------------------- UI loader ------------------------------- */

static void detect_gzip_by_ext_or_magic(const char *path, const char *buf, size_t len){
    UI_IS_GZIP = 0;
    if (!path) return;
    size_t L = strlen(path);
    if (L>=3 && (!strcasecmp(path+L-3,".gz"))) UI_IS_GZIP = 1;
    else if (L>=6 && (!strcasecmp(path+L-6,".gzip"))) UI_IS_GZIP = 1;
    if (!UI_IS_GZIP && buf && len>=2){
        const unsigned char *b=(const unsigned char*)buf;
        if (b[0]==0x1f && b[1]==0x8b) UI_IS_GZIP = 1;
    }
}

static int load_ui_file(const char *path){
    if (!path) return -1;
    if (load_file(path, &UI_BUF, &UI_LEN)!=0) return -1;
    detect_gzip_by_ext_or_magic(path, UI_BUF, UI_LEN);
    fprintf(stderr,"Loaded UI file %s (%zu bytes, gzip=%s)\n", path, UI_LEN, UI_IS_GZIP?"yes":"no");
    return 0;
}

/* ------------------- reload helper --------------------------- */

static int reload_from_disk(void){
    struct config nc;
    if (load_ini_file(&nc)!=0){
        fprintf(stderr,"Reload failed: bad INI\n");
        return -1;
    }
    G = nc;

    /* Recreate HTTP listener (bind/port may have changed) */
    if (HTTP_LFD>=0){
        epoll_ctl(EPFD, EPOLL_CTL_DEL, HTTP_LFD, NULL);
        close(HTTP_LFD);
        HTTP_LFD = -1;
    }
    HTTP_LFD = http_listen(G.http_bind, G.control_port);
    if (HTTP_LFD<0){
        fprintf(stderr,"Reload failed: http listen\n");
        return -2;
    }
    struct epoll_event ev={.events=EPOLLIN, .data.fd=HTTP_LFD};
    epoll_ctl(EPFD, EPOLL_CTL_ADD, HTTP_LFD, &ev);

    /* Re-apply relay sockets */
    if (apply_config_relays(&G)!=0){
        fprintf(stderr,"Reload warning: no valid binds\n");
        /* still proceed; allows clearing */
    }

    /* Resize UDP buffer */
    if (UDP_BUF){ free(UDP_BUF); UDP_BUF=NULL; }
    UDP_BUF = malloc((size_t)G.bufsz);
    if (!UDP_BUF){
        perror("malloc");
        return -3;
    }

    fprintf(stderr,"Reloaded config\n");
    return 0;
}

/* ------------------- main loop -------------------------------- */

static void usage(const char *argv0){
    fprintf(stderr,
        "Usage: %s [--ui /path/to/ui.html[.gz]]\n"
        "  Serves /api/v1/* and optional /ui if --ui is given.\n", argv0);
}

int main(int argc, char **argv){
    const char *ui_path = NULL;
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")){
            usage(argv[0]); return 0;
        } else if (!strcmp(argv[i],"--ui") && i+1<argc){
            ui_path = argv[++i];
        } else if (!strncmp(argv[i],"--ui=",5)){
            ui_path = argv[i]+5;
        } else {
            fprintf(stderr,"Unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }
    if (ui_path){
        if (load_ui_file(ui_path)!=0){
            fprintf(stderr,"Failed to load UI file: %s\n", ui_path);
            return 1;
        }
    }

    struct sigaction sa={0};
    sa.sa_handler = sig_handler;
    sigaction(SIGHUP,&sa,NULL);
    sigaction(SIGINT,&sa,NULL);
    sigaction(SIGTERM,&sa,NULL);
    signal(SIGPIPE, SIG_IGN);

    EPFD = epoll_create1(EPOLL_CLOEXEC);
    if (EPFD<0){ perror("epoll_create1"); return 1; }

    if (load_ini_file(&G)!=0){ fprintf(stderr,"Bad INI, using defaults\n"); cfg_defaults(&G); }
    if (G.bufsz<=0) G.bufsz=9000;

    HTTP_LFD = http_listen(G.http_bind, G.control_port);
    if (HTTP_LFD<0){ fprintf(stderr,"HTTP listen failed\n"); return 1; }
    struct epoll_event ev={.events=EPOLLIN, .data.fd=HTTP_LFD};
    epoll_ctl(EPFD, EPOLL_CTL_ADD, HTTP_LFD, &ev);

    if (apply_config_relays(&G)!=0){
        fprintf(stderr,"No valid bind entries; exiting.\n");
        return 1;
    }

    UDP_BUF = malloc((size_t)G.bufsz);
    if (!UDP_BUF){ perror("malloc"); return 1; }

    struct epoll_event events[MAX_EVENTS];

    while (!WANT_EXIT){
        if (WANT_RELOAD){
            WANT_RELOAD=0;
            (void)reload_from_disk();
        }

        int n = epoll_wait(EPFD, events, MAX_EVENTS, 1000 /*ms*/);
        if (n<0){
            if (errno==EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i=0;i<n;i++){
            int fd = events[i].data.fd;
            uint32_t evs = events[i].events;

            if (fd==HTTP_LFD && (evs & EPOLLIN)){
                while (1){
                    int c=accept(HTTP_LFD, NULL, NULL);
                    if (c<0){ if (errno==EAGAIN||errno==EWOULDBLOCK) break; else { perror("accept"); break; } }
                    set_nonblock(c);
                    struct http_conn *hc=hc_get(c);
                    if (!hc){ close(c); continue; }
                    struct epoll_event cev={.events=EPOLLIN, .data.fd=c};
                    epoll_ctl(EPFD, EPOLL_CTL_ADD, c, &cev);
                }
                continue;
            }

            /* HTTP client readable */
            if (hc_find(fd)){ handle_http_on_fd(fd, evs); continue; }

            /* UDP readable on a relay */
            if (evs & EPOLLIN){
                struct relay *r=NULL; for (int k=0;k<REL_N;k++) if (REL[k].fd==fd){ r=&REL[k]; break; }
                if (!r) continue;

                while (1){
                    /* recvmsg with SO_RXQ_OVFL accumulation and truncation detection */
                    struct iovec iov = { .iov_base = UDP_BUF, .iov_len = (size_t)G.bufsz };
                    char cbuf[CMSG_SPACE(sizeof(uint32_t))];
                    struct msghdr msg = {0};
                    msg.msg_iov = &iov; msg.msg_iovlen = 1;
                    msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);

                    ssize_t m = recvmsg(fd, &msg, 0);
                    if (m>0){
                        /* count kernel drop cmsg (dropped since last packet) */
                        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)){
#ifdef SO_RXQ_OVFL
                            if (cm->cmsg_level==SOL_SOCKET && cm->cmsg_type==SO_RXQ_OVFL){
                                uint32_t dropped = 0;
                                memcpy(&dropped, CMSG_DATA(cm), sizeof(dropped));
                                r->rx_drops += (uint64_t)dropped;
                            }
#endif
                        }

                        /* drop & count truncated datagrams (avoid forwarding partial frames) */
                        if (msg.msg_flags & MSG_TRUNC){
                            r->trunc_drops++;
                            continue;
                        }

                        r->pkts_in++; r->bytes_in += (uint64_t)m; r->last_rx_ns = now_ns();

                        int cnt = r->dest_cnt;
                        if (cnt <= 0){ maybe_rollover_relay(r); continue; }
                        if (cnt > MAX_DESTS) cnt = MAX_DESTS;

                        /* snapshot addresses so HTTP mutations won't race */
                        struct dest snap[MAX_DESTS];
                        memcpy(snap, r->dests, (size_t)cnt*sizeof(struct dest));

                        /* build sendmmsg batch (same payload to N dests) */
                        struct mmsghdr msgs[MAX_DESTS];
                        struct iovec   siov[MAX_DESTS];
                        memset(msgs,0,(size_t)cnt*sizeof(struct mmsghdr));
                        for (int d=0; d<cnt; d++){
                            siov[d].iov_base = UDP_BUF;
                            siov[d].iov_len  = (size_t)m;
                            msgs[d].msg_hdr.msg_iov = &siov[d];
                            msgs[d].msg_hdr.msg_iovlen = 1;
                            msgs[d].msg_hdr.msg_name = &snap[d].addr;
                            msgs[d].msg_hdr.msg_namelen = sizeof(snap[d].addr);
                        }

                        int sent_total = 0;
                        while (sent_total < cnt){
                            int rc = sendmmsg(fd, msgs + sent_total, (unsigned)(cnt - sent_total), 0);
                            if (rc > 0){
                                for (int j=0; j<rc; j++){
                                    r->bytes_out += (uint64_t)m;
                                    struct sockaddr_in *sa = (struct sockaddr_in*)msgs[sent_total + j].msg_hdr.msg_name;
                                    for (int x=0; x<r->dest_cnt; x++){
                                        if (sockaddr_equal(&r->dests[x].addr, sa)){
                                            r->dests[x].pkts_out++;
                                            break;
                                        }
                                    }
                                }
                                sent_total += rc;
                            } else if (rc < 0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
                                r->send_errs += (uint64_t)(cnt - sent_total);
                                break;
                            } else {
                                r->send_errs += (uint64_t)(cnt - sent_total);
                                break;
                            }
                        }

                        maybe_rollover_relay(r);
                    } else if (m<0){
                        if (errno==EAGAIN||errno==EWOULDBLOCK) break;
                        break;
                    } else { break; }
                }
            }
        }
    }

    if (HTTP_LFD>=0){ epoll_ctl(EPFD, EPOLL_CTL_DEL, HTTP_LFD, NULL); close(HTTP_LFD); }
    close_relays();
    for (int i=0;i<MAX_HTTP_CONN;i++) if (HC[i].fd) hc_del(HC[i].fd);
    if (UDP_BUF) free(UDP_BUF);
    if (UI_BUF) free(UI_BUF);
    if (EPFD>=0) close(EPFD);
    return 0;
}
