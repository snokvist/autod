/*
 * antenna_osd.c  (v3.0 — 2025-08-08)
 * - Add single-loop non-blocking HTTP server
 * - Endpoints:
 *     GET  /api/v1/status           -> JSON live status
 *     GET  /api/v1/osd_last         -> latest rendered OSD text (plain)
 * - One hot select() loop; all sockets O_NONBLOCK
 * - Keeps last OSD render in memory for /api/v1/osd_last
 * - Maintains existing behavior, file wildcards
 *
 * antenna_osd.c  (v2.x — previous)
 * - RSSI bar OSD, UDP/RSSI optional 2nd bar, system msg timeout, globbed input
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <netinet/ip_icmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <netinet/tcp.h>

static int last_valid_rssi=0, neg1_count_rssi=0, last_valid_udp=0, neg1_count_udp=0;
static char *info_buf=NULL; static size_t info_size=0; static time_t last_info_attempt=0; static bool info_buf_valid=false;
static int rssi_hist[3]={-1,-1,-1}, udp_hist[3]={-1,-1,-1};
static char *last_osd_text=NULL; static size_t last_osd_len=0;

#define DEF_CFG_FILE        "/etc/antenna_osd.conf"
#define DEF_INFO_FILE       "/proc/net/*8*/wlan0/trx_info_debug"
#define DEF_OUT_FILE        "/tmp/MSPOSD.msg"
#define DEF_INTERVAL        0.10
#define DEF_BAR_WIDTH       37
#define DEF_TOP             80
#define DEF_BOTTOM          20
#define DEF_OSD_HDR         " &F34&L20"
#define DEF_OSD_HDR2        ""
#define DEF_SYS_MSG_HDR     ""
#define DEF_SYS_MSG_TIMEOUT 10
#define DEF_RSSI_CONTROL    0
#define DEF_RSSI_RANGE0     "&F34&L10"
#define DEF_RSSI_RANGE1     "&F34&L10"
#define DEF_RSSI_RANGE2     "&F34&L40"
#define DEF_RSSI_RANGE3     "&F34&L40"
#define DEF_RSSI_RANGE4     "&F34&L20"
#define DEF_RSSI_RANGE5     "&F34&L20"
#define DEF_PING_IP         "192.168.0.10"
#define DEF_START           "["
#define DEF_END             "]"
#define DEF_EMPTY           "."
#define DEF_SHOW_STATS      3
#define SYS_MSG_FILE        "/tmp/osd_system.msg"
#define DEF_RSSI_KEY        "rssi"
#define DEF_CURR_TX_RATE_KEY "curr_tx_rate"
#define DEF_CURR_TX_BW_KEY   "curr_tx_bw"
#define DEF_RSSI_UDP_ENABLE  0
#define DEF_RSSI_UDP_KEY     "rssi_udp"
#define DEF_TX_POWER_KEY     "txpwr"
#define DEF_HTTP_ADDR        "127.0.0.1"
#define DEF_HTTP_PORT        8080
#define DEF_HTTP_CLIENTS     32
#define DEF_HTTP_READ_MAX    16384
#define DEF_HTTP_WRITE_MAX   32768
#define DEF_HTTP_QUANTUM_MS  20   /* how often we wake to serve HTTP */

static inline int64_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}
static inline int64_t sec_to_ms(double s){ return (int64_t)(s*1000.0 + 0.5); }
static inline int64_t clamp_min(int64_t x, int64_t mn){ return x < mn ? mn : x; }


static const char *FULL="\u2588";
static const char *PART[7]={"\u2581","\u2582","\u2583","\u2584","\u2585","\u2586","\u2587"};

typedef struct {
    const char *info_file;
    const char *out_file;
    double      interval;
    int         bar_width;
    int         top;
    int         bottom;
    const char *osd_hdr;
    const char *osd_hdr2;
    const char *sys_msg_hdr;
    char        system_msg[256];
    int        show_stats_line;
    int         sys_msg_timeout;
    bool        rssi_control;
    const char *rssi_hdr[6];
    const char *ping_ip;
    const char *start_sym;
    const char *end_sym;
    const char *empty_sym;
    const char *rssi_key;
    const char *curr_tx_rate_key;
    const char *curr_tx_bw_key;
    bool        rssi_udp_enable;
    const char *rssi_udp_key;
    const char *tx_power_key;
    const char *http_addr;
    int         http_port;
    int         http_max_clients;
} cfg_t;

static cfg_t cfg = {
    .info_file=DEF_INFO_FILE, .out_file=DEF_OUT_FILE, .interval=DEF_INTERVAL, .bar_width=DEF_BAR_WIDTH, .top=DEF_TOP, .bottom=DEF_BOTTOM,
    .osd_hdr=DEF_OSD_HDR, .osd_hdr2=DEF_OSD_HDR2, .sys_msg_hdr=DEF_SYS_MSG_HDR, .system_msg="", .show_stats_line=DEF_SHOW_STATS, .sys_msg_timeout=DEF_SYS_MSG_TIMEOUT,
    .rssi_control=DEF_RSSI_CONTROL, .rssi_hdr={DEF_RSSI_RANGE0,DEF_RSSI_RANGE1,DEF_RSSI_RANGE2,DEF_RSSI_RANGE3,DEF_RSSI_RANGE4,DEF_RSSI_RANGE5},
    .ping_ip=DEF_PING_IP, .start_sym=DEF_START, .end_sym=DEF_END, .empty_sym=DEF_EMPTY, .rssi_key=DEF_RSSI_KEY,
    .curr_tx_rate_key=DEF_CURR_TX_RATE_KEY, .curr_tx_bw_key=DEF_CURR_TX_BW_KEY, .rssi_udp_enable=DEF_RSSI_UDP_ENABLE, .rssi_udp_key=DEF_RSSI_UDP_KEY, .tx_power_key=DEF_TX_POWER_KEY,
    .http_addr=DEF_HTTP_ADDR, .http_port=DEF_HTTP_PORT, .http_max_clients=DEF_HTTP_CLIENTS
};

static void set_cfg_field(const char *k, const char *v)
{
#define EQ(a,b) (strcmp((a),(b))==0)
    if (EQ(k, "info_file")) cfg.info_file = strdup(v);
    else if (EQ(k, "out_file")) cfg.out_file = strdup(v);
    else if (EQ(k, "interval")) cfg.interval = atof(v);
    else if (EQ(k, "bar_width")) cfg.bar_width = atoi(v);
    else if (EQ(k, "top")) cfg.top = atoi(v);
    else if (EQ(k, "bottom")) cfg.bottom = atoi(v);
    else if (EQ(k, "osd_hdr")) cfg.osd_hdr = strdup(v);
    else if (EQ(k, "osd_hdr2")) cfg.osd_hdr2 = strdup(v);
    else if (EQ(k, "sys_msg_hdr")) cfg.sys_msg_hdr = strdup(v);
    else if (EQ(k, "show_stats_line")) {
        if (!strcasecmp(v, "true")) cfg.show_stats_line = 3;
        else if (!strcasecmp(v, "false")) cfg.show_stats_line = 0;
        else {
            int lvl = atoi(v);
            if (lvl < 0) lvl = 0;
            if (lvl > 3) lvl = 3;
            cfg.show_stats_line = lvl;
        }
    }
    else if (EQ(k, "sys_msg_timeout")) cfg.sys_msg_timeout = atoi(v);
    else if (EQ(k, "rssi_control")) cfg.rssi_control = atoi(v) != 0;
    else if (EQ(k, "rssi_range0_hdr")) cfg.rssi_hdr[0] = strdup(v);
    else if (EQ(k, "rssi_range1_hdr")) cfg.rssi_hdr[1] = strdup(v);
    else if (EQ(k, "rssi_range2_hdr")) cfg.rssi_hdr[2] = strdup(v);
    else if (EQ(k, "rssi_range3_hdr")) cfg.rssi_hdr[3] = strdup(v);
    else if (EQ(k, "rssi_range4_hdr")) cfg.rssi_hdr[4] = strdup(v);
    else if (EQ(k, "rssi_range5_hdr")) cfg.rssi_hdr[5] = strdup(v);
    else if (EQ(k, "ping_ip")) cfg.ping_ip = strdup(v);
    else if (EQ(k, "start_sym")) cfg.start_sym = strdup(v);
    else if (EQ(k, "end_sym")) cfg.end_sym = strdup(v);
    else if (EQ(k, "empty_sym")) cfg.empty_sym = strdup(v);
    else if (EQ(k, "rssi_key")) cfg.rssi_key = strdup(v);
    else if (EQ(k, "curr_tx_rate_key")) cfg.curr_tx_rate_key = strdup(v);
    else if (EQ(k, "curr_tx_bw_key")) cfg.curr_tx_bw_key = strdup(v);
    else if (EQ(k, "rssi_udp_enable")) cfg.rssi_udp_enable = atoi(v) != 0;
    else if (EQ(k, "rssi_udp_key")) cfg.rssi_udp_key = strdup(v);
    else if (EQ(k, "tx_power_key")) cfg.tx_power_key = strdup(v);
    else if (EQ(k, "http_addr")) cfg.http_addr = strdup(v);
    else if (EQ(k, "http_port")) cfg.http_port = atoi(v);
    else if (EQ(k, "http_max_clients")) cfg.http_max_clients = atoi(v);
#undef EQ
}

static void load_config(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[antenna_osd] config \"%s\" not found – defaults in use\n", path);
        return;
    }

    char *line = NULL;
    size_t len = 0;
    while (getline(&line, &len, fp) != -1) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\0') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = s;
        char *v = eq + 1;

        char *ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';

        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\n' || ve[-1] == '\r')) *--ve = '\0';

        if (*k) set_cfg_field(k, v);
    }
    free(line);
    fclose(fp);
}

static time_t sys_msg_last_update=0;
static void read_system_msg(void){
    struct stat st;
    if(stat(SYS_MSG_FILE,&st)==0){
        if(st.st_mtime!=sys_msg_last_update){
            FILE *fp=fopen(SYS_MSG_FILE,"r");
            if(fp){
                if(fgets(cfg.system_msg,sizeof(cfg.system_msg),fp)){
                    char *p=strchr(cfg.system_msg,'\n'); if(p)*p='\0';
                }
                fclose(fp);
                sys_msg_last_update=st.st_mtime;
            }
        }
    } else {
        cfg.system_msg[0]='\0';
    }
    time_t now=time(NULL);
    if(cfg.system_msg[0]&&(now-sys_msg_last_update>cfg.sys_msg_timeout)) cfg.system_msg[0]='\0';
}

static uint16_t icmp_cksum(const void *d,size_t l){
    const uint8_t *p=d; uint32_t s=0; while(l>1){uint16_t w; memcpy(&w,p,2); s+=w; p+=2; l-=2;} if(l) s+=*p; s=(s>>16)+(s&0xFFFF); s+=(s>>16); return (uint16_t)~s;
}

static int smooth_rssi_sample(int *hist,int newval){
    if(newval<0) return newval;
    hist[2]=hist[1]; hist[1]=hist[0]; hist[0]=newval;
    if(hist[1]<0||hist[2]<0) return newval;
    return (int)(0.5*hist[0]+0.25*hist[1]+0.25*hist[2]);
}

static int get_display_rssi(int raw){
    if(raw>=0){last_valid_rssi=raw; neg1_count_rssi=0; return raw;}
    if(++neg1_count_rssi>=3) return -1;
    return last_valid_rssi;
}

static int get_display_udp(int raw){
    if(raw>=0){last_valid_udp=raw; neg1_count_udp=0; return raw;}
    if(++neg1_count_udp>=3) return -1;
    return last_valid_udp;
}

static FILE *fopen_glob_first(const char *pattern,const char *mode){
    if(!strpbrk(pattern,"*?[")) return fopen(pattern,mode);
    glob_t g; if(glob(pattern,GLOB_NOSORT,NULL,&g)!=0||g.gl_pathc==0){globfree(&g); return NULL;}
    FILE *fp=fopen(g.gl_pathv[0],mode); globfree(&g); return fp;
}

static bool load_info_buffer(void){
    FILE *fp=fopen_glob_first(cfg.info_file,"r"); if(!fp) return false;
    free(info_buf); info_buf=NULL; info_size=0;
    size_t cap=0; char tmp[256];
    while(fgets(tmp,sizeof(tmp),fp)){
        size_t len=strlen(tmp);
        if(info_size+len+1>cap){cap=(cap+len+1)*2; char *nb=realloc(info_buf,cap); if(!nb){free(info_buf); fclose(fp); return false;} info_buf=nb;}
        memcpy(info_buf+info_size,tmp,len); info_size+=len;
    }
    if(info_buf) info_buf[info_size]='\0'; else {info_buf=strdup(""); info_size=0;}
    fclose(fp); return true;
}

static bool try_initial_load_info(void){
    time_t now=time(NULL);
    if(info_buf_valid) return true;
    if(now-last_info_attempt<3) return false;
    last_info_attempt=now;
    if(load_info_buffer()){info_buf_valid=true; return true;}
    return false;
}

static int parse_int_from_buf(const char *buf,const char *key){
    const char *p=buf;
    while((p=strcasestr(p,key))!=NULL){
        const char *sep=strchr(p,':'); if(!sep) sep=strchr(p,'='); if(sep){sep++; while(*sep==' '||*sep=='\t') sep++; return (int)strtol(sep,NULL,10);}
        p+=strlen(key);
    }
    return -1;
}

static void parse_value_from_buf(const char *buf,const char *key,char *out,size_t outlen){
    const char *p=buf;
    while((p=strcasestr(p,key))!=NULL){
        const char *sep=strchr(p,':'); if(!sep) sep=strchr(p,'='); if(!sep){p+=strlen(key); continue;}
        sep++; while(*sep==' '||*sep=='\t') sep++;
        const char *end=sep; while(*end&&*end!='\n'&&*end!='\r') end++;
        size_t len=end-sep; if(len>=outlen) len=outlen-1; memcpy(out,sep,len); out[len]='\0'; return;
    }
    snprintf(out,outlen,"NA");
}

static int init_icmp_socket(const char *ip,struct sockaddr_in *dst){
    if(!ip||!*ip) return -1;
    int s=socket(AF_INET,SOCK_RAW,IPPROTO_ICMP); if(s<0) return -1;
    memset(dst,0,sizeof(*dst)); dst->sin_family=AF_INET;
    if(inet_pton(AF_INET,ip,&dst->sin_addr)!=1){close(s); return -1;}
    int fl=fcntl(s,F_GETFL,0); fcntl(s,F_SETFL,fl|O_NONBLOCK);
    return s;
}

static int send_icmp_echo(int s,struct sockaddr_in *dst,uint16_t seq){
    struct {struct icmphdr h; char p[56];} pkt={0};
    pkt.h.type=ICMP_ECHO; pkt.h.un.echo.id=htons(getpid()&0xFFFF); pkt.h.un.echo.sequence=htons(seq);
    for(size_t i=0;i<sizeof(pkt.p);++i) pkt.p[i]=(char)i;
    pkt.h.checksum=icmp_cksum(&pkt,sizeof(pkt));
    return sendto(s,&pkt,sizeof(pkt),0,(struct sockaddr*)dst,sizeof(*dst));
}

static void build_bar(char *o,size_t sz,int pct){
    if(pct<0)pct=0; else if(pct>100)pct=100;
    int total=pct*cfg.bar_width*8/100; int full=total/8; int rem=total%8;
    if(full>cfg.bar_width){full=cfg.bar_width; rem=0;}
    size_t pos=0;
    for(int i=0;i<cfg.bar_width;++i){
        const char *sym=cfg.empty_sym;
        if(i<full) sym=FULL; else if(i==full && rem>0) sym=PART[rem-1];
        size_t L=strlen(sym); if(pos+L<sz){memcpy(o+pos,sym,L); pos+=L;}
    }
    o[pos]='\0';
}

static inline const char *choose_rssi_hdr(int pct){
    if(!cfg.rssi_control) return cfg.osd_hdr;
    int idx=(pct*6)/100; if(idx>5) idx=5; return cfg.rssi_hdr[idx];
}

static void set_last_osd(const char *buf,size_t len){
    free(last_osd_text); last_osd_text=malloc(len+1); if(!last_osd_text){last_osd_len=0; return;}
    memcpy(last_osd_text,buf,len); last_osd_text[len]='\0'; last_osd_len=len;
}

static void write_osd(int rssi,int udp_rssi,const char *mcs_str,const char *bw_str,const char *tx_str){
    int pct; if(rssi<0)pct=0; else if(rssi<=cfg.bottom)pct=0; else if(rssi>=cfg.top)pct=100; else pct=(rssi-cfg.bottom)*100/(cfg.top-cfg.bottom);
    char bar[cfg.bar_width*3+1]; build_bar(bar,sizeof(bar),pct); const char *hdr=choose_rssi_hdr(pct);
    int pct_udp=0; char bar_udp[cfg.bar_width*3+1]; const char *hdr_udp=NULL;
    if(cfg.rssi_udp_enable){
        int disp_udp=udp_rssi;
        if(disp_udp<0)pct_udp=0; else if(disp_udp<=cfg.bottom)pct_udp=0; else if(disp_udp>=cfg.top)pct_udp=100; else pct_udp=(disp_udp-cfg.bottom)*100/(cfg.top-cfg.bottom);
        build_bar(bar_udp,sizeof(bar_udp),pct_udp); hdr_udp=choose_rssi_hdr(pct_udp);
    }
    char filebuf[2048]; int flen=0;
    flen+=snprintf(filebuf+flen,sizeof(filebuf)-flen,"%s %3d%% %s%s%s\n",hdr,pct,cfg.start_sym,bar,cfg.end_sym);
    if(cfg.rssi_udp_enable) flen+=snprintf(filebuf+flen,sizeof(filebuf)-flen,"%s %3d%% %s%s%s\n",hdr_udp,pct_udp,cfg.start_sym,bar_udp,cfg.end_sym);
    /* progressive stat line (0..3), osd_hdr2 appended at the end */
    if (cfg.show_stats_line > 0) {
        int lvl = cfg.show_stats_line;
        if (lvl == 1) {
            /* Only battery (&B) */
            flen += snprintf(filebuf+flen, sizeof(filebuf)-flen,
                            "&B%s\n",
                            cfg.osd_hdr2);
        } else if (lvl == 2) {
            /* MCS / BW / TX power + battery */
            flen += snprintf(filebuf+flen, sizeof(filebuf)-flen,
                            "%s / %s / %s | &B%s\n",
                            mcs_str, bw_str, tx_str,
                            cfg.osd_hdr2);
        } else { /* lvl == 3 — full line */
            flen += snprintf(filebuf+flen, sizeof(filebuf)-flen,
                            "TEMP: &TC/&WC | CPU: &C | %s / %s / %s | &B%s\n",
                            mcs_str, bw_str, tx_str,
                            cfg.osd_hdr2);
        }
    }
    if(cfg.system_msg[0]) flen+=snprintf(filebuf+flen,sizeof(filebuf)-flen,"%s%s\n",cfg.sys_msg_hdr,cfg.system_msg);
    FILE *fp=fopen(cfg.out_file,"w"); if(fp){fwrite(filebuf,1,flen,fp); fclose(fp);}
    set_last_osd(filebuf,flen);
}

typedef struct {
    int fd;
    bool used;
    bool want_close;
    char *inbuf;
    size_t inlen, incap;
    char *outbuf;
    size_t outlen, outpos, outcap;
    bool headers_parsed;
    char method[8];
    char path[256];
} client_t;

static int set_nonblock(int fd){int f=fcntl(fd,F_GETFL,0); if(f<0) return -1; return fcntl(fd,F_SETFL,f|O_NONBLOCK);}
static int http_listen_fd=-1; static client_t *clients=NULL; static int max_clients=0;

static int http_listen_init(const char *addr,int port,int maxc){
    int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -1;
    int on=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
    struct sockaddr_in sa; memset(&sa,0,sizeof(sa)); sa.sin_family=AF_INET; sa.sin_port=htons(port);
    if(inet_pton(AF_INET,addr,&sa.sin_addr)!=1){close(fd); return -1;}
    if(bind(fd,(struct sockaddr*)&sa,sizeof(sa))<0){close(fd); return -1;}
    if(listen(fd,64)<0){close(fd); return -1;}
    set_nonblock(fd);
    clients=calloc(maxc,sizeof(client_t)); max_clients=maxc;
    return fd;
}

static client_t* http_alloc_client(int fd){
    for(int i=0;i<max_clients;i++) if(!clients[i].used){clients[i].used=true; clients[i].fd=fd; clients[i].want_close=false;
        clients[i].inbuf=NULL; clients[i].inlen=0; clients[i].incap=0;
        clients[i].outbuf=NULL; clients[i].outlen=0; clients[i].outpos=0; clients[i].outcap=0;
        clients[i].headers_parsed=false; clients[i].method[0]=0; clients[i].path[0]=0; return &clients[i];}
    return NULL;
}
static void http_free_client(client_t *c){
    if(!c||!c->used) return;
    if(c->fd>=0) close(c->fd);
    free(c->inbuf); free(c->outbuf);
    memset(c,0,sizeof(*c));
}

static void buf_append(char **buf,size_t *len,size_t *cap,const void *data,size_t n){
    if(*len+n+1>*cap){size_t nc=(*cap?*cap:512); while(nc<*len+n+1) nc*=2; char *nb=realloc(*buf,nc); if(!nb) return; *buf=nb; *cap=nc;}
    memcpy(*buf+*len,data,n); *len+=n; (*buf)[*len]='\0';
}
static void out_printf(client_t *c,const char *fmt,...){
    char tmp[4096]; va_list ap; va_start(ap,fmt); int n=vsnprintf(tmp,sizeof(tmp),fmt,ap); va_end(ap);
    if(n<0) return;
    if((size_t)n>=sizeof(tmp)) n=sizeof(tmp)-1;
    buf_append(&c->outbuf,&c->outlen,&c->outcap,tmp,n);
}
static void http_send(client_t *c,int code,const char *ctype,const char *body,size_t blen,bool keep){
    out_printf(c,"HTTP/1.1 %d\r\n",code);
    out_printf(c,"Content-Type: %s\r\n",ctype?ctype:"text/plain");
    out_printf(c,"Content-Length: %zu\r\n",blen);
    out_printf(c,"Connection: %s\r\n",keep?"keep-alive":"close");
    out_printf(c,"\r\n");
    buf_append(&c->outbuf,&c->outlen,&c->outcap,body,blen);
    if(!keep) c->want_close=true;
}
static void http_send_text(client_t *c,int code,const char *ctype,const char *s,bool keep){
    http_send(c,code,ctype,(const char*)s,strlen(s),keep);
}
static char hex2(int c){return (c<10)?('0'+c):('A'+c-10);}
static void json_escape_append(client_t *c,const char *s){
    for(const unsigned char *p=(const unsigned char*)s;*p;p++){
        if(*p=='\"'||*p=='\\') out_printf(c,"\\%c",*p);
        else if(*p>=0x20) out_printf(c,"%c",*p);
        else out_printf(c,"\\u00%c%c",hex2((*p>>4)&0xF),hex2((*p)&0xF));
    }
}
static void api_status(client_t *c,int last_rssi,int last_udp,const char *mcs,const char *bw,const char *txp){
    out_printf(c,"HTTP/1.1 200\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n");
    out_printf(c,"{");
    out_printf(c,"\"interval\":%.3f,",cfg.interval);
    out_printf(c,"\"bar_width\":%d,",cfg.bar_width);
    out_printf(c,"\"top\":%d,\"bottom\":%d,",cfg.top,cfg.bottom);
    out_printf(c,"\"rssi\":%d,",last_rssi);
    out_printf(c,"\"rssi_udp\":%d,",cfg.rssi_udp_enable?last_udp:-1);
    out_printf(c,"\"mcs\":\""); json_escape_append(c,mcs); out_printf(c,"\",");
    out_printf(c,"\"bw\":\""); json_escape_append(c,bw); out_printf(c,"\",");
    out_printf(c,"\"tx_power\":\""); json_escape_append(c,txp); out_printf(c,"\",");
    out_printf(c,"\"system_msg\":\""); json_escape_append(c,cfg.system_msg); out_printf(c,"\"");
    out_printf(c,"}\n");
    c->want_close=true;
}
static void api_osd_last(client_t *c){
    const char *s=last_osd_text?last_osd_text:"";
    http_send(c,200,"text/plain",s,strlen(s),false);
}

static void handle_http_request(client_t *c){
    char *hdr_end=strstr(c->inbuf,"\r\n\r\n"); if(!hdr_end) return;
    c->headers_parsed=true;
    char *first=c->inbuf;
    char *sp1=strchr(first,' ');
    if(!sp1) { c->want_close=true; return; }
    size_t mlen=(size_t)(sp1-first); if(mlen>=sizeof(c->method)) mlen=sizeof(c->method)-1; memcpy(c->method,first,mlen); c->method[mlen]='\0';
    char *sp2=strchr(sp1+1,' '); if(!sp2){ c->want_close=true; return; }
    size_t plen=(size_t)(sp2-(sp1+1)); if(plen>=sizeof(c->path)) plen=sizeof(c->path)-1; memcpy(c->path,sp1+1,plen); c->path[plen]='\0';
    if(strcasecmp(c->method,"GET")!=0){
        http_send_text(c,405,"text/plain","method not allowed\n",false);
        return;
    }
    const char *qs=strchr(c->path,'?'); char pure[256];
    if(qs){ size_t n=(size_t)(qs-c->path); if(n>=sizeof(pure)) n=sizeof(pure)-1; memcpy(pure,c->path,n); pure[n]='\0'; }
    else { strncpy(pure,c->path,sizeof(pure)); pure[sizeof(pure)-1]='\0'; }

    if(strcmp(pure,"/api/v1/status")==0){
        char mcs[32],bw[32],txs[32]; int rr=-1,uu=-1;
        rr=last_valid_rssi; uu=last_valid_udp;
        if(info_buf){ parse_value_from_buf(info_buf,cfg.curr_tx_rate_key,mcs,sizeof(mcs)); parse_value_from_buf(info_buf,cfg.curr_tx_bw_key,bw,sizeof(bw)); parse_value_from_buf(info_buf,cfg.tx_power_key,txs,sizeof(txs)); }
        else { strcpy(mcs,"NA"); strcpy(bw,"NA"); strcpy(txs,"NA"); }
        api_status(c,rr,uu,mcs,bw,txs); return;
    }
    if(strcmp(pure,"/api/v1/osd_last")==0){ api_osd_last(c); return; }
    http_send_text(c,404,"text/plain","not found\n",false);
}

int main(int argc, char **argv){
    if(getuid()!=0){ fprintf(stderr,"antenna_osd: need root (raw ICMP)\n"); return 1; }

    /* args */
    static const struct option optv[]={{"help",no_argument,NULL,'h'},{0,0,0,0}};
    int opt; while((opt=getopt_long(argc,argv,"h",optv,NULL))!=-1){
        if(opt=='h'){
            printf("Usage: %s [--help] [config_path]\n",argv[0]);
            return 0;
        }
        return 1;
    }

    const char *cfg_path = DEF_CFG_FILE;
    if(optind < argc){
        if(optind + 1 < argc){
            fprintf(stderr,"Usage: %s [--help] [config_path]\n",argv[0]);
            return 1;
        }
        cfg_path = argv[optind];
    }

    load_config(cfg_path);

    /* ping socket (disabled if no/empty IP or socket fails) */
    bool ping_en = (cfg.ping_ip && *cfg.ping_ip);
    struct sockaddr_in dst; int icmp_sock=-1; uint16_t seq=0;
    if (ping_en){
        icmp_sock = init_icmp_socket(cfg.ping_ip,&dst);
        if (icmp_sock < 0) ping_en = false;
    }

    /* HTTP listen */
    int httpfd = http_listen_init(cfg.http_addr,cfg.http_port,cfg.http_max_clients);
    if(httpfd<0){ fprintf(stderr,"[antenna_osd] http listen failed\n"); return 2; }
    http_listen_fd = httpfd;

    /* scheduling (ms), independent cadences */
    struct timespec ts0; clock_gettime(CLOCK_MONOTONIC,&ts0);
    int64_t t_now_ms = (int64_t)ts0.tv_sec*1000 + ts0.tv_nsec/1000000;

    if (cfg.interval < 0.02) cfg.interval = 0.02;          /* 20 ms floor */
    int64_t osd_period_ms  = (int64_t)(cfg.interval*1000.0 + 0.5);
    int64_t ping_period_ms = (int64_t)((cfg.interval/3.0)*1000.0 + 0.5);
    int64_t next_osd_ms    = t_now_ms + osd_period_ms;
    int64_t next_ping_ms   = t_now_ms + ping_period_ms;

    /* last-known strings for status */
    char last_mcs[32]="NA", last_bw[32]="NA", last_tx[32]="NA";

    /* reload-on-change support (only when no wildcard) */
    bool info_has_wild = (strpbrk(cfg.info_file,"*?[") != NULL);
    time_t last_info_mtime = 0;
    off_t  last_info_size  = -1;

    for(;;){
        fd_set rfds,wfds; FD_ZERO(&rfds); FD_ZERO(&wfds);
        int maxfd = -1;

        /* HTTP listen for new clients */
        FD_SET(http_listen_fd,&rfds); if(http_listen_fd>maxfd) maxfd=http_listen_fd;

        /* NOTE: do NOT watch icmp_sock for writability; it is almost always writable */

        /* Clients */
        for(int i=0;i<max_clients;i++) if(clients[i].used){
            if(clients[i].inlen < DEF_HTTP_READ_MAX) FD_SET(clients[i].fd,&rfds);
            if(clients[i].outlen>clients[i].outpos)   FD_SET(clients[i].fd,&wfds);
            if(clients[i].fd>maxfd) maxfd=clients[i].fd;
        }

        /* dynamic timeout = min(until next OSD, until next ping (if any), idle backoff) */
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
        t_now_ms = (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;

        int64_t to_osd_ms  = next_osd_ms  - t_now_ms;
        int64_t to_ping_ms = ping_en ? (next_ping_ms - t_now_ms) : INT64_MAX;

        bool have_pending_out=false;
        for(int i=0;i<max_clients;i++)
            if(clients[i].used && clients[i].outlen>clients[i].outpos){ have_pending_out=true; break; }

        int64_t idle_backoff_ms = have_pending_out ? 50 : 250; /* tunable; higher lowers CPU */
        int64_t to_ms = idle_backoff_ms;
        if (to_osd_ms  > 0 && to_osd_ms  < to_ms) to_ms = to_osd_ms;
        if (to_ping_ms > 0 && to_ping_ms < to_ms) to_ms = to_ping_ms;
        if (to_osd_ms  <= 0 || (ping_en && to_ping_ms <= 0)) to_ms = 0;

        struct timeval tv; tv.tv_sec = (time_t)(to_ms/1000); tv.tv_usec = (int)((to_ms%1000)*1000);

        /* multiplex */
        int rv = select(maxfd+1,&rfds,&wfds,NULL,&tv);
        (void)rv;

        /* ---- HTTP accept ---- */
        if(FD_ISSET(http_listen_fd,&rfds)){
            for(;;){
                int cfd;
                /* Prefer accept4 with SOCK_NONBLOCK; fallback to accept+fcntl */
                #ifdef SOCK_NONBLOCK
                cfd = accept4(http_listen_fd,NULL,NULL,SOCK_NONBLOCK);
                #else
                cfd = accept(http_listen_fd,NULL,NULL);
                #endif
                if(cfd<0){
                    if(errno==EAGAIN||errno==EWOULDBLOCK) break;
                    else break;
                }
                #ifndef SOCK_NONBLOCK
                set_nonblock(cfd);
                #endif
                /* TCP_NODELAY for snappy small responses */
                int one=1; setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                client_t *cl = http_alloc_client(cfd);
                if(!cl){ close(cfd); break; }
            }
        }

        /* ---- HTTP read ---- */
        for(int i=0;i<max_clients;i++) if(clients[i].used && FD_ISSET(clients[i].fd,&rfds)){
            char buf[4096]; ssize_t n=recv(clients[i].fd,buf,sizeof(buf),0);
            if(n<=0){ clients[i].want_close=true; continue; }
            buf_append(&clients[i].inbuf,&clients[i].inlen,&clients[i].incap,buf,(size_t)n);
            if(!clients[i].headers_parsed) handle_http_request(&clients[i]);
        }

        /* ---- HTTP write ---- */
        for(int i=0;i<max_clients;i++) if(clients[i].used && FD_ISSET(clients[i].fd,&wfds)){
            if(clients[i].outlen>clients[i].outpos){
                ssize_t n=send(clients[i].fd,clients[i].outbuf+clients[i].outpos,clients[i].outlen-clients[i].outpos,0);
                if(n>0) clients[i].outpos+=n; else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK) clients[i].want_close=true;
            }
        }

        /* ---- periodic ping (no fd readiness needed) ---- */
        clock_gettime(CLOCK_MONOTONIC,&ts);
        t_now_ms = (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
        if (ping_en && t_now_ms >= next_ping_ms) {
            send_icmp_echo(icmp_sock,&dst,seq++);
            do next_ping_ms += ping_period_ms; while (next_ping_ms <= t_now_ms);
        }

        /* ---- system message aging ---- */
        read_system_msg();

        /* ---- periodic OSD render/write ---- */
        if (t_now_ms >= next_osd_ms) {
            bool should_load = true;
            if (!info_has_wild){
                struct stat st;
                if (stat(cfg.info_file, &st)==0){
                    if (st.st_mtime == last_info_mtime && st.st_size == last_info_size){
                        should_load = false;  /* unchanged; skip re-parse */
                    } else {
                        last_info_mtime = st.st_mtime; last_info_size = st.st_size;
                    }
                }
            }
            if (!try_initial_load_info()){
                /* advance anyway to avoid hot loop when file unavailable */
                do next_osd_ms += osd_period_ms; while (next_osd_ms <= t_now_ms);
            } else {
                if (should_load){
                    if (!load_info_buffer()){
                        info_buf_valid=false;
                        last_info_attempt=time(NULL);
                    }
                }

                if (!info_buf_valid || !info_buf){
                    strcpy(last_mcs,"NA"); strcpy(last_bw,"NA"); strcpy(last_tx,"NA");
                    int disp_rssi = smooth_rssi_sample(rssi_hist, get_display_rssi(-1));
                    int disp_udp  = cfg.rssi_udp_enable ? smooth_rssi_sample(udp_hist, get_display_udp(-1)) : -1;
                    write_osd(disp_rssi, disp_udp, last_mcs, last_bw, last_tx);
                } else {
                    int raw_rssi = parse_int_from_buf(info_buf,cfg.rssi_key);
                    int raw_udp  = cfg.rssi_udp_enable ? parse_int_from_buf(info_buf,cfg.rssi_udp_key) : -1;

                    int disp_rssi = get_display_rssi(raw_rssi); disp_rssi = smooth_rssi_sample(rssi_hist, disp_rssi);
                    int disp_udp  = get_display_udp(raw_udp);   disp_udp  = smooth_rssi_sample(udp_hist,  disp_udp);

                    parse_value_from_buf(info_buf,cfg.curr_tx_rate_key,last_mcs,sizeof(last_mcs));
                    parse_value_from_buf(info_buf,cfg.curr_tx_bw_key,  last_bw, sizeof(last_bw));
                    parse_value_from_buf(info_buf,cfg.tx_power_key,    last_tx, sizeof(last_tx));

                    write_osd(disp_rssi, disp_udp, last_mcs, last_bw, last_tx);
                }

                do next_osd_ms += osd_period_ms; while (next_osd_ms <= t_now_ms);
            }
        }

        /* ---- close clients that finished sending ---- */
        for (int i=0;i<max_clients;i++) if (clients[i].used){
            bool sent_all = (clients[i].outpos >= clients[i].outlen);
            if (clients[i].want_close && sent_all) http_free_client(&clients[i]);
        }
    }
}
