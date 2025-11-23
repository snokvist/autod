/*
 * antenna_osd.c  (v3.1 — 2025-08-20)
 * - Remove embedded HTTP server and ICMP ping helpers
 * - Retain periodic OSD rendering with smoothed RSSI values
 *
 * antenna_osd.c  (v2.x — previous)
 * - RSSI bar OSD, UDP/RSSI optional 2nd bar, system msg timeout, globbed input
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <glob.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_INFO_SOURCES 2

static int last_valid_rssi=0, neg1_count_rssi=0, last_valid_rssi2=0, neg1_count_rssi2=0;
static char *info_buf[MAX_INFO_SOURCES]={NULL,NULL};
static size_t info_size[MAX_INFO_SOURCES]={0,0};
static time_t last_info_attempt[MAX_INFO_SOURCES]={0,0};
static bool info_buf_valid[MAX_INFO_SOURCES]={false,false};
static int rssi_hist[3]={-1,-1,-1}, rssi2_hist[3]={-1,-1,-1};

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
#define DEF_RSSI_2_ENABLE  0
#define DEF_RSSI_2_KEY     "rssi_2"
#define DEF_TX_POWER_KEY     "txpwr"

static inline int64_t now_ms(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

static const char *FULL="\u2588";
static const char *PART[7]={"\u2581","\u2582","\u2583","\u2584","\u2585","\u2586","\u2587"};

typedef struct {
    const char *info_files[MAX_INFO_SOURCES];
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
    bool        rssi2_enable;
    const char *rssi2_key;
    const char *tx_power_key;
} cfg_t;

static const cfg_t cfg_default = {
    .info_files={DEF_INFO_FILE,NULL}, .out_file=DEF_OUT_FILE, .interval=DEF_INTERVAL, .bar_width=DEF_BAR_WIDTH, .top=DEF_TOP, .bottom=DEF_BOTTOM,
    .osd_hdr=DEF_OSD_HDR, .osd_hdr2=DEF_OSD_HDR2, .sys_msg_hdr=DEF_SYS_MSG_HDR, .system_msg="", .show_stats_line=DEF_SHOW_STATS, .sys_msg_timeout=DEF_SYS_MSG_TIMEOUT,
    .rssi_control=DEF_RSSI_CONTROL, .rssi_hdr={DEF_RSSI_RANGE0,DEF_RSSI_RANGE1,DEF_RSSI_RANGE2,DEF_RSSI_RANGE3,DEF_RSSI_RANGE4,DEF_RSSI_RANGE5},
    .start_sym=DEF_START, .end_sym=DEF_END, .empty_sym=DEF_EMPTY, .rssi_key=DEF_RSSI_KEY,
    .curr_tx_rate_key=DEF_CURR_TX_RATE_KEY, .curr_tx_bw_key=DEF_CURR_TX_BW_KEY, .rssi2_enable=DEF_RSSI_2_ENABLE, .rssi2_key=DEF_RSSI_2_KEY, .tx_power_key=DEF_TX_POWER_KEY
};

static cfg_t cfg;
static volatile sig_atomic_t reload_requested = 0;
static const char *config_path = DEF_CFG_FILE;

static void reset_info_buffers(void)
{
    for (int i = 0; i < MAX_INFO_SOURCES; ++i) {
        free(info_buf[i]);
        info_buf[i] = NULL;
        info_size[i] = 0;
        info_buf_valid[i] = false;
        last_info_attempt[i] = 0;
    }
}

static void free_config_strings(void)
{
    for (int i = 0; i < MAX_INFO_SOURCES; ++i) {
        if (cfg.info_files[i] && cfg.info_files[i] != cfg_default.info_files[i]) {
            free((void *)cfg.info_files[i]);
        }
    }
    if (cfg.out_file && cfg.out_file != cfg_default.out_file) free((void *)cfg.out_file);
    if (cfg.osd_hdr && cfg.osd_hdr != cfg_default.osd_hdr) free((void *)cfg.osd_hdr);
    if (cfg.osd_hdr2 && cfg.osd_hdr2 != cfg_default.osd_hdr2) free((void *)cfg.osd_hdr2);
    if (cfg.sys_msg_hdr && cfg.sys_msg_hdr != cfg_default.sys_msg_hdr) free((void *)cfg.sys_msg_hdr);
    if (cfg.start_sym && cfg.start_sym != cfg_default.start_sym) free((void *)cfg.start_sym);
    if (cfg.end_sym && cfg.end_sym != cfg_default.end_sym) free((void *)cfg.end_sym);
    if (cfg.empty_sym && cfg.empty_sym != cfg_default.empty_sym) free((void *)cfg.empty_sym);
    for (int i = 0; i < 6; ++i) {
        if (cfg.rssi_hdr[i] && cfg.rssi_hdr[i] != cfg_default.rssi_hdr[i]) {
            free((void *)cfg.rssi_hdr[i]);
        }
    }
    if (cfg.rssi_key && cfg.rssi_key != cfg_default.rssi_key) free((void *)cfg.rssi_key);
    if (cfg.curr_tx_rate_key && cfg.curr_tx_rate_key != cfg_default.curr_tx_rate_key) free((void *)cfg.curr_tx_rate_key);
    if (cfg.curr_tx_bw_key && cfg.curr_tx_bw_key != cfg_default.curr_tx_bw_key) free((void *)cfg.curr_tx_bw_key);
    if (cfg.rssi2_key && cfg.rssi2_key != cfg_default.rssi2_key) free((void *)cfg.rssi2_key);
    if (cfg.tx_power_key && cfg.tx_power_key != cfg_default.tx_power_key) free((void *)cfg.tx_power_key);
}

static void reset_config_defaults(void)
{
    free_config_strings();
    cfg = cfg_default;
}

static void request_reload(int sig)
{
    if (sig == SIGHUP) reload_requested = 1;
}

static void set_cfg_string(const char **field, const char *value, const char *default_value)
{
    char *dup = strdup(value);
    if (!dup) return;
    if (*field && *field != default_value) free((void *)*field);
    *field = dup;
}

static void set_cfg_field(const char *k, const char *v)
{
#define EQ(a,b) (strcmp((a),(b))==0)
    if (EQ(k, "info_file") || EQ(k, "telemetry_file") || EQ(k, "telemetry_primary")) { set_cfg_string(&cfg.info_files[0], v, cfg_default.info_files[0]); info_buf_valid[0] = false; last_info_attempt[0] = 0; }
    else if (EQ(k, "info_file2") || EQ(k, "info_file_alt") || EQ(k, "info_file_secondary") || EQ(k, "telemetry_file2") || EQ(k, "telemetry_secondary") || EQ(k, "telemetry_alt")) { set_cfg_string(&cfg.info_files[1], v, cfg_default.info_files[1]); info_buf_valid[1] = false; last_info_attempt[1] = 0; }
    else if (EQ(k, "out_file")) set_cfg_string(&cfg.out_file, v, cfg_default.out_file);
    else if (EQ(k, "interval")) cfg.interval = atof(v);
    else if (EQ(k, "bar_width")) cfg.bar_width = atoi(v);
    else if (EQ(k, "top")) cfg.top = atoi(v);
    else if (EQ(k, "bottom")) cfg.bottom = atoi(v);
    else if (EQ(k, "osd_hdr")) set_cfg_string(&cfg.osd_hdr, v, cfg_default.osd_hdr);
    else if (EQ(k, "osd_hdr2")) set_cfg_string(&cfg.osd_hdr2, v, cfg_default.osd_hdr2);
    else if (EQ(k, "sys_msg_hdr")) set_cfg_string(&cfg.sys_msg_hdr, v, cfg_default.sys_msg_hdr);
    else if (EQ(k, "show_stats_line") || EQ(k, "stats_line_mode")) {
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
    else if (EQ(k, "rssi_range0_hdr")) set_cfg_string(&cfg.rssi_hdr[0], v, cfg_default.rssi_hdr[0]);
    else if (EQ(k, "rssi_range1_hdr")) set_cfg_string(&cfg.rssi_hdr[1], v, cfg_default.rssi_hdr[1]);
    else if (EQ(k, "rssi_range2_hdr")) set_cfg_string(&cfg.rssi_hdr[2], v, cfg_default.rssi_hdr[2]);
    else if (EQ(k, "rssi_range3_hdr")) set_cfg_string(&cfg.rssi_hdr[3], v, cfg_default.rssi_hdr[3]);
    else if (EQ(k, "rssi_range4_hdr")) set_cfg_string(&cfg.rssi_hdr[4], v, cfg_default.rssi_hdr[4]);
    else if (EQ(k, "rssi_range5_hdr")) set_cfg_string(&cfg.rssi_hdr[5], v, cfg_default.rssi_hdr[5]);
    else if (EQ(k, "start_sym")) set_cfg_string(&cfg.start_sym, v, cfg_default.start_sym);
    else if (EQ(k, "end_sym")) set_cfg_string(&cfg.end_sym, v, cfg_default.end_sym);
    else if (EQ(k, "empty_sym")) set_cfg_string(&cfg.empty_sym, v, cfg_default.empty_sym);
    else if (EQ(k, "rssi_key") || EQ(k, "signal_key") || EQ(k, "signal_strength_key")) set_cfg_string(&cfg.rssi_key, v, cfg_default.rssi_key);
    else if (EQ(k, "curr_tx_rate_key") || EQ(k, "stats_mcs_key") || EQ(k, "stats_rate_key")) set_cfg_string(&cfg.curr_tx_rate_key, v, cfg_default.curr_tx_rate_key);
    else if (EQ(k, "curr_tx_bw_key") || EQ(k, "stats_bw_key") || EQ(k, "stats_bandwidth_key")) set_cfg_string(&cfg.curr_tx_bw_key, v, cfg_default.curr_tx_bw_key);
    else if (EQ(k, "rssi_2_enable") || EQ(k, "secondary_rssi_enable") || EQ(k, "alt_rssi_enable")) cfg.rssi2_enable = atoi(v) != 0;
    else if (EQ(k, "rssi_2_key") || EQ(k, "secondary_rssi_key") || EQ(k, "alt_rssi_key")) set_cfg_string(&cfg.rssi2_key, v, cfg_default.rssi2_key);
    else if (EQ(k, "tx_power_key") || EQ(k, "stats_tx_power_key") || EQ(k, "stats_txpwr_key")) set_cfg_string(&cfg.tx_power_key, v, cfg_default.tx_power_key);
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

static int get_display_rssi2(int raw){
    if(raw>=0){last_valid_rssi2=raw; neg1_count_rssi2=0; return raw;}
    if(++neg1_count_rssi2>=3) return -1;
    return last_valid_rssi2;
}

static FILE *fopen_glob_first(const char *pattern,const char *mode){
    if(!strpbrk(pattern,"*?[]")) return fopen(pattern,mode);
    glob_t g; if(glob(pattern,GLOB_NOSORT,NULL,&g)!=0||g.gl_pathc==0){globfree(&g); return NULL;}
    FILE *fp=fopen(g.gl_pathv[0],mode); globfree(&g); return fp;
}

static bool load_info_buffer(int idx){
    if(idx < 0 || idx >= MAX_INFO_SOURCES) return false;
    const char *path = cfg.info_files[idx];
    if(!path) return false;
    FILE *fp=fopen_glob_first(path,"r"); if(!fp) return false;
    free(info_buf[idx]); info_buf[idx]=NULL; info_size[idx]=0;
    size_t cap=0; char tmp[256];
    while(fgets(tmp,sizeof(tmp),fp)){
        size_t len=strlen(tmp);
        if(info_size[idx]+len+1>cap){
            cap=(cap+len+1)*2;
            char *nb=realloc(info_buf[idx],cap);
            if(!nb){free(info_buf[idx]); info_buf[idx]=NULL; fclose(fp); return false;}
            info_buf[idx]=nb;
        }
        memcpy(info_buf[idx]+info_size[idx],tmp,len); info_size[idx]+=len;
    }
    if(info_buf[idx]) info_buf[idx][info_size[idx]]='\0';
    else {
        info_buf[idx]=strdup("");
        if(!info_buf[idx]){fclose(fp); return false;}
        info_size[idx]=0;
    }
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

static int resolve_source_from_spec(const char *spec,const char **key_out){
    if(!spec){
        *key_out=NULL;
        return 0;
    }
    const char *colon=strchr(spec,':');
    if(!colon||colon==spec){
        *key_out=spec;
        return 0;
    }
    size_t prefix_len=(size_t)(colon-spec);
    if(prefix_len>=16){
        *key_out=spec;
        return 0;
    }
    char prefix[16];
    for(size_t i=0;i<prefix_len;++i){
        prefix[i]=(char)tolower((unsigned char)spec[i]);
    }
    prefix[prefix_len]='\0';
    int idx=-1;
    if(strcmp(prefix,"file1")==0||strcmp(prefix,"info1")==0||strcmp(prefix,"primary")==0||strcmp(prefix,"main")==0||strcmp(prefix,"0")==0||strcmp(prefix,"1")==0){
        idx=0;
    } else if(strcmp(prefix,"file2")==0||strcmp(prefix,"info2")==0||strcmp(prefix,"secondary")==0||strcmp(prefix,"alt")==0||strcmp(prefix,"2")==0){
        idx=1;
    }
    if(idx>=0){
        *key_out=colon+1;
        return idx;
    }
    *key_out=spec;
    return 0;
}

static int parse_int_from_spec(const char *spec,const bool have_info[]){
    const char *key=NULL;
    int idx=resolve_source_from_spec(spec,&key);
    if(idx<0||idx>=MAX_INFO_SOURCES) idx=0;
    if(!key||!*key) return -1;
    if(!have_info[idx]||!info_buf[idx]) return -1;
    return parse_int_from_buf(info_buf[idx],key);
}

static void parse_value_from_spec(const char *spec,const bool have_info[],char *out,size_t outlen){
    const char *key=NULL;
    int idx=resolve_source_from_spec(spec,&key);
    if(idx<0||idx>=MAX_INFO_SOURCES) idx=0;
    if(!key||!*key||!have_info[idx]||!info_buf[idx]){
        snprintf(out,outlen,"NA");
        return;
    }
    parse_value_from_buf(info_buf[idx],key,out,outlen);
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

static void write_osd(int rssi,int rssi2,const char *mcs_str,const char *bw_str,const char *tx_str){
    int pct; if(rssi<0)pct=0; else if(rssi<=cfg.bottom)pct=0; else if(rssi>=cfg.top)pct=100; else pct=(rssi-cfg.bottom)*100/(cfg.top-cfg.bottom);
    char bar[cfg.bar_width*3+1]; build_bar(bar,sizeof(bar),pct); const char *hdr=choose_rssi_hdr(pct);
    int pct_rssi2=0; char bar_rssi2[cfg.bar_width*3+1]; const char *hdr_rssi2=NULL;
    if(cfg.rssi2_enable){
        int disp_rssi2=rssi2;
        if(disp_rssi2<0)pct_rssi2=0; else if(disp_rssi2<=cfg.bottom)pct_rssi2=0; else if(disp_rssi2>=cfg.top)pct_rssi2=100; else pct_rssi2=(disp_rssi2-cfg.bottom)*100/(cfg.top-cfg.bottom);
        build_bar(bar_rssi2,sizeof(bar_rssi2),pct_rssi2); hdr_rssi2=choose_rssi_hdr(pct_rssi2);
    }
    char filebuf[2048]; int flen=0;
    flen+=snprintf(filebuf+flen,sizeof(filebuf)-flen,"%s %3d%% %s%s%s\n",hdr,pct,cfg.start_sym,bar,cfg.end_sym);
    if(cfg.rssi2_enable) flen+=snprintf(filebuf+flen,sizeof(filebuf)-flen,"%s %3d%% %s%s%s\n",hdr_rssi2,pct_rssi2,cfg.start_sym,bar_rssi2,cfg.end_sym);
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
                            "TEMP: &TC | CPU: &C | %s / %s / %s | &B%s\n",
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

    config_path = cfg_path;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = request_reload;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGHUP, &sa, NULL);

    reset_config_defaults();
    load_config(config_path);
    reset_info_buffers();

    if (cfg.interval < 0.02) cfg.interval = 0.02;
    int64_t osd_period_ms = (int64_t)(cfg.interval*1000.0 + 0.5);
    if (osd_period_ms < 20) osd_period_ms = 20;

    int64_t next_osd_ms = now_ms();
    char last_mcs[32]="NA", last_bw[32]="NA", last_tx[32]="NA";

    for(;;){
        if (reload_requested) {
            reload_requested = 0;
            reset_config_defaults();
            load_config(config_path);
            reset_info_buffers();
            sys_msg_last_update = 0;
            if (cfg.interval < 0.02) cfg.interval = 0.02;
            osd_period_ms = (int64_t)(cfg.interval*1000.0 + 0.5);
            if (osd_period_ms < 20) osd_period_ms = 20;
            next_osd_ms = now_ms();
            strcpy(last_mcs,"NA");
            strcpy(last_bw,"NA");
            strcpy(last_tx,"NA");
        }

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

        bool have_info[MAX_INFO_SOURCES]={false,false};
        bool any_info=false;
        for(int i=0;i<MAX_INFO_SOURCES;++i){
            if(!cfg.info_files[i]) continue;
            if(info_buf_valid[i]){
                if(load_info_buffer(i)){
                    last_info_attempt[i]=now_sec;
                    have_info[i]=true;
                } else {
                    info_buf_valid[i]=false;
                    last_info_attempt[i]=now_sec;
                }
            } else if(now_sec - last_info_attempt[i] >= 3){
                if(load_info_buffer(i)){
                    info_buf_valid[i]=true;
                    have_info[i]=true;
                }
                last_info_attempt[i]=now_sec;
            }
            if(have_info[i]) any_info=true;
        }

        if(!any_info){
            strcpy(last_mcs,"NA"); strcpy(last_bw,"NA"); strcpy(last_tx,"NA");
            smooth_rssi_sample(rssi_hist, get_display_rssi(-1));
            if (cfg.rssi2_enable) smooth_rssi_sample(rssi2_hist, get_display_rssi2(-1));
            continue;
        } else {
            int raw_rssi = parse_int_from_spec(cfg.rssi_key,have_info);
            int raw_rssi2  = cfg.rssi2_enable ? parse_int_from_spec(cfg.rssi2_key,have_info) : -1;

            int disp_rssi = get_display_rssi(raw_rssi); disp_rssi = smooth_rssi_sample(rssi_hist, disp_rssi);
            int disp_rssi2  = get_display_rssi2(raw_rssi2);   disp_rssi2  = smooth_rssi_sample(rssi2_hist,  disp_rssi2);

            parse_value_from_spec(cfg.curr_tx_rate_key,have_info,last_mcs,sizeof(last_mcs));
            parse_value_from_spec(cfg.curr_tx_bw_key,  have_info,last_bw, sizeof(last_bw));
            parse_value_from_spec(cfg.tx_power_key,    have_info,last_tx, sizeof(last_tx));

            write_osd(disp_rssi, disp_rssi2, last_mcs, last_bw, last_tx);
        }

    }
}
