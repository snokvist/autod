// ip2uart.c — TTY/STDIO <-> TCP/UDP bridge for embedded Linux
// - UART side selectable: uart_backend=tty | stdio
// - Network side selectable: net_mode=tcp_server | tcp_client | udp_peer
// - UDP: static peer (fire-and-forget), accepts from any sender
// - UDP coalescing: size threshold + short idle before send (reason logged)
// - TCP: one client max on server; client mode auto-reconnect; tcp_nodelay option
// - Short-write safe: ring buffers for both directions (non-blocking)
// - SIGHUP: reload /etc/eth2uart.conf and reopen (SO_REUSEADDR/PORT) — logged
// - Periodic INI stats snapshot to log_file (status_interval_ms); config not echoed
// - Verbose logging: -v / -vv / -vvv (or -vvvv...) to stderr with timestamps
// SPDX-License-Identifier: MIT

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#if defined(__linux__)
#include <asm/ioctls.h>
#define termios asm_termios
#include <asm/termbits.h>
#undef termios
#endif
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONF "/etc/ip2uart.conf"
#define MAX_LINE     512
#define MAX_KEY      64
#define MAX_VAL      256
#define MAX_EVENTS   18
#define CADENCE_WINDOW 32

typedef enum { UART_TTY, UART_STDIO } uart_backend_t;
typedef enum { NET_TCP_SERVER, NET_TCP_CLIENT, NET_UDP_PEER } net_mode_t;

/* ----------------------------- Verbose logging ------------------------------ */
static int g_verbosity = 0;

static long long ts_ms_now(void){
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    return (long long)rt.tv_sec*1000LL + rt.tv_nsec/1000000LL;
}
static void vlog(int level, const char *fmt, ...){
    if (g_verbosity < level) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%lld] ", ts_ms_now());
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ----------------- Small ring buffer (non-blocking IO helper) ----------------- */
typedef struct { uint8_t *buf; size_t cap, head, tail, len; } ringbuf_t;
static int    ring_init (ringbuf_t *r, size_t cap){
    r->buf=NULL; r->cap=r->head=r->tail=r->len=0;
    if(!cap) return 0;
    r->buf=(uint8_t*)malloc(cap);
    if(!r->buf){ errno=ENOMEM; return -1; }
    r->cap=cap;
    return 0;
}
static void   ring_free (ringbuf_t *r){ free(r->buf); r->buf=NULL; r->cap=r->head=r->tail=r->len=0; }
static size_t ring_space(const ringbuf_t *r){ return r->cap - r->len; }
static size_t ring_write(ringbuf_t *r, const uint8_t *src, size_t n){
    if (!r->cap || !n) return 0;
    size_t w = n>ring_space(r)?ring_space(r):n;
    size_t first = w>(r->cap-r->head) ? (r->cap-r->head) : w;
    if (first) memcpy(r->buf + r->head, src, first);
    size_t second = w - first;
    if (second) memcpy(r->buf, src + first, second);
    r->head = (r->head + w) % r->cap; r->len += w;
    return w;
}
static size_t ring_peek(const ringbuf_t *r, const uint8_t **p1, size_t *l1, const uint8_t **p2, size_t *l2){
    if (!r->len){ *p1=*p2=NULL; *l1=*l2=0; return 0; }
    size_t first = r->len>(r->cap-r->tail) ? (r->cap-r->tail) : r->len;
    *p1 = r->buf + r->tail; *l1 = first; *p2 = NULL; *l2 = 0;
    if (r->len > first){ *p2 = r->buf; *l2 = r->len - first; }
    return r->len;
}
static void ring_consume(ringbuf_t *r, size_t n){ if (n>r->len) n=r->len; r->tail=(r->tail+n)%r->cap; r->len-=n; }

/* --------------------------------- Config ----------------------------------- */
typedef struct {
    // Selectors
    uart_backend_t uart_backend;   // tty | stdio
    net_mode_t     net_mode;       // tcp_server | tcp_client | udp_peer

    // UART
    char uart_device[128];
    int  uart_baud;
    int  uart_databits;    // 5..8
    char uart_parity[8];   // none|even|odd
    int  uart_stopbits;    // 1|2
    char uart_flow[16];    // none|rtscts

    // TCP server
    char listen_addr[64];
    int  listen_port;
    int  tcp_listen_backlog;

    // TCP client
    char remote_host[128];
    int  remote_port;
    int  reconnect_delay_ms;
    int  tcp_nodelay;      // 1=disable Nagle, 0=allow

    // UDP static
    char udp_bind_addr[64];
    int  udp_bind_port;
    char udp_peer_addr[64];
    int  udp_peer_port;

    // UDP coalescing
    int  udp_coalesce_bytes;   // send immediately if >=
    int  udp_coalesce_idle_ms; // or if idle for this long
    int  udp_max_datagram;     // max size of one datagram

    // Logging / status
    char log_file[128];
    int  dump_on_start;       // 1/0
    int  status_interval_ms;  // 0 = disabled
    double expected_hz;       // optional cadence target

    // Buffers
    size_t rx_buf; // scratch RX buffers
    size_t tx_buf; // ring buffer capacity
} config_t;

/* --------------------------------- State ------------------------------------ */
typedef struct {
    // fds
    int fd_uart;     // UART or STDIN
    int fd_stdout;   // only used when uart_backend=STDIO, else -1
    int fd_listen;
    int fd_net;      // TCP conn or UDP socket
    int epfd;

    bool stdout_registered;

    // UDP peer (static outbound)
    struct sockaddr_in udp_peer;
    bool udp_peer_set;

    // UDP coalesce buffer
    uint8_t *udp_out;
    size_t   udp_out_len;
    size_t   udp_out_cap;

    // stats
    uint64_t bytes_uart_to_net, bytes_net_to_uart;
    uint64_t pkts_uart_to_net,  pkts_net_to_uart;
    uint64_t connects, disconnects;
    uint64_t drops_uart_to_net, drops_net_to_uart;

    // current TCP peer (if connected)
    char tcp_peer_ip[128];
    int  tcp_peer_port;
    bool tcp_connected;

    // timers
    struct timespec last_status;
    struct timespec last_uart_rx; // for UDP idle
    struct timespec next_reconnect;

    // rings for short-write safety
    ringbuf_t tcp_out;   // UART/STDIN -> TCP pending bytes
    ringbuf_t uart_out;  // NET -> UART/STDOUT pending bytes

    bool running;
    bool reconnect_pending;

    struct timespec last_pkt_uart_to_net;
    struct timespec last_pkt_net_to_uart;
    uint64_t intervals_uart_to_net[CADENCE_WINDOW];
    uint64_t intervals_net_to_uart[CADENCE_WINDOW];
    size_t intervals_uart_to_net_count;
    size_t intervals_net_to_uart_count;
    size_t intervals_uart_to_net_pos;
    size_t intervals_net_to_uart_pos;
    uint64_t last_log_pkts_uart_to_net;
    uint64_t last_log_pkts_net_to_uart;
} state_t;

/* ------------------------------- Signals ------------------------------------ */
static volatile sig_atomic_t g_reload = 0, g_stop = 0;
static void on_sighup(int sig){ (void)sig; g_reload = 1; }
static void on_sigterm(int sig){ (void)sig; g_stop = 1; }

/* ------------------------------- Utilities ---------------------------------- */
static int set_nonblock(int fd){ int fl=fcntl(fd,F_GETFL,0); if(fl<0) return -1; return fcntl(fd,F_SETFL,fl|O_NONBLOCK); }
static void trim(char *s){ if(!s) return; size_t n=strlen(s),i=0,j=n; while(i<n&&(s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n')) i++; while(j>i&&(s[j-1]==' '||s[j-1]=='\t'||s[j-1]=='\r'||s[j-1]=='\n')) j--; if(i>0) memmove(s,s+i,j-i); s[j-i]=0; }
static bool parse_bool(const char* v){ return !strcasecmp(v,"1")||!strcasecmp(v,"true")||!strcasecmp(v,"yes")||!strcasecmp(v,"on"); }
static speed_t baud_to_speed(int baud){
    switch(baud){
        case 9600: return B9600; case 19200: return B19200; case 38400: return B38400;
        case 57600: return B57600; case 115200: return B115200;
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
static void get_mono(struct timespec *ts){ clock_gettime(CLOCK_MONOTONIC, ts); }
static long long diff_ms(const struct timespec *a, const struct timespec *b){ return (a->tv_sec-b->tv_sec)*1000LL + (a->tv_nsec-b->tv_nsec)/1000000LL; }
static uint64_t diff_us(const struct timespec *a, const struct timespec *b){
    long long sec = (long long)a->tv_sec - (long long)b->tv_sec;
    long long nsec = (long long)a->tv_nsec - (long long)b->tv_nsec;
    long long total = sec*1000000LL + nsec/1000LL;
    if(total < 0) total = 0;
    return (uint64_t)total;
}

static void reset_cadence_stats(state_t *st){
    memset(&st->last_pkt_uart_to_net, 0, sizeof(st->last_pkt_uart_to_net));
    memset(&st->last_pkt_net_to_uart, 0, sizeof(st->last_pkt_net_to_uart));
    memset(st->intervals_uart_to_net, 0, sizeof(st->intervals_uart_to_net));
    memset(st->intervals_net_to_uart, 0, sizeof(st->intervals_net_to_uart));
    st->intervals_uart_to_net_count = 0;
    st->intervals_net_to_uart_count = 0;
    st->intervals_uart_to_net_pos = 0;
    st->intervals_net_to_uart_pos = 0;
}

static void record_cadence_sample(struct timespec *last, uint64_t *buf, size_t *pos, size_t *count, const struct timespec *now){
    if(last->tv_sec || last->tv_nsec){
        uint64_t us = diff_us(now, last);
        buf[*pos] = us;
        if(*count < CADENCE_WINDOW) (*count)++;
        *pos = (*pos + 1) % CADENCE_WINDOW;
    }
    *last = *now;
}

static void cadence_note_packet(state_t *st, bool uart_to_net){
    struct timespec now;
    get_mono(&now);
    if(uart_to_net){
        record_cadence_sample(&st->last_pkt_uart_to_net, st->intervals_uart_to_net,
                              &st->intervals_uart_to_net_pos, &st->intervals_uart_to_net_count, &now);
    } else {
        record_cadence_sample(&st->last_pkt_net_to_uart, st->intervals_net_to_uart,
                              &st->intervals_net_to_uart_pos, &st->intervals_net_to_uart_count, &now);
    }
}

typedef struct {
    size_t samples;
    uint64_t min_us;
    uint64_t max_us;
    uint64_t p95_us;
    double avg_interval_us;
} cadence_stats_t;

static int cmp_u64(const void *a, const void *b){
    uint64_t ua = *(const uint64_t*)a;
    uint64_t ub = *(const uint64_t*)b;
    if(ua < ub) return -1;
    if(ua > ub) return 1;
    return 0;
}

static void cadence_collect_stats(const state_t *st, bool uart_to_net, cadence_stats_t *out){
    const uint64_t *buf = uart_to_net ? st->intervals_uart_to_net : st->intervals_net_to_uart;
    size_t count = uart_to_net ? st->intervals_uart_to_net_count : st->intervals_net_to_uart_count;
    size_t pos = uart_to_net ? st->intervals_uart_to_net_pos : st->intervals_net_to_uart_pos;
    out->samples = count;
    out->min_us = 0;
    out->max_us = 0;
    out->p95_us = 0;
    out->avg_interval_us = 0.0;
    if(!count) return;

    uint64_t scratch[CADENCE_WINDOW];
    size_t start = (count==CADENCE_WINDOW)?pos:0;
    for(size_t i=0;i<count;i++){
        size_t idx = (start + i) % CADENCE_WINDOW;
        scratch[i] = buf[idx];
    }

    uint64_t min = scratch[0];
    uint64_t max = scratch[0];
    long double sum = 0.0;
    for(size_t i=0;i<count;i++){
        uint64_t v = scratch[i];
        if(v < min) min = v;
        if(v > max) max = v;
        sum += (long double)v;
    }

    out->min_us = min;
    out->max_us = max;
    out->avg_interval_us = (double)(sum / (long double)count);

    uint64_t sorted[CADENCE_WINDOW];
    memcpy(sorted, scratch, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), cmp_u64);
    size_t idx = (count * 95 + 99) / 100;
    if(idx == 0) idx = 1;
    if(idx > count) idx = count;
    out->p95_us = sorted[idx-1];
}

/* --------------------------------- Config ----------------------------------- */
static int parse_config(const char *path, config_t *cfg){
    memset(cfg,0,sizeof(*cfg));
    // Defaults
    cfg->uart_backend = UART_TTY;
    cfg->net_mode     = NET_TCP_SERVER;

    strcpy(cfg->uart_device, "/dev/ttyS1");
    cfg->uart_baud=115200; cfg->uart_databits=8; strcpy(cfg->uart_parity,"none"); cfg->uart_stopbits=1; strcpy(cfg->uart_flow,"none");

    strcpy(cfg->listen_addr,"0.0.0.0"); cfg->listen_port=5760; cfg->tcp_listen_backlog=8;
    strcpy(cfg->remote_host,"127.0.0.1"); cfg->remote_port=5760; cfg->reconnect_delay_ms=1000; cfg->tcp_nodelay=1;

    strcpy(cfg->udp_bind_addr,"0.0.0.0"); cfg->udp_bind_port=14550; cfg->udp_peer_addr[0]=0; cfg->udp_peer_port=14550;
    cfg->udp_coalesce_bytes=1200; cfg->udp_coalesce_idle_ms=5; cfg->udp_max_datagram=1200;

    strcpy(cfg->log_file,"/tmp/ip2uart.log"); cfg->dump_on_start=1; cfg->status_interval_ms=0;
    cfg->expected_hz=0.0;
    cfg->rx_buf=65536; cfg->tx_buf=65536;

    FILE *f=fopen(path,"r"); if(!f) return -1;
    char line[MAX_LINE];
    while(fgets(line,sizeof(line),f)){
        if(line[0]=='#'||line[0]==';') continue;
        char *eq=strchr(line,'='); if(!eq) continue; *eq=0;
        char key[MAX_KEY], val[MAX_VAL];
        strncpy(key,line,sizeof(key)-1); key[sizeof(key)-1]=0;
        strncpy(val,eq+1,sizeof(val)-1); val[sizeof(val)-1]=0;
        trim(key); trim(val); if(!*key) continue;

        // selectors
        if(!strcmp(key,"uart_backend")){
            if(!strcmp(val,"tty")) cfg->uart_backend=UART_TTY;
            else if(!strcmp(val,"stdio")) cfg->uart_backend=UART_STDIO;
        } else if(!strcmp(key,"net_mode")){
            if(!strcmp(val,"tcp_server")) cfg->net_mode=NET_TCP_SERVER;
            else if(!strcmp(val,"tcp_client")) cfg->net_mode=NET_TCP_CLIENT;
            else if(!strcmp(val,"udp_peer"))   cfg->net_mode=NET_UDP_PEER;
        }
        // uart
        else if(!strcmp(key,"uart_device")){ strncpy(cfg->uart_device,val,sizeof(cfg->uart_device)-1); cfg->uart_device[sizeof(cfg->uart_device)-1]=0; }
        else if(!strcmp(key,"uart_baud")) cfg->uart_baud=atoi(val);
        else if(!strcmp(key,"uart_databits")) cfg->uart_databits=atoi(val);
        else if(!strcmp(key,"uart_parity")){ strncpy(cfg->uart_parity,val,sizeof(cfg->uart_parity)-1); cfg->uart_parity[sizeof(cfg->uart_parity)-1]=0; }
        else if(!strcmp(key,"uart_stopbits")) cfg->uart_stopbits=atoi(val);
        else if(!strcmp(key,"uart_flow")){ strncpy(cfg->uart_flow,val,sizeof(cfg->uart_flow)-1); cfg->uart_flow[sizeof(cfg->uart_flow)-1]=0; }

        // tcp
        else if(!strcmp(key,"listen_addr")){ strncpy(cfg->listen_addr,val,sizeof(cfg->listen_addr)-1); cfg->listen_addr[sizeof(cfg->listen_addr)-1]=0; }
        else if(!strcmp(key,"listen_port")) cfg->listen_port=atoi(val);
        else if(!strcmp(key,"tcp_listen_backlog")) cfg->tcp_listen_backlog=atoi(val);
        else if(!strcmp(key,"remote_host")){ strncpy(cfg->remote_host,val,sizeof(cfg->remote_host)-1); cfg->remote_host[sizeof(cfg->remote_host)-1]=0; }
        else if(!strcmp(key,"remote_port")) cfg->remote_port=atoi(val);
        else if(!strcmp(key,"reconnect_delay_ms")) cfg->reconnect_delay_ms=atoi(val);
        else if(!strcmp(key,"tcp_nodelay")) cfg->tcp_nodelay=parse_bool(val)?1:0;

        // udp
        else if(!strcmp(key,"udp_bind_addr")){ strncpy(cfg->udp_bind_addr,val,sizeof(cfg->udp_bind_addr)-1); cfg->udp_bind_addr[sizeof(cfg->udp_bind_addr)-1]=0; }
        else if(!strcmp(key,"udp_bind_port")) cfg->udp_bind_port=atoi(val);
        else if(!strcmp(key,"udp_peer_addr")){ strncpy(cfg->udp_peer_addr,val,sizeof(cfg->udp_peer_addr)-1); cfg->udp_peer_addr[sizeof(cfg->udp_peer_addr)-1]=0; }
        else if(!strcmp(key,"udp_peer_port")) cfg->udp_peer_port=atoi(val);
        else if(!strcmp(key,"udp_coalesce_bytes")) cfg->udp_coalesce_bytes=atoi(val);
        else if(!strcmp(key,"udp_coalesce_idle_ms")) cfg->udp_coalesce_idle_ms=atoi(val);
        else if(!strcmp(key,"udp_max_datagram")) cfg->udp_max_datagram=atoi(val);

        // logging/buffers
        else if(!strcmp(key,"log_file")){ strncpy(cfg->log_file,val,sizeof(cfg->log_file)-1); cfg->log_file[sizeof(cfg->log_file)-1]=0; }
        else if(!strcmp(key,"dump_on_start")) cfg->dump_on_start=parse_bool(val);
        else if(!strcmp(key,"status_interval_ms")) cfg->status_interval_ms=atoi(val);
        else if(!strcmp(key,"expected_hz")) cfg->expected_hz=strtod(val,NULL);
        else if(!strcmp(key,"rx_buf")) cfg->rx_buf=(size_t)strtoul(val,NULL,10);
        else if(!strcmp(key,"tx_buf")) cfg->tx_buf=(size_t)strtoul(val,NULL,10);
    }
    fclose(f);

    // sanitize UDP coalesce
    if (cfg->udp_max_datagram <= 0) cfg->udp_max_datagram = 1200;
    if (cfg->udp_coalesce_bytes <= 0 || cfg->udp_coalesce_bytes > cfg->udp_max_datagram)
        cfg->udp_coalesce_bytes = cfg->udp_max_datagram;
    if (cfg->udp_coalesce_idle_ms < 0) cfg->udp_coalesce_idle_ms = 0;
    if (cfg->tcp_listen_backlog <= 0) cfg->tcp_listen_backlog = 1;

    return 0;
}

/* --------------------------------- UART ------------------------------------- */
static int open_uart(const config_t *cfg){
    int fd = open(cfg->uart_device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) { close(fd); return -1; }
    cfmakeraw(&tio);

    speed_t sp = baud_to_speed(cfg->uart_baud);
    if (sp){
        cfsetispeed(&tio, sp);
        cfsetospeed(&tio, sp);
    } else {
        cfsetispeed(&tio, B38400);
        cfsetospeed(&tio, B38400);
    }

    tio.c_cflag &= ~CSIZE;
    switch (cfg->uart_databits) { case 5: tio.c_cflag|=CS5; break; case 6: tio.c_cflag|=CS6; break;
        case 7: tio.c_cflag|=CS7; break; default: case 8: tio.c_cflag|=CS8; break; }
    if (!strcmp(cfg->uart_parity,"even")) { tio.c_cflag|=PARENB; tio.c_cflag&=~PARODD; }
    else if (!strcmp(cfg->uart_parity,"odd")) { tio.c_cflag|=PARENB; tio.c_cflag|=PARODD; }
    else { tio.c_cflag&=~PARENB; }
    if (cfg->uart_stopbits==2) tio.c_cflag|=CSTOPB; else tio.c_cflag&=~CSTOPB;
    if (!strcmp(cfg->uart_flow,"rtscts")) tio.c_cflag|=CRTSCTS; else tio.c_cflag&=~CRTSCTS;

    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN]=1; tio.c_cc[VTIME]=0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) { close(fd); return -1; }

    if (!sp){
        if (set_custom_baud(fd, cfg->uart_baud) < 0){
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }
    }
    return fd;
}

/* ------------------------------- Sockets ------------------------------------ */
static int make_tcp_server(const char *addr, int port, int backlog){
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
    int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one));
#endif
    struct sockaddr_in sa={0}; sa.sin_family=AF_INET; sa.sin_port=htons(port);
    if(inet_pton(AF_INET,addr,&sa.sin_addr)!=1){ close(fd); errno=EINVAL; return -1; }
    if(bind(fd,(struct sockaddr*)&sa,sizeof(sa))<0){ close(fd); return -1; }
    if(backlog<=0) backlog=1;
    if(listen(fd,backlog)<0){ close(fd); return -1; }
    set_nonblock(fd); return fd;
}
static int make_tcp_client_connect(const char *host, int port){
    struct addrinfo hints={0},*ai=NULL; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    char pbuf[16]; snprintf(pbuf,sizeof(pbuf),"%d",port);
    if(getaddrinfo(host,pbuf,&hints,&ai)!=0) return -1;
    int fd=-1; for(struct addrinfo *p=ai;p;p=p->ai_next){
        fd=socket(p->ai_family,p->ai_socktype,p->ai_protocol); if(fd<0) continue;
        set_nonblock(fd);
        if(connect(fd,p->ai_addr,p->ai_addrlen)<0 && errno!=EINPROGRESS){ close(fd); fd=-1; continue; }
        break;
    }
    freeaddrinfo(ai); return fd;
}
static int make_udp_bind(const char *addr, int port){
    int fd=socket(AF_INET,SOCK_DGRAM,0); if(fd<0) return -1;
    int one=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&one,sizeof(one));
#endif
    struct sockaddr_in sa={0}; sa.sin_family=AF_INET; sa.sin_port=htons(port);
    if(inet_pton(AF_INET,addr,&sa.sin_addr)!=1){ close(fd); errno=EINVAL; return -1; }
    if(bind(fd,(struct sockaddr*)&sa,sizeof(sa))<0){ close(fd); return -1; }
    set_nonblock(fd); return fd;
}
static int add_ep(int epfd,int fd,uint32_t ev){ struct epoll_event e={.events=ev,.data.fd=fd}; return epoll_ctl(epfd,EPOLL_CTL_ADD,fd,&e); }
static int mod_ep(int epfd,int fd,uint32_t ev){ struct epoll_event e={.events=ev,.data.fd=fd}; return epoll_ctl(epfd,EPOLL_CTL_MOD,fd,&e); }
static int del_ep(int epfd,int fd){ return epoll_ctl(epfd,EPOLL_CTL_DEL,fd,NULL); }
static void close_fd(int *fd){ if(*fd>=0){ close(*fd); *fd=-1; } }
static void timespec_add_ms(struct timespec *ts, int ms){
    ts->tv_sec += ms / 1000;
    long nsec = ts->tv_nsec + (long)(ms % 1000) * 1000000L;
    if (nsec >= 1000000000L){
        ts->tv_sec += 1;
        nsec -= 1000000000L;
    }
    ts->tv_nsec = nsec;
}

static void schedule_tcp_reconnect(const config_t *cfg, state_t *st, const char *reason){
    if(cfg->net_mode!=NET_TCP_CLIENT) return;
    int delay = cfg->reconnect_delay_ms>0?cfg->reconnect_delay_ms:500;
    struct timespec now; get_mono(&now);
    struct timespec target = now;
    timespec_add_ms(&target, delay);

    bool update=false;
    if(!st->reconnect_pending){
        st->next_reconnect = target;
        st->reconnect_pending = true;
        update=true;
    } else if(diff_ms(&st->next_reconnect, &target) > 0){
        st->next_reconnect = target;
        update=true;
    }

    if(update){
        long long wait = diff_ms(&st->next_reconnect, &now);
        if(wait<0) wait=0;
        vlog(2, "TCP client: %s, retry in %lldms", reason?reason:"retry scheduled", wait);
    } else if(reason){
        vlog(3, "TCP client: %s, retry already pending", reason);
    }
}

static void maybe_start_tcp_dial(const config_t *cfg, state_t *st){
    if(cfg->net_mode!=NET_TCP_CLIENT) return;
    if(st->fd_net>=0) return;
    if(!st->reconnect_pending) return;

    struct timespec now; get_mono(&now);
    if(diff_ms(&now,&st->next_reconnect) < 0) return;

    st->reconnect_pending=false;
    int fd = make_tcp_client_connect(cfg->remote_host,cfg->remote_port);
    if(fd<0){
        schedule_tcp_reconnect(cfg, st, "connect attempt failed");
        return;
    }

    st->fd_net = fd;
    add_ep(st->epfd, st->fd_net, EPOLLIN|EPOLLOUT);
    vlog(2, "TCP client: dialing %s:%d", cfg->remote_host, cfg->remote_port);
}

/* --------------------------- Short-write helpers ---------------------------- */
static ssize_t write_from_ring_fd(int fd, ringbuf_t *r){
    const uint8_t *p1,*p2; size_t l1,l2; ssize_t total=0;
    ring_peek(r,&p1,&l1,&p2,&l2);
    if(l1){ ssize_t w=write(fd,p1,l1); if(w>0){ ring_consume(r,(size_t)w); total+=w; } else return w; }
    if(r->len && l2){ ring_peek(r,&p1,&l1,&p2,&l2); if(l2){ ssize_t w=write(fd,p2,l2); if(w>0){ ring_consume(r,(size_t)w); total+=w; } else return (total>0?total:w); } }
    return total;
}
static ssize_t send_from_ring_tcp(int fd, ringbuf_t *r){
    const uint8_t *p1,*p2; size_t l1,l2; ssize_t total=0;
    ring_peek(r,&p1,&l1,&p2,&l2);
    if(l1){ ssize_t w=send(fd,p1,l1,MSG_NOSIGNAL); if(w>0){ ring_consume(r,(size_t)w); total+=w; } else return w; }
    if(r->len && l2){ ring_peek(r,&p1,&l1,&p2,&l2); if(l2){ ssize_t w=send(fd,p2,l2,MSG_NOSIGNAL); if(w>0){ ring_consume(r,(size_t)w); total+=w; } else return (total>0?total:w); } }
    return total;
}
static int apply_tcp_nodelay(int fd,int nodelay){ int v=nodelay?1:0; return setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&v,sizeof(v)); }

/* ------------------------ Open/close based on config ------------------------ */
static int reopen_everything(const config_t *cfg, state_t *st){
    vlog(2, "Reopen: closing existing fds");
    if (st->fd_net    >=0){ del_ep(st->epfd,st->fd_net);    close_fd(&st->fd_net); }
    if (st->fd_listen >=0){ del_ep(st->epfd,st->fd_listen); close_fd(&st->fd_listen); }
    if (cfg->uart_backend==UART_TTY && st->fd_uart>=0){ del_ep(st->epfd,st->fd_uart); close_fd(&st->fd_uart); }
    if (st->fd_stdout>=0 && st->stdout_registered){ del_ep(st->epfd,st->fd_stdout); st->stdout_registered=false; }
    st->fd_stdout=-1;

    st->tcp_connected=false; st->tcp_peer_ip[0]=0; st->tcp_peer_port=0; st->udp_peer_set=false;
    st->reconnect_pending=false; memset(&st->next_reconnect,0,sizeof(st->next_reconnect));
    st->udp_out_len=0;
    reset_cadence_stats(st);

    ring_free(&st->tcp_out); ring_free(&st->uart_out);
    if(ring_init(&st->tcp_out,cfg->tx_buf)<0 || ring_init(&st->uart_out,cfg->tx_buf)<0){
        vlog(1, "ring buffer allocation failed (%s)", strerror(errno));
        return -1;
    }

    // UART / STDIO
    if (cfg->uart_backend==UART_STDIO){
        st->fd_uart=STDIN_FILENO; st->fd_stdout=STDOUT_FILENO;
        set_nonblock(st->fd_uart); set_nonblock(st->fd_stdout);
        setvbuf(stdout,NULL,_IONBF,0);
        add_ep(st->epfd, st->fd_uart, EPOLLIN);
        vlog(1, "UART backend: stdio (stdin/stdout)");
    } else {
        st->fd_uart = open_uart(cfg);
        if(st->fd_uart<0) { vlog(1,"UART open failed (%s)", strerror(errno)); return -1; }
        set_nonblock(st->fd_uart); add_ep(st->epfd, st->fd_uart, EPOLLIN);
        vlog(1, "UART backend: tty dev=%s baud=%d %d%s%d flow=%s",
             cfg->uart_device, cfg->uart_baud, cfg->uart_databits,
             (!strcmp(cfg->uart_parity,"none")?"N":(!strcmp(cfg->uart_parity,"even")?"E":"O")),
             cfg->uart_stopbits, cfg->uart_flow);
    }

    // Network
    if (cfg->net_mode==NET_TCP_SERVER){
        st->fd_listen=make_tcp_server(cfg->listen_addr,cfg->listen_port,cfg->tcp_listen_backlog); if(st->fd_listen<0){ vlog(1,"TCP listen failed (%s)", strerror(errno)); return -1; }
        add_ep(st->epfd, st->fd_listen, EPOLLIN);
        vlog(1, "TCP server: listen %s:%d (backlog=%d)", cfg->listen_addr, cfg->listen_port, cfg->tcp_listen_backlog);
    } else if (cfg->net_mode==NET_TCP_CLIENT){
        st->fd_net=make_tcp_client_connect(cfg->remote_host,cfg->remote_port);
        if(st->fd_net<0){
            vlog(1,"TCP client connect setup failed, will retry (err=%d)", errno);
            schedule_tcp_reconnect(cfg, st, "initial connect failed");
        } else {
            add_ep(st->epfd, st->fd_net, EPOLLIN|EPOLLOUT);
            vlog(1, "TCP client: connect to %s:%d (nodelay=%d)", cfg->remote_host, cfg->remote_port, cfg->tcp_nodelay);
        }
    } else { // UDP
        st->fd_net=make_udp_bind(cfg->udp_bind_addr,cfg->udp_bind_port); if(st->fd_net<0){ vlog(1,"UDP bind failed (%s)", strerror(errno)); return -1; }
        add_ep(st->epfd, st->fd_net, EPOLLIN);
        if(cfg->udp_peer_addr[0]){ memset(&st->udp_peer,0,sizeof(st->udp_peer));
            st->udp_peer.sin_family=AF_INET; st->udp_peer.sin_port=htons(cfg->udp_peer_port);
            if(inet_pton(AF_INET,cfg->udp_peer_addr,&st->udp_peer.sin_addr)==1) st->udp_peer_set=true;
        }
        vlog(1, "UDP peer: bind %s:%d -> peer %s:%d (coalesce=%dB/%dms, max=%dB)",
             cfg->udp_bind_addr, cfg->udp_bind_port,
             cfg->udp_peer_addr[0]?cfg->udp_peer_addr:"(unset)", cfg->udp_peer_port,
             cfg->udp_coalesce_bytes, cfg->udp_coalesce_idle_ms, cfg->udp_max_datagram);
    }
    return 0;
}

static void disconnect_tcp(state_t *st){
    if(st->fd_net>=0){ del_ep(st->epfd,st->fd_net); close_fd(&st->fd_net); }
    st->tcp_connected=false; st->disconnects++; st->tcp_peer_ip[0]=0; st->tcp_peer_port=0;
    st->tcp_out.len=st->tcp_out.head=st->tcp_out.tail=0;
    vlog(1, "TCP: disconnected");
}

/* ------------------------------- Stats dump --------------------------------- */
static void dump_ini(const config_t *cfg, state_t *st){
    FILE *f=fopen(cfg->log_file,"w"); if(!f) return;
    struct timespec rt; clock_gettime(CLOCK_REALTIME,&rt);
    long long ts_ms=(long long)rt.tv_sec*1000LL + rt.tv_nsec/1000000LL;
    fprintf(f,"timestamp_ms=%lld\n",ts_ms);

    fprintf(f,"uart_backend=%s\n", cfg->uart_backend==UART_TTY?"tty":"stdio");
    const char *nmode = (cfg->net_mode==NET_TCP_SERVER)?"tcp_server":(cfg->net_mode==NET_TCP_CLIENT)?"tcp_client":"udp_peer";
    fprintf(f,"net_mode=%s\n", nmode);

    fprintf(f,"tcp_connected=%d\n", st->tcp_connected?1:0);
    fprintf(f,"tcp_peer_ip=%s\n", (st->tcp_connected&&st->tcp_peer_ip[0])?st->tcp_peer_ip:"");
    fprintf(f,"tcp_peer_port=%d\n", st->tcp_connected?st->tcp_peer_port:0);

    fprintf(f,"bytes_uart_to_net=%llu\n",(unsigned long long)st->bytes_uart_to_net);
    fprintf(f,"bytes_net_to_uart=%llu\n",(unsigned long long)st->bytes_net_to_uart);
    fprintf(f,"pkts_uart_to_net=%llu\n",(unsigned long long)st->pkts_uart_to_net);
    fprintf(f,"pkts_net_to_uart=%llu\n",(unsigned long long)st->pkts_net_to_uart);
    fprintf(f,"connects=%llu\n",(unsigned long long)st->connects);
    fprintf(f,"disconnects=%llu\n",(unsigned long long)st->disconnects);
    fprintf(f,"drops_uart_to_net=%llu\n",(unsigned long long)st->drops_uart_to_net);
    fprintf(f,"drops_net_to_uart=%llu\n",(unsigned long long)st->drops_net_to_uart);

    fprintf(f,"tcp_out_queued=%zu\n", st->tcp_out.len);
    fprintf(f,"uart_out_queued=%zu\n", st->uart_out.len);
    fprintf(f,"udp_out_len=%zu\n", st->udp_out_len);

    cadence_stats_t stats_utn; cadence_collect_stats(st, true, &stats_utn);
    cadence_stats_t stats_ntu; cadence_collect_stats(st, false, &stats_ntu);

    bool stale_utn = (st->pkts_uart_to_net == st->last_log_pkts_uart_to_net);
    bool stale_ntu = (st->pkts_net_to_uart == st->last_log_pkts_net_to_uart);

    if(stale_utn){
        stats_utn.samples = 0;
        stats_utn.min_us = 0;
        stats_utn.max_us = 0;
        stats_utn.p95_us = 0;
        stats_utn.avg_interval_us = 0.0;
    }
    if(stale_ntu){
        stats_ntu.samples = 0;
        stats_ntu.min_us = 0;
        stats_ntu.max_us = 0;
        stats_ntu.p95_us = 0;
        stats_ntu.avg_interval_us = 0.0;
    }

    fprintf(f,"interval_uart_to_net_samples=%zu\n", stats_utn.samples);
    fprintf(f,"interval_us_uart_to_net_min=%llu\n", (unsigned long long)stats_utn.min_us);
    fprintf(f,"interval_us_uart_to_net_max=%llu\n", (unsigned long long)stats_utn.max_us);
    fprintf(f,"interval_us_uart_to_net_p95=%llu\n", (unsigned long long)stats_utn.p95_us);
    fprintf(f,"pps_uart_to_net_recent=%.3f\n", (stats_utn.avg_interval_us>0.0)?(1000000.0/stats_utn.avg_interval_us):0.0);

    fprintf(f,"interval_net_to_uart_samples=%zu\n", stats_ntu.samples);
    fprintf(f,"interval_us_net_to_uart_min=%llu\n", (unsigned long long)stats_ntu.min_us);
    fprintf(f,"interval_us_net_to_uart_max=%llu\n", (unsigned long long)stats_ntu.max_us);
    fprintf(f,"interval_us_net_to_uart_p95=%llu\n", (unsigned long long)stats_ntu.p95_us);
    fprintf(f,"pps_net_to_uart_recent=%.3f\n", (stats_ntu.avg_interval_us>0.0)?(1000000.0/stats_ntu.avg_interval_us):0.0);

    if(cfg->expected_hz > 0.0){
        double actual_utn = (stats_utn.avg_interval_us>0.0)?(1000000.0/stats_utn.avg_interval_us):0.0;
        double actual_ntu = (stats_ntu.avg_interval_us>0.0)?(1000000.0/stats_ntu.avg_interval_us):0.0;
        int score_utn = 0;
        int score_ntu = 0;
        if(stats_utn.samples>0){
            double pct = actual_utn / cfg->expected_hz;
            if(pct >= 1.0) score_utn = 100;
            else if(pct <= 0.0) score_utn = 0;
            else {
                double scaled = pct * 100.0;
                if(scaled < 0.0) scaled = 0.0;
                score_utn = (int)(scaled + 0.5);
                if(score_utn > 100) score_utn = 100;
            }
        }
        if(stats_ntu.samples>0){
            double pct = actual_ntu / cfg->expected_hz;
            if(pct >= 1.0) score_ntu = 100;
            else if(pct <= 0.0) score_ntu = 0;
            else {
                double scaled = pct * 100.0;
                if(scaled < 0.0) scaled = 0.0;
                score_ntu = (int)(scaled + 0.5);
                if(score_ntu > 100) score_ntu = 100;
            }
        }
        fprintf(f,"expected_hz=%.3f\n", cfg->expected_hz);
        fprintf(f,"cadence_score_uart_to_net=%d\n", score_utn);
        fprintf(f,"cadence_score_net_to_uart=%d\n", score_ntu);
    }
    fclose(f);
    st->last_log_pkts_uart_to_net = st->pkts_uart_to_net;
    st->last_log_pkts_net_to_uart = st->pkts_net_to_uart;
    vlog(3, "stats: wrote %s", cfg->log_file);
}

/* --------------------------- Periodic timers -------------------------------- */
static void tick_status(const config_t *cfg, state_t *st){
    if(cfg->status_interval_ms<=0) return;
    struct timespec now; get_mono(&now);
    if(st->last_status.tv_sec==0 && st->last_status.tv_nsec==0){ st->last_status=now; dump_ini(cfg,st); return; }
    if(diff_ms(&now,&st->last_status)>=cfg->status_interval_ms){ dump_ini(cfg,st); st->last_status=now; }
}

/* ----------------------------- UDP coalescing ------------------------------- */
static void udp_flush_if_ready(const config_t *cfg, state_t *st, bool force, uint32_t base_events, const char *reason){
    if(cfg->net_mode!=NET_UDP_PEER) return;
    if(!st->udp_peer_set){ st->udp_out_len=0; return; }
    if(st->udp_out_len==0) return;

    bool size_ready = (int)st->udp_out_len >= cfg->udp_coalesce_bytes;
    bool send_now = force || size_ready;
    if(!send_now) return;

    ssize_t w = sendto(st->fd_net, st->udp_out, st->udp_out_len, 0,
                       (struct sockaddr*)&st->udp_peer, sizeof(st->udp_peer));
    if(w==(ssize_t)st->udp_out_len){
        st->bytes_uart_to_net += (uint64_t)w; st->pkts_uart_to_net += 1;
        cadence_note_packet(st, true);
        vlog(3, "UDP: sent datagram bytes=%zd reason=%s", w, reason?reason:"(unknown)");
        st->udp_out_len=0;
        mod_ep(st->epfd, st->fd_net, base_events);
    } else if(w<0 && (errno==EAGAIN||errno==EWOULDBLOCK||errno==ENOBUFS)){
        mod_ep(st->epfd, st->fd_net, base_events|EPOLLOUT);
        vlog(2, "UDP: EAGAIN/ENOBUFS (reason=%s), will retry", reason?reason:"(unknown)");
    } else {
        st->drops_uart_to_net += st->udp_out_len;
        vlog(1, "UDP: send error (%d) dropping datagram reason=%s", errno, reason?reason:"(unknown)");
        st->udp_out_len=0;
        mod_ep(st->epfd, st->fd_net, base_events);
    }
}

/* --------------------------------- main ------------------------------------- */
int main(int argc, char **argv){
    const char *conf_path = DEFAULT_CONF;

    // Improved arg parsing: accept -v, -vv, -vvv, ... (count 'v's)
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"-c") && i+1<argc) { conf_path=argv[++i]; }
        else if (argv[i][0]=='-' && argv[i][1]=='v') {
            const char *p = argv[i]+1;
            while (*p=='v'){ g_verbosity++; p++; }
        } else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){
            fprintf(stderr,
                "Usage: %s [-c /path/to/conf] [-v|-vv|-vvv]\n"
                "  -c FILE   Path to config (default %s)\n"
                "  -v        Verbose (repeat v's for more)\n",
                argv[0], DEFAULT_CONF);
            return 0;
        }
    }

    config_t cfg;
    if(parse_config(conf_path,&cfg)<0){
        fprintf(stderr,"Failed to read config: %s (%s)\n", conf_path, strerror(errno));
        return 1;
    }
    vlog(1, "Loaded config: uart_backend=%s, net_mode=%s",
         (cfg.uart_backend==UART_TTY?"tty":"stdio"),
         (cfg.net_mode==NET_TCP_SERVER?"tcp_server":(cfg.net_mode==NET_TCP_CLIENT?"tcp_client":"udp_peer")));

    state_t st; memset(&st,0,sizeof(st));
    reset_cadence_stats(&st);
    st.fd_uart=st.fd_listen=st.fd_net=-1; st.fd_stdout=-1;
    st.epfd=epoll_create1(0); if(st.epfd<0){ perror("epoll_create1"); return 1; }

    size_t udp_out_cap = cfg.udp_max_datagram>0?(size_t)cfg.udp_max_datagram:1200;
    st.udp_out=(uint8_t*)malloc(udp_out_cap);
    st.udp_out_len=0;
    st.udp_out_cap=udp_out_cap;
    if(!st.udp_out){
        st.udp_out_cap=0;
        fprintf(stderr,"Failed to allocate UDP buffer (%s)\n", strerror(errno));
        return 1;
    }
    if(ring_init(&st.tcp_out,cfg.tx_buf)<0 || ring_init(&st.uart_out,cfg.tx_buf)<0){
        fprintf(stderr,"Failed to allocate ring buffers (%s)\n", strerror(errno));
        free(st.udp_out); st.udp_out=NULL;
        return 1;
    }

    struct sigaction sa={0}; sa.sa_handler=on_sighup; sigaction(SIGHUP,&sa,NULL);
    sa.sa_handler=on_sigterm; sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);

    if(reopen_everything(&cfg,&st)<0){
        fprintf(stderr,"Failed to open UART/STDIO/network (%s)\n", strerror(errno));
        return 1;
    }

    if(cfg.dump_on_start) dump_ini(&cfg,&st);

    uint8_t *buf_uart=malloc(cfg.rx_buf), *buf_net=malloc(cfg.rx_buf);
    size_t rx_buf_cap = cfg.rx_buf;
    if(!buf_uart||!buf_net){ fprintf(stderr,"OOM\n"); free(buf_uart); free(buf_net); return 1; }

    st.running=true; get_mono(&st.last_uart_rx);

    while(st.running && !g_stop){
        if(g_reload){
            vlog(1, "SIGHUP: reloading %s", conf_path);
            g_reload=0;
            config_t newcfg;
            if(parse_config(conf_path,&newcfg)==0){
                vlog(2, "SIGHUP: parse ok");
                config_t oldcfg = cfg;

                size_t desired_udp_cap = newcfg.udp_max_datagram>0?(size_t)newcfg.udp_max_datagram:1200;
                bool udp_resize = desired_udp_cap != st.udp_out_cap;
                uint8_t *new_udp_out = NULL;
                if(udp_resize){
                    new_udp_out = (uint8_t*)malloc(desired_udp_cap);
                    if(!new_udp_out){
                        vlog(1, "SIGHUP: UDP buffer alloc failed (%s), keeping previous config", strerror(errno));
                        continue;
                    }
                }

                size_t desired_rx = newcfg.rx_buf>0?newcfg.rx_buf:1;
                bool rx_resize = desired_rx != rx_buf_cap;
                uint8_t *new_buf_uart=NULL, *new_buf_net=NULL;
                if(rx_resize){
                    new_buf_uart=(uint8_t*)malloc(desired_rx);
                    new_buf_net=(uint8_t*)malloc(desired_rx);
                    if(!new_buf_uart||!new_buf_net){
                        int err = errno;
                        free(new_buf_uart);
                        free(new_buf_net);
                        if(new_udp_out) free(new_udp_out);
                        vlog(1, "SIGHUP: RX buffer alloc failed (%s), keeping previous config", strerror(err));
                        continue;
                    }
                }

                if(reopen_everything(&newcfg,&st)<0){
                    int err = errno;
                    vlog(1, "SIGHUP: reopen failed (%s), attempting to restore previous config", strerror(err));
                    if(reopen_everything(&oldcfg,&st)<0){
                        int restore_err = errno;
                        vlog(0, "SIGHUP: failed to restore previous config (%s), stopping", strerror(restore_err));
                        if(rx_resize){ free(new_buf_uart); free(new_buf_net); }
                        if(new_udp_out) free(new_udp_out);
                        st.running=false;
                        break;
                    }
                    cfg = oldcfg;
                    if(rx_resize){ free(new_buf_uart); free(new_buf_net); }
                    if(new_udp_out) free(new_udp_out);
                    memset(&st.last_status,0,sizeof(st.last_status));
                    dump_ini(&cfg,&st);
                    continue;
                }

                cfg = newcfg;

                if(rx_resize){
                    free(buf_uart);
                    free(buf_net);
                    buf_uart = new_buf_uart;
                    buf_net = new_buf_net;
                    rx_buf_cap = desired_rx;
                }

                if(new_udp_out){
                    uint8_t *old_udp = st.udp_out;
                    st.udp_out = new_udp_out;
                    st.udp_out_cap = desired_udp_cap;
                    st.udp_out_len = 0;
                    free(old_udp);
                } else {
                    st.udp_out_cap = desired_udp_cap;
                    st.udp_out_len = 0;
                }

                vlog(1, "SIGHUP: reopen successful (uart_backend=%s net_mode=%s)",
                     (cfg.uart_backend==UART_TTY?"tty":"stdio"),
                     (cfg.net_mode==NET_TCP_SERVER?"tcp_server":(cfg.net_mode==NET_TCP_CLIENT?"tcp_client":"udp_peer")));
                memset(&st.last_status,0,sizeof(st.last_status));
                dump_ini(&cfg,&st);
            } else {
                vlog(1, "SIGHUP: parse failed, keeping previous config");
            }
        }

        if(cfg.net_mode==NET_TCP_CLIENT && st.fd_net<0)
            maybe_start_tcp_dial(&cfg, &st);

        int timeout_ms=500;
        if(cfg.net_mode==NET_UDP_PEER && st.udp_out_len>0 && cfg.udp_coalesce_idle_ms>0){
            struct timespec now; get_mono(&now);
            long long waited=diff_ms(&now,&st.last_uart_rx);
            long long remain=cfg.udp_coalesce_idle_ms - waited;
            if(remain<0) remain=0;
            if(remain<timeout_ms) timeout_ms=(int)remain;
        }
        if(cfg.net_mode==NET_TCP_CLIENT && st.fd_net<0 && st.reconnect_pending){
            struct timespec now; get_mono(&now);
            long long remain=diff_ms(&st.next_reconnect,&now);
            if(remain<0) remain=0;
            if(remain<timeout_ms) timeout_ms=(int)remain;
        }
        if(cfg.status_interval_ms>0 && timeout_ms>cfg.status_interval_ms) timeout_ms=cfg.status_interval_ms;

        // NET events
        uint32_t net_events = EPOLLIN;
        if(cfg.net_mode==NET_TCP_CLIENT && st.fd_net>=0 && !st.tcp_connected) net_events|=EPOLLOUT;
        if(cfg.net_mode!=NET_UDP_PEER && st.tcp_out.len>0 && st.fd_net>=0) net_events|=EPOLLOUT;
        if(cfg.net_mode==NET_UDP_PEER && st.udp_out_len>0) net_events|=EPOLLOUT;
        if(st.fd_net>=0) mod_ep(st.epfd, st.fd_net, net_events);

        // UART/STDIO events
        if(cfg.uart_backend==UART_TTY){
            uint32_t uart_events = EPOLLIN | (st.uart_out.len>0?EPOLLOUT:0);
            mod_ep(st.epfd, st.fd_uart, uart_events);
        } else {
            if(st.uart_out.len>0 && !st.stdout_registered){ add_ep(st.epfd, STDOUT_FILENO, EPOLLOUT); st.stdout_registered=true; }
            else if(st.uart_out.len==0 && st.stdout_registered){ del_ep(st.epfd, STDOUT_FILENO); st.stdout_registered=false; }
        }

        tick_status(&cfg,&st);

        struct epoll_event evs[MAX_EVENTS]; int n=epoll_wait(st.epfd, evs, MAX_EVENTS, timeout_ms);
        if(n<0){ if(errno==EINTR) continue; break; }

        if(cfg.net_mode==NET_TCP_CLIENT && st.fd_net<0)
            maybe_start_tcp_dial(&cfg, &st);

        // UDP idle flush
        if(cfg.net_mode==NET_UDP_PEER && st.udp_out_len>0 && cfg.udp_coalesce_idle_ms>0){
            struct timespec now; get_mono(&now);
            if(diff_ms(&now,&st.last_uart_rx) >= cfg.udp_coalesce_idle_ms){
                udp_flush_if_ready(&cfg,&st,true,EPOLLIN,"idle_timeout");
            }
        }

        for(int i=0;i<n;i++){
            int fd=evs[i].data.fd; uint32_t ev=evs[i].events;

            // TCP server accept
            if(fd==st.fd_listen && (ev&EPOLLIN)){
                struct sockaddr_in ca; socklen_t clen=sizeof(ca);
                int cfd=accept(st.fd_listen,(struct sockaddr*)&ca,&clen);
                if(cfd>=0){
                    set_nonblock(cfd);
                    if(st.fd_net>=0){ del_ep(st.epfd,st.fd_net); close_fd(&st.fd_net); st.disconnects++; }
                    st.fd_net=cfd; add_ep(st.epfd, st.fd_net, EPOLLIN);
                    st.tcp_connected=true; st.connects++;
                    inet_ntop(AF_INET,&ca.sin_addr,st.tcp_peer_ip,sizeof(st.tcp_peer_ip));
                    st.tcp_peer_port=ntohs(ca.sin_port);
                    if(cfg.tcp_nodelay) apply_tcp_nodelay(st.fd_net,1);
                    vlog(1, "TCP server: accepted %s:%d", st.tcp_peer_ip, st.tcp_peer_port);
                }
                continue;
            }

            // TCP client connect complete
            if(fd==st.fd_net && (ev&EPOLLOUT) && cfg.net_mode==NET_TCP_CLIENT && !st.tcp_connected){
                int err=0; socklen_t elen=sizeof(err);
                if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&elen)==0 && err==0){
                    st.tcp_connected=true; st.connects++;
                    st.reconnect_pending=false; memset(&st.next_reconnect,0,sizeof(st.next_reconnect));
                    mod_ep(st.epfd, fd, EPOLLIN | (st.tcp_out.len?EPOLLOUT:0));
                    struct sockaddr_in pa; socklen_t pl=sizeof(pa);
                    if(getpeername(fd,(struct sockaddr*)&pa,&pl)==0){
                        inet_ntop(AF_INET,&pa.sin_addr,st.tcp_peer_ip,sizeof(st.tcp_peer_ip));
                        st.tcp_peer_port=ntohs(pa.sin_port);
                    } else {
                        strncpy(st.tcp_peer_ip,cfg.remote_host,sizeof(st.tcp_peer_ip)-1); st.tcp_peer_ip[sizeof(st.tcp_peer_ip)-1]=0;
                        st.tcp_peer_port=cfg.remote_port;
                    }
                    if(cfg.tcp_nodelay) apply_tcp_nodelay(st.fd_net,1);
                    vlog(1, "TCP client: connected to %s:%d", st.tcp_peer_ip, st.tcp_peer_port);
                } else {
                    del_ep(st.epfd,fd); close_fd(&st.fd_net);
                    vlog(2, "TCP client: connect failed (err=%d)", err);
                    schedule_tcp_reconnect(&cfg, &st, "connect failed");
                }
                continue;
            }

            // UART/STDIN -> NET
            if(fd==st.fd_uart && (ev&EPOLLIN)){
                ssize_t r=read(st.fd_uart,(void*)buf_uart,cfg.rx_buf);
                if(r>0){
                    get_mono(&st.last_uart_rx);
                    if(cfg.net_mode==NET_UDP_PEER){
                        size_t room=(size_t)cfg.udp_max_datagram - st.udp_out_len;
                        if((size_t)r>room){
                            udp_flush_if_ready(&cfg,&st,true,EPOLLIN,"buffer_full");
                            room=(size_t)cfg.udp_max_datagram - st.udp_out_len;
                        }
                        if((size_t)r>room){
                            size_t off=0;
                            while(off<(size_t)r){
                                size_t chunk=(size_t)r-off; if(chunk>(size_t)cfg.udp_max_datagram) chunk=(size_t)cfg.udp_max_datagram;
                                memcpy(st.udp_out, buf_uart+off, chunk); st.udp_out_len=chunk;
                                udp_flush_if_ready(&cfg,&st,true,EPOLLIN,"buffer_full");
                                off+=chunk;
                            }
                        } else {
                            memcpy(st.udp_out+st.udp_out_len, buf_uart, (size_t)r);
                            st.udp_out_len+=(size_t)r;
                            udp_flush_if_ready(&cfg,&st,false,EPOLLIN, st.udp_out_len >= (size_t)cfg.udp_coalesce_bytes ? "size_threshold" : "pending");
                        }
                    } else {
                        if(st.fd_net>=0){
                            ssize_t w=send(st.fd_net, buf_uart, (size_t)r, MSG_NOSIGNAL);
                            if(w>0){
                                st.bytes_uart_to_net+=(uint64_t)w; st.pkts_uart_to_net+=1;
                                cadence_note_packet(&st, true);
                                if(w<r){
                                    size_t rem=(size_t)r-(size_t)w;
                                    size_t wr=ring_write(&st.tcp_out, buf_uart+w, rem);
                                    if(wr<rem) st.drops_uart_to_net+=(uint64_t)(rem-wr);
                                    mod_ep(st.epfd, st.fd_net, EPOLLIN|EPOLLOUT);
                                }
                            } else if(w<0 && (errno==EAGAIN||errno==EWOULDBLOCK)){
                                size_t wr=ring_write(&st.tcp_out, buf_uart, (size_t)r);
                                if(wr<(size_t)r) st.drops_uart_to_net+=(uint64_t)((size_t)r-wr);
                                mod_ep(st.epfd, st.fd_net, EPOLLIN|EPOLLOUT);
                            }
                        }
                    }
                }
            }

            // NET -> UART/STDOUT
            if(fd==st.fd_net && (ev&EPOLLIN)){
                if(cfg.net_mode==NET_UDP_PEER){
                    struct sockaddr_in from; socklen_t flen=sizeof(from);
                    ssize_t r=recvfrom(st.fd_net, buf_net, cfg.rx_buf, 0,(struct sockaddr*)&from,&flen);
                    if(r>0){
                        if(!cfg.udp_peer_addr[0]){
                            bool changed = !st.udp_peer_set ||
                                st.udp_peer.sin_addr.s_addr!=from.sin_addr.s_addr ||
                                st.udp_peer.sin_port!=from.sin_port;
                            if(changed){
                                st.udp_peer = from; st.udp_peer_set=true;
                                char ipbuf[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET,&from.sin_addr,ipbuf,sizeof(ipbuf));
                                vlog(1, "UDP: learned peer %s:%d", ipbuf, ntohs(from.sin_port));
                            }
                        }
                        int outfd = (cfg.uart_backend==UART_STDIO)? STDOUT_FILENO : st.fd_uart;
                        ssize_t w=write(outfd, buf_net, (size_t)r);
                        if(w>0){
                            st.bytes_net_to_uart+=(uint64_t)w; st.pkts_net_to_uart+=1;
                            cadence_note_packet(&st, false);
                            if(w<r){
                                size_t rem=(size_t)r-(size_t)w;
                                size_t wr=ring_write(&st.uart_out, buf_net+w, rem);
                                if(wr<rem) st.drops_net_to_uart+=(uint64_t)(rem-wr);
                                if(cfg.uart_backend==UART_STDIO){
                                    if(!st.stdout_registered){ add_ep(st.epfd, STDOUT_FILENO, EPOLLOUT); st.stdout_registered=true; }
                                } else {
                                    mod_ep(st.epfd, st.fd_uart, EPOLLIN|EPOLLOUT);
                                }
                            }
                        } else if(w<0 && (errno==EAGAIN||errno==EWOULDBLOCK)){
                            size_t wr=ring_write(&st.uart_out, buf_net, (size_t)r);
                            if(wr<(size_t)r) st.drops_net_to_uart+=(uint64_t)((size_t)r-wr);
                            if(cfg.uart_backend==UART_STDIO){
                                if(!st.stdout_registered){ add_ep(st.epfd, STDOUT_FILENO, EPOLLOUT); st.stdout_registered=true; }
                            } else {
                                mod_ep(st.epfd, st.fd_uart, EPOLLIN|EPOLLOUT);
                            }
                        }
                    }
                } else {
                    ssize_t r=recv(st.fd_net, buf_net, cfg.rx_buf, 0);
                    if(r>0){
                        int outfd = (cfg.uart_backend==UART_STDIO)? STDOUT_FILENO : st.fd_uart;
                        ssize_t w=write(outfd, buf_net, (size_t)r);
                        if(w>0){
                            st.bytes_net_to_uart+=(uint64_t)w; st.pkts_net_to_uart+=1;
                            if(w<r){
                                size_t rem=(size_t)r-(size_t)w;
                                size_t wr=ring_write(&st.uart_out, buf_net+w, rem);
                                if(wr<rem) st.drops_net_to_uart+=(uint64_t)(rem-wr);
                                if(cfg.uart_backend==UART_STDIO){
                                    if(!st.stdout_registered){ add_ep(st.epfd, STDOUT_FILENO, EPOLLOUT); st.stdout_registered=true; }
                                } else {
                                    mod_ep(st.epfd, st.fd_uart, EPOLLIN|EPOLLOUT);
                                }
                            }
                        } else if(w<0 && (errno==EAGAIN||errno==EWOULDBLOCK)){
                            size_t wr=ring_write(&st.uart_out, buf_net, (size_t)r);
                            if(wr<(size_t)r) st.drops_net_to_uart+=(uint64_t)((size_t)r-wr);
                            if(cfg.uart_backend==UART_STDIO){
                                if(!st.stdout_registered){ add_ep(st.epfd, STDOUT_FILENO, EPOLLOUT); st.stdout_registered=true; }
                            } else {
                                mod_ep(st.epfd, st.fd_uart, EPOLLIN|EPOLLOUT);
                            }
                        } else if(r==0){
                            if(cfg.net_mode==NET_TCP_SERVER || cfg.net_mode==NET_TCP_CLIENT){
                                disconnect_tcp(&st);
                                if(cfg.net_mode==NET_TCP_CLIENT){
                                    schedule_tcp_reconnect(&cfg, &st, "peer closed");
                                }
                            }
                        }
                    } else if(r==0){
                        if(cfg.net_mode==NET_TCP_SERVER || cfg.net_mode==NET_TCP_CLIENT){
                            disconnect_tcp(&st);
                            if(cfg.net_mode==NET_TCP_CLIENT){
                                schedule_tcp_reconnect(&cfg, &st, "peer closed");
                            }
                        }
                    } else if(errno!=EAGAIN && errno!=EWOULDBLOCK){
                        if(cfg.net_mode==NET_TCP_SERVER || cfg.net_mode==NET_TCP_CLIENT){
                            disconnect_tcp(&st);
                            if(cfg.net_mode==NET_TCP_CLIENT){
                                schedule_tcp_reconnect(&cfg, &st, "socket error");
                            }
                        }
                    }
                }
            }

            // NET EPOLLOUT
            if(fd==st.fd_net && (ev&EPOLLOUT)){
                if(cfg.net_mode==NET_UDP_PEER){
                    if(st.udp_out_len>0) udp_flush_if_ready(&cfg,&st,true,EPOLLIN,"retry");
                    else mod_ep(st.epfd, st.fd_net, EPOLLIN);
                } else {
                    if(st.tcp_out.len>0){ ssize_t w=send_from_ring_tcp(st.fd_net,&st.tcp_out); if(w>0) st.bytes_uart_to_net+=(uint64_t)w; }
                    mod_ep(st.epfd, st.fd_net, EPOLLIN|(st.tcp_out.len?EPOLLOUT:0));
                }
            }

            // TTY UART EPOLLOUT
            if(cfg.uart_backend==UART_TTY && fd==st.fd_uart && (ev&EPOLLOUT)){
                if(st.uart_out.len>0){ ssize_t w=write_from_ring_fd(st.fd_uart,&st.uart_out); if(w>0) st.bytes_net_to_uart+=(uint64_t)w; }
                uint32_t want=EPOLLIN | (st.uart_out.len?EPOLLOUT:0); mod_ep(st.epfd, st.fd_uart, want);
            }

            // STDOUT EPOLLOUT
            if(cfg.uart_backend==UART_STDIO && st.stdout_registered && fd==STDOUT_FILENO && (ev&EPOLLOUT)){
                if(st.uart_out.len>0){ ssize_t w=write_from_ring_fd(STDOUT_FILENO,&st.uart_out); if(w>0) st.bytes_net_to_uart+=(uint64_t)w; }
                if(st.uart_out.len==0){ del_ep(st.epfd, STDOUT_FILENO); st.stdout_registered=false; }
            }
        }

        // UDP size-trigger flush after events
        if(cfg.net_mode==NET_UDP_PEER) udp_flush_if_ready(&cfg,&st,false,EPOLLIN, st.udp_out_len >= (size_t)cfg.udp_coalesce_bytes ? "size_threshold" : "pending");
    }

    dump_ini(&cfg,&st);
    vlog(1, "Exiting");

    if(cfg.uart_backend==UART_TTY && st.fd_uart>=0) del_ep(st.epfd,st.fd_uart), close_fd(&st.fd_uart);
    if(st.fd_net>=0) del_ep(st.epfd,st.fd_net), close_fd(&st.fd_net);
    if(st.fd_listen>=0) del_ep(st.epfd,st.fd_listen), close_fd(&st.fd_listen);
    if(st.stdout_registered){ del_ep(st.epfd, STDOUT_FILENO); }
    if(st.epfd>=0) close(st.epfd);
    free(buf_uart); free(buf_net);
    ring_free(&st.tcp_out); ring_free(&st.uart_out);
    free(st.udp_out);
    return 0;
}
