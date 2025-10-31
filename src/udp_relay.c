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
 * Config file: /etc/udp_relay/udp_relay.conf
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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <asm/ioctls.h>
#define termios asm_termios
#include <asm/termbits.h>
#undef termios
#endif

/* ------------------- tunables & constants ------------------- */

#define MAX_RELAYS      64
#define MAX_DESTS       128
#define MAX_BINDS       64
#define MAX_UARTS       4
#define MAX_LINE        1024
#define MAX_EVENTS      128
#define MAX_HTTP_CONN   64
#define HTTP_BUF_MAX    65536
#define STATUS_CAP      8192
#define CFG_PATH        "/etc/udp_relay/udp_relay.conf"
#define CFG_TMP_PATH    "/etc/udp_relay/udp_relay.conf.tmp"

#define UART_TX_BUF_DEFAULT 4096
#define UART_RX_BUF_DEFAULT 4096

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

static void uart_token_format(int idx, char *out, size_t len){
    if (!out || len == 0) return;
    if (idx <= 0){
        snprintf(out, len, "uart");
    } else {
        snprintf(out, len, "uart%d", idx);
    }
}

static int uart_token_parse(const char *token){
    if (!token) return -1;
    if (strncasecmp(token, "uart", 4) != 0) return -1;
    const char *p = token + 4;
    if (*p == '\0') return 0;
    if (!isdigit((unsigned char)*p)) return -1;
    char *end=NULL;
    long idx = strtol(p, &end, 10);
    if (end == p || *end != '\0') return -1;
    if (idx < 0 || idx >= MAX_UARTS) return -1;
    if (idx == 0) return 0;
    return (int)idx;
}

/* ------------------- small ring buffer ---------------------- */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   head;
    size_t   tail;
    size_t   len;
} ringbuf_t;

static int ring_init(ringbuf_t *r, size_t cap){
    r->buf = NULL;
    r->cap = r->head = r->tail = r->len = 0;
    if (!cap) return 0;
    r->buf = (uint8_t*)malloc(cap);
    if (!r->buf){ errno = ENOMEM; return -1; }
    r->cap = cap;
    return 0;
}

static void ring_free(ringbuf_t *r){
    free(r->buf);
    r->buf = NULL;
    r->cap = r->head = r->tail = r->len = 0;
}

static size_t ring_space(const ringbuf_t *r){
    if (r->cap < r->len) return 0;
    return r->cap - r->len;
}

static size_t ring_write(ringbuf_t *r, const uint8_t *src, size_t n){
    if (!r->cap || !n) return 0;
    size_t w = n;
    if (w > ring_space(r)) w = ring_space(r);
    size_t first = w > (r->cap - r->head) ? (r->cap - r->head) : w;
    if (first) memcpy(r->buf + r->head, src, first);
    size_t second = w - first;
    if (second) memcpy(r->buf, src + first, second);
    r->head = (r->head + w) % r->cap;
    r->len += w;
    return w;
}

static size_t ring_peek(const ringbuf_t *r, const uint8_t **p1, size_t *l1,
                        const uint8_t **p2, size_t *l2){
    if (!r->len){
        if (p1) *p1 = NULL;
        if (l1) *l1 = 0;
        if (p2) *p2 = NULL;
        if (l2) *l2 = 0;
        return 0;
    }
    size_t first = r->len > (r->cap - r->tail) ? (r->cap - r->tail) : r->len;
    if (p1) *p1 = r->buf + r->tail;
    if (l1) *l1 = first;
    if (p2) *p2 = NULL;
    if (l2) *l2 = 0;
    if (r->len > first){
        if (p2) *p2 = r->buf;
        if (l2) *l2 = r->len - first;
    }
    return r->len;
}

static void ring_consume(ringbuf_t *r, size_t n){
    if (n > r->len) n = r->len;
    r->tail = (r->tail + n) % r->cap;
    r->len -= n;
}

/* ------------------- UART helpers --------------------------- */

static speed_t baud_to_speed(int baud){
    switch (baud){
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
        default: return 0;
    }
}

static int set_custom_baud(int fd, int baud){
#if defined(__linux__) && defined(TCGETS2) && defined(TCSETS2)
    struct termios2 tio2;
    if (ioctl(fd, TCGETS2, &tio2) < 0) return -1;
    tio2.c_cflag &= ~CBAUD;
    tio2.c_cflag |= BOTHER;
    tio2.c_ispeed = baud;
    tio2.c_ospeed = baud;
    return ioctl(fd, TCSETS2, &tio2);
#else
    (void)fd; (void)baud;
    errno = EINVAL;
    return -1;
#endif
}

/* ------------------- config model --------------------------- */

enum dest_type {
    DEST_UDP = 0,
    DEST_UART = 1,
};

struct dest {
    int type;
    struct sockaddr_in addr;
    int uart_idx;
    uint64_t pkts_out;
};

enum relay_kind {
    RELAY_KIND_UDP = 0,
    RELAY_KIND_UART = 1,
};

struct relay {
    int kind;
    int src_port;
    int fd;
    int uart_idx;
    struct dest dests[MAX_DESTS];
    int dest_cnt;
    uint64_t pkts_in, bytes_in, bytes_out, send_errs, last_rx_ns;
    uint64_t rx_drops;    /* kernel-reported drops (SO_RXQ_OVFL accumulation) */
    uint64_t trunc_drops; /* datagrams > bufsz (MSG_TRUNC) — dropped instead of forwarded */
};

static void relay_id_format(const struct relay *r, char *out, size_t len){
    if (!out || len == 0){
        return;
    }
    if (!r){
        out[0] = '\0';
        return;
    }
    if (r->kind == RELAY_KIND_UART){
        uart_token_format(r->uart_idx, out, len);
    } else {
        snprintf(out, len, "%d", r->src_port);
    }
}

static void maybe_rollover_relay(struct relay *r);

struct uart_cfg {
    char device[128];
    int  baud;
    int  databits;
    char parity[8];
    int  stopbits;
    char flow[16];
    int  tx_buf;
    int  rx_buf;
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
    struct uart_cfg uart[MAX_UARTS];
};

static struct config G;                    /* current config */
static struct relay REL[MAX_RELAYS];       /* active relays */
static int REL_N = 0;

struct uart_runtime {
    int enabled;
    int index;
    char token[16];
    int fd;          /* UART file descriptor */
    int udp_fd;      /* UDP socket for UART->UDP forwarding */
    ringbuf_t out;   /* pending bytes to write to UART */
    uint8_t *rx_buf; /* buffer for UART reads */
    size_t   rx_buf_cap;
    struct relay *relay;
    struct uart_cfg cfg;
    uint64_t pkts_in;    /* UART -> UDP */
    uint64_t bytes_in;
    uint64_t pkts_out;   /* UDP -> UART */
    uint64_t bytes_out;
    uint64_t send_errs;
    uint64_t drops;
    uint64_t last_rx_ns;
};

static struct uart_runtime UARTS[MAX_UARTS];

static void uart_runtime_init_all(void){
    for (int i=0;i<MAX_UARTS;i++){
        struct uart_runtime *u = &UARTS[i];
        memset(u, 0, sizeof(*u));
        u->fd = -1;
        u->udp_fd = -1;
        u->index = i;
        uart_token_format(i, u->token, sizeof(u->token));
        u->relay = NULL;
    }
}

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
    for (int i=0;i<MAX_UARTS;i++){
        struct uart_cfg *u = &c->uart[i];
        u->device[0] = '\0';
        u->baud = 115200;
        u->databits = 8;
        snprintf(u->parity, sizeof(u->parity), "none");
        u->stopbits = 1;
        snprintf(u->flow, sizeof(u->flow), "none");
        u->tx_buf = UART_TX_BUF_DEFAULT;
        u->rx_buf = UART_RX_BUF_DEFAULT;
    }
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
        } else if(!strncmp(key,"uart",4)){
            int idx = 0;
            const char *p = key + 4;
            if (*p && isdigit((unsigned char)*p)){
                char *end=NULL;
                long v = strtol(p, &end, 10);
                if (end && end!=p){
                    idx = (int)v;
                    p = end;
                } else {
                    idx = -1;
                }
            }
            if (idx < 0 || idx >= MAX_UARTS) continue;
            if (*p != '_') continue;
            const char *attr = p + 1;
            struct uart_cfg *u = &c->uart[idx];
            if (!strcmp(attr,"device")){
                snprintf(u->device, sizeof(u->device), "%s", val);
            } else if (!strcmp(attr,"baud")){
                int v=parse_int_bounded(val,1200,10000000); if(v>0) u->baud=v;
            } else if (!strcmp(attr,"databits")){
                int v=parse_int_bounded(val,5,8); if(v>0) u->databits=v;
            } else if (!strcmp(attr,"parity")){
                snprintf(u->parity, sizeof(u->parity), "%s", val);
            } else if (!strcmp(attr,"stopbits")){
                int v=parse_int_bounded(val,1,2); if(v>0) u->stopbits=v;
            } else if (!strcmp(attr,"flow")){
                snprintf(u->flow, sizeof(u->flow), "%s", val);
            } else if (!strcmp(attr,"tx_buf")){
                int v=parse_int_bounded(val,128,4*1024*1024); if(v>0) u->tx_buf=v;
            } else if (!strcmp(attr,"rx_buf")){
                int v=parse_int_bounded(val,128,4*1024*1024); if(v>0) u->rx_buf=v;
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

static int add_dest_udp(struct dest *dests, int *cnt, int max, const char *ip, int port){
    if (*cnt >= max) return -1;
    struct dest *d=&dests[*cnt];
    memset(d,0,sizeof(*d));
    d->type = DEST_UDP;
    d->addr.sin_family=AF_INET;
    d->addr.sin_port=htons(port);
    if (inet_pton(AF_INET, ip, &d->addr.sin_addr)!=1) return -1;
    d->pkts_out=0;
    (*cnt)++;
    return 0;
}

static int parse_dest_token(struct dest *dests, int *cnt, int max, const char *tok, bool allow_uart){
    if (!tok) return -1;
    char buf[128]; snprintf(buf,sizeof(buf),"%s",tok);
    char *s=trim(buf);
    if (!*s) return -1;

    int uart_idx = uart_token_parse(s);
    if (uart_idx >= 0){
        if (!allow_uart) return -1;
        for (int i=0;i<*cnt;i++){
            if (dests[i].type==DEST_UART && dests[i].uart_idx==uart_idx) return 0;
        }
        if (*cnt >= max) return -1;
        struct dest *d=&dests[*cnt];
        memset(d,0,sizeof(*d));
        d->type = DEST_UART;
        d->uart_idx = uart_idx;
        d->pkts_out = 0;
        (*cnt)++;
        return 0;
    }

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
            if (add_dest_udp(dests, cnt, max, ip, p)<0) break;
        }
        return 0;
    } else {
        int p=parse_int_bounded(port_part,1,65535);
        if (p<0) return -1;
        return add_dest_udp(dests, cnt, max, ip, p);
    }
}

static int parse_dest_list(struct dest *dests, int *cnt, int max, const char *list, bool replace, bool allow_uart){
    struct dest tmp[MAX_DESTS];
    int tmp_cnt = 0;
    if (list && *list){
        char *dup=strdup(list); if(!dup) return -1;
        char *save=NULL;
        for(char *tok=strtok_r(dup,",",&save); tok; tok=strtok_r(NULL,",",&save)){
            if (parse_dest_token(tmp, &tmp_cnt, MAX_DESTS, trim(tok), allow_uart)<0){ free(dup); return -1; }
        }
        free(dup);
    }
    if (replace){
        *cnt = 0; /* stats preserved by caller if needed */
    }
    for (int i=0;i<tmp_cnt && *cnt<max;i++){
        dests[*cnt] = tmp[i];
        (*cnt)++;
    }
    return 0;
}

static int uart_open_fd(const struct uart_cfg *cfg){
    int fd = open(cfg->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0){
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    cfmakeraw(&tio);

    speed_t sp = baud_to_speed(cfg->baud);
    if (sp){
        cfsetispeed(&tio, sp);
        cfsetospeed(&tio, sp);
    } else {
        cfsetispeed(&tio, B38400);
        cfsetospeed(&tio, B38400);
    }

    tio.c_cflag &= ~CSIZE;
    switch (cfg->databits){
        case 5: tio.c_cflag |= CS5; break;
        case 6: tio.c_cflag |= CS6; break;
        case 7: tio.c_cflag |= CS7; break;
        default: tio.c_cflag |= CS8; break;
    }

    if (!strcasecmp(cfg->parity, "even")){
        tio.c_cflag |= PARENB;
        tio.c_cflag &= ~PARODD;
    } else if (!strcasecmp(cfg->parity, "odd")){
        tio.c_cflag |= PARENB;
        tio.c_cflag |= PARODD;
    } else {
        tio.c_cflag &= ~PARENB;
    }

    if (cfg->stopbits == 2) tio.c_cflag |= CSTOPB;
    else                       tio.c_cflag &= ~CSTOPB;

    if (!strcasecmp(cfg->flow, "rtscts")) tio.c_cflag |= CRTSCTS;
    else                                      tio.c_cflag &= ~CRTSCTS;

    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0){
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (!sp){
        if (set_custom_baud(fd, cfg->baud) < 0){
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }

    return fd;
}

static int uart_open_udp_socket(const char *ip, int port){
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    struct sockaddr_in sa={0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1){
        int saved = errno;
        close(fd);
        errno = saved ? saved : EINVAL;
        return -1;
    }
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0){
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    if (set_nonblock(fd) < 0){
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}


static void uart_update_epoll_interest(struct uart_runtime *u){
    if (!u || u->fd < 0 || EPFD < 0) return;
    uint32_t events = EPOLLIN;
    if (u->out.len > 0) events |= EPOLLOUT;
    struct epoll_event ev={.events=events, .data.fd=u->fd};
    if (epoll_ctl(EPFD, EPOLL_CTL_MOD, u->fd, &ev)<0){
        /* best effort; ignore */
    }
}

static void uart_flush_output(struct uart_runtime *u){
    if (!u || u->fd < 0) return;
    while (u->out.len > 0){
        const uint8_t *p1=NULL,*p2=NULL;
        size_t l1=0,l2=0;
        ring_peek(&u->out, &p1, &l1, &p2, &l2);
        if (!l1) break;
        ssize_t w = write(u->fd, p1, l1);
        if (w > 0){
            ring_consume(&u->out, (size_t)w);
            continue;
        } else if (w < 0 && errno == EINTR){
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
            break;
        } else {
            u->send_errs++;
            size_t drop = u->out.len;
            ring_consume(&u->out, drop);
            break;
        }
    }
}

static int uart_send_bytes(struct uart_runtime *u, const uint8_t *data, size_t len){
    if (!u || u->fd < 0 || !data || !len) return -1;

    uart_flush_output(u);

    size_t done = 0;
    while (done < len){
        ssize_t w = write(u->fd, data + done, len - done);
        if (w > 0){
            done += (size_t)w;
            continue;
        } else if (w < 0 && errno == EINTR){
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
            break;
        } else {
            u->send_errs++;
            return -1;
        }
    }

    if (done < len){
        size_t queued = ring_write(&u->out, data + done, len - done);
        if (queued < len - done){
            u->drops += (uint64_t)((len - done) - queued);
            u->send_errs++;
            return -1;
        }
        uart_update_epoll_interest(u);
    }

    return 0;
}

static void uart_maybe_rollover(struct uart_runtime *u){
    if (!u) return;
    if (u->pkts_in > PKTS_ROLLOVER_LIMIT ||
        u->bytes_in > BYTES_ROLLOVER_LIMIT ||
        u->pkts_out > PKTS_ROLLOVER_LIMIT ||
        u->bytes_out > BYTES_ROLLOVER_LIMIT ||
        u->send_errs > PKTS_ROLLOVER_LIMIT ||
        u->drops > PKTS_ROLLOVER_LIMIT)
    {
        u->pkts_in  >>= 1;
        u->bytes_in  >>= 1;
        u->pkts_out >>= 1;
        u->bytes_out >>= 1;
        u->send_errs >>= 1;
        u->drops    >>= 1;
        if (u->relay){
            for (int i=0;i<u->relay->dest_cnt;i++) u->relay->dests[i].pkts_out >>= 1;
        }
    }
}

static void uart_close(struct uart_runtime *u){
    if (!u) return;
    if (u->fd >= 0){
        if (EPFD >= 0) epoll_ctl(EPFD, EPOLL_CTL_DEL, u->fd, NULL);
        close(u->fd);
    }
    if (u->udp_fd >= 0){
        close(u->udp_fd);
    }
    ring_free(&u->out);
    if (u->rx_buf){
        free(u->rx_buf);
        u->rx_buf = NULL;
    }
    u->rx_buf_cap = 0;
    u->fd = -1;
    u->udp_fd = -1;
    u->enabled = 0;
    memset(&u->cfg, 0, sizeof(u->cfg));
    u->pkts_in = u->bytes_in = 0;
    u->pkts_out = u->bytes_out = 0;
    u->send_errs = u->drops = 0;
    u->last_rx_ns = 0;
}

static int uart_apply_one(int idx, const struct config *c){
    if (idx < 0 || idx >= MAX_UARTS) return -1;
    struct uart_runtime *u = &UARTS[idx];
    const struct uart_cfg *src = &c->uart[idx];

    if (!src->device[0]){
        uart_close(u);
        return 0;
    }

    int fd = uart_open_fd(src);
    if (fd < 0){
        perror("uart open");
        return -1;
    }

    size_t tx_cap = (size_t)((src->tx_buf>0)?src->tx_buf:UART_TX_BUF_DEFAULT);
    ringbuf_t new_out;
    if (ring_init(&new_out, tx_cap) < 0){
        close(fd);
        return -1;
    }

    size_t rx_cap = (size_t)((src->rx_buf>0)?src->rx_buf:UART_RX_BUF_DEFAULT);
    uint8_t *rx_buf = malloc(rx_cap);
    if (!rx_buf){
        ring_free(&new_out);
        close(fd);
        return -1;
    }

    const char *bind_ip = c->src_ip[0] ? c->src_ip : "0.0.0.0";
    int udp_fd = uart_open_udp_socket(bind_ip, 0);
    if (udp_fd < 0){
        perror("uart udp bind");
        free(rx_buf);
        ring_free(&new_out);
        close(fd);
        return -1;
    }

    uart_close(u);

    u->fd = fd;
    u->udp_fd = udp_fd;
    u->out = new_out;
    u->rx_buf = rx_buf;
    u->rx_buf_cap = rx_cap;
    u->cfg = *src;
    u->cfg.tx_buf = (int)tx_cap;
    u->cfg.rx_buf = (int)rx_cap;
    u->enabled = 1;
    u->pkts_in = u->bytes_in = 0;
    u->pkts_out = u->bytes_out = 0;
    u->send_errs = u->drops = 0;
    u->last_rx_ns = 0;

    if (EPFD >= 0){
        struct epoll_event ev={.events=EPOLLIN, .data.fd=u->fd};
        if (epoll_ctl(EPFD, EPOLL_CTL_ADD, u->fd, &ev)<0){
            perror("epoll_ctl add uart");
            uart_close(u);
            return -1;
        }
    }

    int dests = (u->relay) ? u->relay->dest_cnt : 0;
    fprintf(stderr, "UART[%s] enabled on %s (baud=%d, dests=%d)\n",
            u->token[0] ? u->token : "uart", u->cfg.device, u->cfg.baud, dests);
    return 0;
}

static int uart_apply_config_all(const struct config *c){
    int rc = 0;
    for (int i=0;i<MAX_UARTS;i++){
        if (uart_apply_one(i, c) != 0) rc = -1;
    }
    return rc;
}

static int uart_send_from_udp(struct uart_runtime *u, const uint8_t *data, size_t len){
    if (!u || !u->enabled || u->fd < 0) return -1;
    if (uart_send_bytes(u, data, len) == 0){
        u->pkts_out++;
        u->bytes_out += (uint64_t)len;
        uart_maybe_rollover(u);
        return 0;
    }
    return -1;
}

static void uart_handle_write(struct uart_runtime *u){
    uart_flush_output(u);
    uart_update_epoll_interest(u);
}

static void uart_handle_read(struct uart_runtime *u){
    if (!u || !u->enabled || u->fd < 0 || !u->rx_buf || u->rx_buf_cap==0) return;
    struct relay *relay_binding = u->relay;
    while (1){
        ssize_t r = read(u->fd, u->rx_buf, u->rx_buf_cap);
        if (r > 0){
            u->pkts_in++;
            u->bytes_in += (uint64_t)r;
            u->last_rx_ns = now_ns();
            if (relay_binding && relay_binding->dest_cnt > 0 && u->udp_fd >= 0){
                int cnt = 0;
                struct mmsghdr msgs[MAX_DESTS];
                struct iovec iov[MAX_DESTS];
                struct dest *refs[MAX_DESTS];
                for (int i=0;i<relay_binding->dest_cnt;i++){
                    if (relay_binding->dests[i].type != DEST_UDP) continue;
                    if (cnt >= MAX_DESTS) break;
                    refs[cnt] = &relay_binding->dests[i];
                    memset(&msgs[cnt], 0, sizeof(struct mmsghdr));
                    iov[cnt].iov_base = u->rx_buf;
                    iov[cnt].iov_len = (size_t)r;
                    msgs[cnt].msg_hdr.msg_iov = &iov[cnt];
                    msgs[cnt].msg_hdr.msg_iovlen = 1;
                    msgs[cnt].msg_hdr.msg_name = &relay_binding->dests[i].addr;
                    msgs[cnt].msg_hdr.msg_namelen = sizeof(relay_binding->dests[i].addr);
                    cnt++;
                }
                int sent = 0;
                while (sent < cnt){
                    int rc = sendmmsg(u->udp_fd, msgs + sent, (unsigned)(cnt - sent), 0);
                    if (rc > 0){
                        for (int j=0;j<rc;j++){
                            refs[sent + j]->pkts_out++;
                        }
                        sent += rc;
                    } else if (rc < 0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==ENOBUFS)){
                        u->send_errs += (uint64_t)(cnt - sent);
                        break;
                    } else {
                        u->send_errs += (uint64_t)(cnt - sent);
                        break;
                    }
                }
            }
            uart_maybe_rollover(u);
            if (relay_binding) maybe_rollover_relay(relay_binding);
        } else if (r == 0){
            break;
        } else if (r < 0 && errno == EINTR){
            continue;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)){
            break;
        } else {
            u->send_errs++;
            break;
        }
    }
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
    for (int ui=0; ui<MAX_UARTS; ui++){
        UARTS[ui].relay = NULL;
    }
    for (int i=0;i<REL_N;i++){
        if (REL[i].kind == RELAY_KIND_UDP && REL[i].fd>=0){
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
        char *lhs = trim(line);
        char *list = NULL;
        if (sep){ *sep = 0; list = trim(sep+1); }

        int uart_idx = uart_token_parse(lhs);
        struct relay *r=&REL[REL_N];
        memset(r,0,sizeof(*r));

        if (uart_idx >= 0){
            if (uart_idx >= MAX_UARTS){
                fprintf(stderr,"UART index out of range in bind: %s\n", c->bind_lines[i]);
                continue;
            }
            if (UARTS[uart_idx].relay){
                fprintf(stderr,"Duplicate bind for UART index %d ignored\n", uart_idx);
                continue;
            }
            r->kind = RELAY_KIND_UART;
            r->uart_idx = uart_idx;
            r->fd = -1;
            if (list && *list){
                if (parse_dest_list(r->dests, &r->dest_cnt, MAX_DESTS, list, true, false)<0){
                    fprintf(stderr,"Bad UART dest list for uart%d, starting empty\n", uart_idx);
                    r->dest_cnt = 0;
                }
            }
            UARTS[uart_idx].relay = r;
            char token[16]; uart_token_format(uart_idx, token, sizeof(token));
            fprintf(stderr,"Bound %s fan-out (dests=%d)\n", token, r->dest_cnt);
            REL_N++;
            continue;
        }

        int sport=parse_int_bounded(lhs,1,65535);
        if (sport<0){ fprintf(stderr,"Bad bind line: %s\n", c->bind_lines[i]); continue; }

        r->kind = RELAY_KIND_UDP;
        r->uart_idx = -1;
        r->src_port=sport;
        r->fd=make_udp_socket(c->src_ip, sport, c->rcvbuf, c->sndbuf, c->tos);
        if (r->fd<0){ fprintf(stderr,"Bind failed %d\n", sport); continue; }

        struct epoll_event ev={.events=EPOLLIN, .data.fd=r->fd};
        if (epoll_ctl(EPFD, EPOLL_CTL_ADD, r->fd, &ev)<0){ perror("epoll_ctl add udp"); close(r->fd); continue; }

        if (list && *list){
            if (parse_dest_list(r->dests, &r->dest_cnt, MAX_DESTS, list, true, true)<0){
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

static struct relay* relay_find_by_port(int port){
    if (port <= 0) return NULL;
    for (int i=0;i<REL_N;i++){
        if (REL[i].kind == RELAY_KIND_UDP && REL[i].src_port == port){
            return &REL[i];
        }
    }
    return NULL;
}

static struct relay* relay_find_by_uart_idx(int idx){
    if (idx < 0 || idx >= MAX_UARTS) return NULL;
    for (int i=0;i<REL_N;i++){
        if (REL[i].kind == RELAY_KIND_UART && REL[i].uart_idx == idx){
            return &REL[i];
        }
    }
    return NULL;
}

static struct relay* relay_find_by_id(const char *id){
    if (!id) return NULL;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s", id);
    char *name = trim(buf);
    if (!*name) return NULL;
    int uart_idx = uart_token_parse(name);
    if (uart_idx >= 0){
        return relay_find_by_uart_idx(uart_idx);
    }
    int port = parse_int_bounded(name, 1, 65535);
    if (port > 0){
        return relay_find_by_port(port);
    }
    return NULL;
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
        if (!HC[i].buf){
            HC[i].fd=0; HC[i].cap=HC[i].len=HC[i].need=HC[i].have_hdr=0;
            return NULL;
        }
        HC[i].buf[0]='\0';
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

static int json_extract_string(const char *body, const char *key, char *out, size_t outlen){
    if (!body || !key || !out || outlen == 0) return -1;
    const char *p = strstr(body, key);
    if (!p) return -1;
    const char *col = strchr(p, ':');
    if (!col) return -1;
    const char *q1 = strchr(col, '"');
    if (!q1) return -1;
    q1++;
    const char *q2 = strchr(q1, '"');
    if (!q2) return -1;
    size_t n = (size_t)(q2 - q1);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, q1, n);
    out[n] = '\0';
    return 0;
}

static struct relay* json_find_relay(const char *body){
    char idbuf[32];
    if (json_extract_string(body, "\"id\"", idbuf, sizeof(idbuf))==0){
        return relay_find_by_id(idbuf);
    }
    int port = json_extract_port(body);
    if (port > 0){
        return relay_find_by_port(port);
    }
    if (json_extract_string(body, "\"token\"", idbuf, sizeof(idbuf))==0){
        return relay_find_by_id(idbuf);
    }
    return NULL;
}

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
    if (!c){
        int idx = uart_token_parse(token);
        if (idx >= 0){
            char canon[16];
            uart_token_format(idx, canon, sizeof(canon));
            snprintf(ip, iplen, "%s", canon);
            *port = 0;
            return 0;
        }
        return -1;
    }
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
static int apply_set_like(struct relay *r, const char *body, bool replace){
    if (!r) return -2;

    /* Extract array slice of dests */
    const char *key="\"dests\"";
    const char *k=strstr(body,key); if(!k) return -3;
    const char *lb=strchr(k,'['); if(!lb) return -3;
    const char *rb=strchr(lb,']'); if(!rb) return -3;
    size_t n=(size_t)(rb - (lb+1));
    char *arr=malloc(n+1); if(!arr) return -3;
    memcpy(arr, lb+1, n); arr[n]=0;

    struct dest tmp[MAX_DESTS];
    int tmp_cnt = 0;
    char *s=arr;
    while (*s){
        while (*s && (isspace((unsigned char)*s) || *s==',')) s++;
        if (!*s) break;
        if (*s=='"'){
            s++; char *e=strchr(s,'"'); if(!e) break;
            *e=0;
            bool allow_uart = (r->kind == RELAY_KIND_UDP);
            if (parse_dest_token(tmp, &tmp_cnt, MAX_DESTS, s, allow_uart)<0){ free(arr); return -4; }
            s=e+1;
        } else {
            char *e=s; while(*e && *e!=',') e++;
            char sv=*e; *e=0;
            if (strlen(s)){
                bool allow_uart = (r->kind == RELAY_KIND_UDP);
                if(parse_dest_token(tmp,&tmp_cnt,MAX_DESTS,trim(s),allow_uart)<0){ *e=sv; free(arr); return -4; }
            }
            *e=sv; s=e;
        }
    }
    free(arr);

    if (replace){
        r->dest_cnt=0; /* stats preserved */
    }
    for (int i=0;i<tmp_cnt && r->dest_cnt<MAX_DESTS;i++)
        r->dests[r->dest_cnt++]=tmp[i];
    return 0;
}

/* append_range: {"port":5801,"ip":"1.2.3.4","start":7000,"end":7005} (ip optional) */
static int apply_append_range(struct relay *r, const char *body){
    if (!r || r->kind != RELAY_KIND_UDP) return -1;
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
    for (int p=start; p<=end && r->dest_cnt<MAX_DESTS; p++){
        if (add_dest_udp(r->dests, &r->dest_cnt, MAX_DESTS, ip, p)<0) break;
    }
    return 0;
}

/* Remove one destination from one relay (atomic) */
static int apply_clear_to(struct relay *r, const char *body){
    char ip[64]={0}; int dport=-1;
    if (!r) return -1;

    /* Accept either {"dest":"ip:port"} or {"ip":"..","port":..} */
    if (json_extract_dest_token(body, ip, sizeof(ip), &dport)!=0){
        if (json_extract_ip_port(body, ip, sizeof(ip), &dport)!=0) return -1;
    }

    int idx=-1;
    int uart_idx = uart_token_parse(ip);
    if (uart_idx >= 0){
        for (int j=0;j<r->dest_cnt;j++){
            if (r->dests[j].type == DEST_UART && r->dests[j].uart_idx == uart_idx){ idx=j; break; }
        }
    } else {
        struct sockaddr_in target={0};
        target.sin_family=AF_INET;
        target.sin_port=htons(dport);
        if (inet_pton(AF_INET, ip, &target.sin_addr)!=1) return -1;
        for (int j=0;j<r->dest_cnt;j++){
            if (r->dests[j].type == DEST_UDP && sockaddr_equal(&r->dests[j].addr, &target)){ idx=j; break; }
        }
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

    APPEND("HTTP/1.0 200 OK\r\n"
           "Content-Type: application/json\r\n"
           "Connection: close\r\n"
           "\r\n");
    APPEND("{\"relays\":[");
    for (int i=0;i<REL_N;i++){
        if (i) APPEND(",");
        struct relay *r=&REL[i];
        char idbuf[32]; relay_id_format(r, idbuf, sizeof(idbuf));
        uint64_t pkts_out_total=0;
        for (int j=0;j<r->dest_cnt;j++) pkts_out_total += r->dests[j].pkts_out;
        if (r->kind == RELAY_KIND_UART){
            struct uart_runtime *u = (r->uart_idx>=0 && r->uart_idx<MAX_UARTS) ? &UARTS[r->uart_idx] : NULL;
            uint64_t pkts_in = u ? u->pkts_in : 0;
            uint64_t bytes_in = u ? u->bytes_in : 0;
            uint64_t bytes_out = u ? u->bytes_out : 0;
            uint64_t send_errs = u ? u->send_errs : 0;
            uint64_t drops = u ? u->drops : 0;
            uint64_t last_rx = u ? u->last_rx_ns : 0;
            int enabled = (u && u->enabled && u->fd >= 0);
            APPEND("{\"id\":\"%s\",\"kind\":\"uart\",\"token\":\"%s\",\"enabled\":%s,\"pkts_in\":%" PRIu64 ",\"bytes_in\":%" PRIu64 ",\"bytes_out\":%" PRIu64 ",\"send_errs\":%" PRIu64 ",\"drops\":%" PRIu64 ",\"last_rx_ns\":%" PRIu64 ",\"pkts_out_total\":%" PRIu64 ",\"dests\":[",
                   idbuf, idbuf, enabled?"true":"false", pkts_in, bytes_in, bytes_out, send_errs, drops, last_rx, pkts_out_total);
        } else {
            APPEND("{\"id\":\"%s\",\"kind\":\"udp\",\"port\":%d,\"pkts_in\":%" PRIu64 ",\"bytes_in\":%" PRIu64 ",\"bytes_out\":%" PRIu64 ",\"send_errs\":%" PRIu64 ",\"last_rx_ns\":%" PRIu64 ",\"rx_drops\":%" PRIu64 ",\"trunc_drops\":%" PRIu64 ",\"pkts_out_total\":%" PRIu64 ",\"dests\":[",
                   idbuf, r->src_port, r->pkts_in, r->bytes_in, r->bytes_out, r->send_errs, r->last_rx_ns, r->rx_drops, r->trunc_drops, pkts_out_total);
        }
        for (int j=0;j<r->dest_cnt;j++){
            if (j) APPEND(",");
            if (r->dests[j].type == DEST_UART){
                char token_buf[16]; uart_token_format(r->dests[j].uart_idx, token_buf, sizeof(token_buf));
                APPEND("{\"type\":\"uart\",\"token\":\"%s\",\"pkts\":%" PRIu64 "}", token_buf, r->dests[j].pkts_out);
            } else {
                char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&r->dests[j].addr.sin_addr,ip,sizeof(ip));
                APPEND("{\"type\":\"udp\",\"ip\":\"%s\",\"port\":%d,\"pkts\":%" PRIu64 "}", ip,
                       ntohs(r->dests[j].addr.sin_port), r->dests[j].pkts_out);
            }
        }
        APPEND("]}");
    }
    APPEND("],\"uarts\":[");
    for (int ui=0; ui<MAX_UARTS; ui++){
        if (ui) APPEND(",");
        struct uart_runtime *u = &UARTS[ui];
        char token[16]; uart_token_format(ui, token, sizeof(token));
        struct relay *relay = relay_find_by_uart_idx(ui);
        if (u->enabled && u->fd >= 0){
            APPEND("{\"token\":\"%s\",\"enabled\":true,\"device\":\"%s\",\"baud\":%d,\"pkts_in\":%" PRIu64 ",\"bytes_in\":%" PRIu64 ",\"pkts_out\":%" PRIu64 ",\"bytes_out\":%" PRIu64 ",\"send_errs\":%" PRIu64 ",\"drops\":%" PRIu64 ",\"last_rx_ns\":%" PRIu64 ",\"dests\":[",
                   token, u->cfg.device, u->cfg.baud, u->pkts_in, u->bytes_in, u->pkts_out, u->bytes_out,
                   u->send_errs, u->drops, u->last_rx_ns);
            int first = 1;
            if (relay){
                for (int j=0;j<relay->dest_cnt;j++){
                    if (relay->dests[j].type != DEST_UDP) continue;
                    if (!first) APPEND(",");
                    first = 0;
                    char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&relay->dests[j].addr.sin_addr,ip,sizeof(ip));
                    APPEND("{\"ip\":\"%s\",\"port\":%d,\"pkts\":%" PRIu64 "}", ip,
                           ntohs(relay->dests[j].addr.sin_port), relay->dests[j].pkts_out);
                }
            }
            APPEND("]}");
        } else {
            APPEND("{\"token\":\"%s\",\"enabled\":false,\"dests\":[", token);
            int first = 1;
            if (relay){
                for (int j=0;j<relay->dest_cnt;j++){
                    if (relay->dests[j].type != DEST_UDP) continue;
                    if (!first) APPEND(",");
                    first = 0;
                    char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&relay->dests[j].addr.sin_addr,ip,sizeof(ip));
                    APPEND("{\"ip\":\"%s\",\"port\":%d,\"pkts\":%" PRIu64 "}", ip,
                           ntohs(relay->dests[j].addr.sin_port), relay->dests[j].pkts_out);
                }
            }
            APPEND("]}");
        }
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
    struct config newc;
    char *dup = strndup(body, len);
    if (!dup){
        http_send(fd,"HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\noom\n");
        return;
    }
    if (load_ini_text(dup, &newc)!=0){
        free(dup);
        http_send(fd,"HTTP/1.0 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nbad ini\n");
        return;
    }
    free(dup);

    if (newc.bufsz <= 0) newc.bufsz = 9000;

    struct config oldc = G;
    int need_http = strcmp(newc.http_bind, oldc.http_bind) || newc.control_port != oldc.control_port;
    int new_http_fd = -1;

    if (need_http){
        new_http_fd = http_listen(newc.http_bind, newc.control_port);
        if (new_http_fd < 0){
            http_send(fd,"HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nhttp listen failed\n");
            return;
        }
    }

    char *new_udp = malloc((size_t)newc.bufsz);
    if (!new_udp){
        if (new_http_fd >= 0) close(new_http_fd);
        http_send(fd,"HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\noom\n");
        return;
    }

    if (apply_config_relays(&newc)!=0){
        if (apply_config_relays(&oldc)!=0){
            fprintf(stderr,"Failed to restore previous relay config after error\n");
        }
        free(new_udp);
        if (new_http_fd >= 0) close(new_http_fd);
        http_send(fd,"HTTP/1.0 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nno valid binds\n");
        return;
    }

    if (uart_apply_config_all(&newc)!=0){
        if (apply_config_relays(&oldc)!=0){
            fprintf(stderr,"Failed to restore previous relay config after UART error\n");
        }
        if (uart_apply_config_all(&oldc)!=0){
            fprintf(stderr,"Failed to restore previous UART config after error\n");
        }
        free(new_udp);
        if (new_http_fd >= 0) close(new_http_fd);
        http_send(fd,"HTTP/1.0 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nuart setup failed\n");
        return;
    }

    if (need_http){
        struct epoll_event ev={.events=EPOLLIN, .data.fd=new_http_fd};
        if (epoll_ctl(EPFD, EPOLL_CTL_ADD, new_http_fd, &ev)<0){
            perror("epoll_ctl add http");
            if (apply_config_relays(&oldc)!=0){
                fprintf(stderr,"Failed to restore previous relay config after error\n");
            }
            free(new_udp);
            close(new_http_fd);
            http_send(fd,"HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nhttp listen failed\n");
            return;
        }
        if (HTTP_LFD>=0){
            epoll_ctl(EPFD, EPOLL_CTL_DEL, HTTP_LFD, NULL);
            close(HTTP_LFD);
        }
        HTTP_LFD = new_http_fd;
        new_http_fd = -1;
    }

    char *old_udp = UDP_BUF;
    UDP_BUF = new_udp;
    new_udp = NULL;

    G = newc;

    if (save_file_atomic(CFG_TMP_PATH, CFG_PATH, body, len)!=0){
        http_send(fd,"HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\npersist failed\n");
        reload_from_disk();
        if (old_udp) free(old_udp);
        if (new_http_fd >= 0) close(new_http_fd);
        return;
    }

    if (old_udp) free(old_udp);
    http_send(fd,"HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"ok\":true}\n");
}

static void http_handle_action(int fd, const char *verb, const char *body){
    int rc=-1;
    struct relay *r = NULL;
    if (!strcmp(verb,"set")){
        r = json_find_relay(body);
        rc=apply_set_like(r, body, true);
    } else if (!strcmp(verb,"append")){
        r = json_find_relay(body);
        rc=apply_set_like(r, body, false);
    } else if (!strcmp(verb,"append_range")){
        r = json_find_relay(body);
        rc=apply_append_range(r, body);
    } else if (!strcmp(verb,"clear")){
        r = json_find_relay(body);
        if (r){ r->dest_cnt=0; rc=0; }
    } else if (!strcmp(verb,"reset")){
        r = json_find_relay(body);
        if (r){
            r->pkts_in=r->bytes_in=r->bytes_out=r->send_errs=0;
            for(int j=0;j<r->dest_cnt;j++) r->dests[j].pkts_out=0;
            if (r->kind == RELAY_KIND_UART && r->uart_idx >=0 && r->uart_idx < MAX_UARTS){
                struct uart_runtime *u = &UARTS[r->uart_idx];
                u->pkts_in = u->bytes_in = u->pkts_out = u->bytes_out = 0;
                u->send_errs = u->drops = 0;
            }
            rc=0;
        }
    } else if (!strcmp(verb,"clear_to")){
        r = json_find_relay(body);
        rc=apply_clear_to(r, body);
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
            if (hc->len + (size_t)r >= HTTP_BUF_MAX){ hc_del(fd); break; }
            if (hc->len + (size_t)r >= hc->cap){
                size_t need = hc->len + (size_t)r + 1;
                size_t ncap = hc->cap*2; if (ncap < need) ncap = need;
                if (ncap>HTTP_BUF_MAX) ncap=HTTP_BUF_MAX;
                char *nb=realloc(hc->buf,ncap); if(!nb){ hc_del(fd); break; }
                hc->buf=nb; hc->cap=ncap;
            }
            memcpy(hc->buf+hc->len, tmp, (size_t)r);
            hc->len += (size_t)r;
            if (hc->len < hc->cap) hc->buf[hc->len]='\0';

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

    if (uart_apply_config_all(&G)!=0){
        fprintf(stderr,"Reload warning: UART setup failed\n");
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

    uart_runtime_init_all();

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

    if (uart_apply_config_all(&G)!=0){
        fprintf(stderr,"UART setup failed\n");
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

            int handled_uart = 0;
            for (int ui=0; ui<MAX_UARTS; ui++){
                struct uart_runtime *u = &UARTS[ui];
                if (!u->enabled || u->fd < 0) continue;
                if (fd == u->fd){
                    handled_uart = 1;
                    if (evs & (EPOLLHUP|EPOLLERR)){
                        fprintf(stderr,"UART[%s] connection closed\n", u->token[0]?u->token:"uart");
                        uart_close(u);
                    } else {
                        if (evs & EPOLLIN) uart_handle_read(u);
                        if (evs & EPOLLOUT) uart_handle_write(u);
                    }
                    break;
                }
            }
            if (handled_uart) continue;

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
                        struct dest *dest_refs[MAX_DESTS];
                        memcpy(snap, r->dests, (size_t)cnt*sizeof(struct dest));
                        for (int d=0; d<cnt; d++){
                            dest_refs[d] = &r->dests[d];
                        }

                        struct mmsghdr msgs[MAX_DESTS];
                        struct iovec   siov[MAX_DESTS];
                        struct dest   *udp_refs[MAX_DESTS];
                        int uart_seen[MAX_UARTS] = {0};
                        struct dest *uart_dest_for_idx[MAX_UARTS] = {0};
                        memset(msgs,0,sizeof(msgs));

                        int udp_cnt = 0;
                        for (int d=0; d<cnt; d++){
                            dest_refs[d] = &r->dests[d];
                            if (snap[d].type == DEST_UART){
                                int ui = snap[d].uart_idx;
                                if (ui >= 0 && ui < MAX_UARTS){
                                    uart_seen[ui] = 1;
                                    uart_dest_for_idx[ui] = dest_refs[d];
                                }
                                continue;
                            }
                            if (udp_cnt >= MAX_DESTS) continue;
                            udp_refs[udp_cnt] = dest_refs[d];
                            siov[udp_cnt].iov_base = UDP_BUF;
                            siov[udp_cnt].iov_len  = (size_t)m;
                            msgs[udp_cnt].msg_hdr.msg_iov = &siov[udp_cnt];
                            msgs[udp_cnt].msg_hdr.msg_iovlen = 1;
                            msgs[udp_cnt].msg_hdr.msg_name = &snap[d].addr;
                            msgs[udp_cnt].msg_hdr.msg_namelen = sizeof(snap[d].addr);
                            udp_cnt++;
                        }

                        if (udp_cnt > 0){
                            int sent_total = 0;
                            while (sent_total < udp_cnt){
                                int rc = sendmmsg(fd, msgs + sent_total, (unsigned)(udp_cnt - sent_total), 0);
                                if (rc > 0){
                                    for (int j=0; j<rc; j++){
                                        r->bytes_out += (uint64_t)m;
                                        struct dest *dd = udp_refs[sent_total + j];
                                        if (dd){
                                            dd->pkts_out++;
                                        }
                                    }
                                    sent_total += rc;
                                } else if (rc < 0 && (errno==EAGAIN || errno==EWOULDBLOCK)){
                                    r->send_errs += (uint64_t)(udp_cnt - sent_total);
                                    break;
                                } else {
                                    r->send_errs += (uint64_t)(udp_cnt - sent_total);
                                    break;
                                }
                            }
                        }

                        for (int ui=0; ui<MAX_UARTS; ui++){
                            if (!uart_seen[ui]) continue;
                            struct uart_runtime *u = &UARTS[ui];
                            if (uart_send_from_udp(u, (const uint8_t*)UDP_BUF, (size_t)m)==0){
                                r->bytes_out += (uint64_t)m;
                                if (uart_dest_for_idx[ui]) uart_dest_for_idx[ui]->pkts_out++;
                            } else {
                                r->send_errs++;
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
    for (int ui=0; ui<MAX_UARTS; ui++) uart_close(&UARTS[ui]);
    if (UDP_BUF) free(UDP_BUF);
    if (UI_BUF) free(UI_BUF);
    if (EPFD>=0) close(EPFD);
    return 0;
}
