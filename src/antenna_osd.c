/*
 * antenna_osd.c  (v3.1 — 2025-08-20)
 * - Remove embedded HTTP server and ICMP ping helpers
 * - Retain periodic OSD rendering with smoothed RSSI values
 *
 * antenna_osd.c  (v2.x — previous)
 * - RSSI bar OSD, UDP/RSSI optional 2nd bar, system msg timeout, globbed input
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int last_valid_rssi=0, neg1_count_rssi=0, last_valid_udp=0, neg1_count_udp=0;
static char *info_buf=NULL; static size_t info_size=0; static time_t last_info_attempt=0; static bool info_buf_valid=false;
static int rssi_hist[3]={-1,-1,-1}, udp_hist[3]={-1,-1,-1};

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

static inline int64_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

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
    int         show_stats_line;
    int         sys_msg_timeout;
    bool        rssi_control;
    const char *rssi_hdr[6];
    const char *start_sym;
    const char *end_sym;
    const char *empty_sym;
    const char *rssi_key;
    const char *curr_tx_rate_key;
    const char *curr_tx_bw_key;
    bool        rssi_udp_enable;
    const char *rssi_udp_key;
    const char *tx_power_key;
} cfg_t;

static cfg_t cfg = {
    .info_file=DEF_INFO_FILE, .out_file=DEF_OUT_FILE, .interval=DEF_INTERVAL, .bar_width=DEF_BAR_WIDTH, .top=DEF_TOP, .bottom=DEF_BOTTOM,
    .osd_hdr=DEF_OSD_HDR, .osd_hdr2=DEF_OSD_HDR2, .sys_msg_hdr=DEF_SYS_MSG_HDR, .system_msg="", .show_stats_line=DEF_SHOW_STATS, .sys_msg_timeout=DEF_SYS_MSG_TIMEOUT,
    .rssi_control=DEF_RSSI_CONTROL, .rssi_hdr={DEF_RSSI_RANGE0,DEF_RSSI_RANGE1,DEF_RSSI_RANGE2,DEF_RSSI_RANGE3,DEF_RSSI_RANGE4,DEF_RSSI_RANGE5},
    .start_sym=DEF_START, .end_sym=DEF_END, .empty_sym=DEF_EMPTY, .rssi_key=DEF_RSSI_KEY,
    .curr_tx_rate_key=DEF_CURR_TX_RATE_KEY, .curr_tx_bw_key=DEF_CURR_TX_BW_KEY, .rssi_udp_enable=DEF_RSSI_UDP_ENABLE, .rssi_udp_key=DEF_RSSI_UDP_KEY, .tx_power_key=DEF_TX_POWER_KEY
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
    else if (EQ(k, "start_sym")) cfg.start_sym = strdup(v);
    else if (EQ(k, "end_sym")) cfg.end_sym = strdup(v);
    else if (EQ(k, "empty_sym")) cfg.empty_sym = strdup(v);
    else if (EQ(k, "rssi_key")) cfg.rssi_key = strdup(v);
    else if (EQ(k, "curr_tx_rate_key")) cfg.curr_tx_rate_key = strdup(v);
    else if (EQ(k, "curr_tx_bw_key")) cfg.curr_tx_bw_key = strdup(v);
    else if (EQ(k, "rssi_udp_enable")) cfg.rssi_udp_enable = atoi(v) != 0;
    else if (EQ(k, "rssi_udp_key")) cfg.rssi_udp_key = strdup(v);
    else if (EQ(k, "tx_power_key")) cfg.tx_power_key = strdup(v);
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
    if(!strpbrk(pattern,"*?[]")) return fopen(pattern,mode);
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
    if (cfg.show_stats_line > 0) {
        int lvl = cfg.show_stats_line;
        if (lvl == 1) {
            flen += snprintf(filebuf+flen, sizeof(filebuf)-flen,
                            "&B%s\n",
                            cfg.osd_hdr2);
        } else if (lvl == 2) {
            flen += snprintf(filebuf+flen, sizeof(filebuf)-flen,
                            "%s / %s / %s | &B%s\n",
                            mcs_str, bw_str, tx_str,
                            cfg.osd_hdr2);
        } else {
            flen += snprintf(filebuf+flen, sizeof(filebuf)-flen,
                            "TEMP: &TC/&WC | CPU: &C | %s / %s / %s | &B%s\n",
                            mcs_str, bw_str, tx_str,
                            cfg.osd_hdr2);
        }
    }
    if(cfg.system_msg[0]) flen+=snprintf(filebuf+flen,sizeof(filebuf)-flen,"%s%s\n",cfg.sys_msg_hdr,cfg.system_msg);
    FILE *fp=fopen(cfg.out_file,"w"); if(fp){fwrite(filebuf,1,flen,fp); fclose(fp);}    
}

int main(int argc, char **argv){
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

    if (cfg.interval < 0.02) cfg.interval = 0.02;
    int64_t osd_period_ms = (int64_t)(cfg.interval*1000.0 + 0.5);
    if (osd_period_ms < 20) osd_period_ms = 20;

    int64_t next_osd_ms = now_ms();
    char last_mcs[32]="NA", last_bw[32]="NA", last_tx[32]="NA";

    for(;;){
        int64_t t_now_ms = now_ms();
        if (t_now_ms < next_osd_ms){
            int64_t diff = next_osd_ms - t_now_ms;
            struct timespec ts;
            ts.tv_sec = (time_t)(diff/1000);
            ts.tv_nsec = (long)((diff%1000)*1000000);
            nanosleep(&ts,NULL);
            continue;
        }

        next_osd_ms += osd_period_ms;
        while (next_osd_ms <= t_now_ms) next_osd_ms += osd_period_ms;

        read_system_msg();
        time_t now_sec = time(NULL);

        bool have_info = false;
        if(info_buf_valid){
            if(load_info_buffer()){
                last_info_attempt = now_sec;
                have_info = true;
            } else {
                info_buf_valid=false;
                last_info_attempt = now_sec;
            }
        } else if(now_sec - last_info_attempt >= 3){
            if(load_info_buffer()){
                info_buf_valid=true;
                have_info = true;
            }
            last_info_attempt = now_sec;
        }

        if(!have_info || !info_buf){
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

    }
}
