/**
 * ps4_crsf.c  –  250 Hz PS4-pad ➜ 16-channel CRSF streamer / simulator
 *
 * 2025-06-20  • Default UART speed changed to **115 200 baud** +
 *               new `--baud` flag (any standard rate).
 *             • If `--stats` is ON, we now read back any ASCII telemetry
 *               the device prints on the same TTY and echo it to stdout.
 *
 * Build
 * ─────
 *   sudo apt-get install libsdl2-dev
 *   gcc -O2 -Wall -o ps4_crsf \
 *       ps4_crsf.c $(pkg-config --cflags --libs sdl2)
 *
 * Common use
 * ──────────
 *   ./ps4_crsf -d /dev/ttyUSB0 --baud 115200  -r 125        # quiet
 *   ./ps4_crsf --simulation --channels -r 50                # no UART
 *   ./ps4_crsf --stats --mode 1,2,3,5,4 --invert 5          # print timings + RX stats
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <SDL2/SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ───── protocol / timing ───── */
#define LOOP_HZ            250
#define LOOP_NS            4000000L               /* 4 ms  */

#define CRSF_DEST          0xC8
#define CRSF_TYPE_CHANNELS 0x16
#define CRSF_PAYLOAD_LEN   22                      /* 16×11-bit */
#define CRSF_FRAME_LEN     24

/* ───── globals ───── */
static volatile int g_run = 1;
static void on_sigint(int s){ (void)s; g_run = 0; }

/* ───── util: CRC-8 (Dallas/Maxim) ───── */
static uint8_t crc8(const uint8_t *d, size_t n)
{
    uint8_t c = 0;
    while (n--){
        uint8_t in = *d++;
        for (int i=0;i<8;i++){
            uint8_t mix = (c ^ in) & 1;
            c >>= 1;
            if (mix) c ^= 0x8C;
            in >>= 1;
        }
    }
    return c;
}

/* pack 16×11-bit → 22-byte payload (little-endian bits) */
static void pack_channels(const uint16_t ch[16], uint8_t out[CRSF_PAYLOAD_LEN])
{
    memset(out,0,CRSF_PAYLOAD_LEN);
    uint32_t bit=0;
    for(int i=0;i<16;i++){
        uint32_t b=bit>>3, off=bit&7, v=ch[i]&0x7FF;
        out[b]   |= v<<off;
        out[b+1] |= v>>(8-off);
        out[b+2] |= v>>(16-off);
        bit+=11;
    }
}

/* baud helper -------------------------------------------------------- */
static speed_t baud_const(int baud){
    switch(baud){
        case   9600: return B9600;
        case  19200: return B19200;
        case  38400: return B38400;
        case  57600: return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
#ifdef B400000
        case 400000: return B400000;
#endif
        default:     return 0;
    }
}

/* open serial raw 8N1 at selected baud (defaults to 115 200) */
static int open_serial(const char *dev, int baud)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
    if (fd < 0){ perror("serial"); return -1; }

    struct termios t; tcgetattr(fd,&t); cfmakeraw(&t);
    speed_t sp = baud_const(baud);
    if(!sp){ fprintf(stderr,"Unsupported baud %d, falling back 115200\n",baud); sp=B115200; }
    cfsetspeed(&t, sp);
    t.c_cflag |= CLOCAL | CREAD;
    if(tcsetattr(fd,TCSANOW,&t)<0){ perror("tcsetattr"); close(fd); return -1;}
    return fd;
}

static void try_rt(int prio){
    struct sched_param sp={.sched_priority=prio};
    if(!sched_setscheduler(0,SCHED_FIFO,&sp))
        fprintf(stderr,"◎ SCHED_FIFO %d\n",prio);
}

/* axis −32768…32767 ➜ 172…1811 (≈1000±839) */
static inline uint16_t scale_axis(int32_t v){
    return (uint16_t)(992 + (v/32767.0f)*660);
}
static inline int32_t clip_dead(int32_t v,int thr){
    return (thr>0 && v>-thr && v<thr) ? 0 : v;
}

/* ───── joystick ➜ 16-channel arrays ───── */
static void build_channels(SDL_Joystick *js,const int dead[16],
                           uint16_t ch_s[16],int32_t ch_r[16])
{
    /* sticks */
    ch_r[0]=SDL_JoystickGetAxis(js,0);
    ch_r[1]=SDL_JoystickGetAxis(js,1);
    ch_r[2]=SDL_JoystickGetAxis(js,2);
    ch_r[3]=SDL_JoystickGetAxis(js,5);
    for(int i=0;i<4;i++) ch_r[i]=clip_dead(ch_r[i],dead[i]);
    ch_s[0]=scale_axis(ch_r[0]);
    ch_s[1]=scale_axis(-ch_r[1]);
    ch_s[2]=scale_axis(ch_r[2]);
    ch_s[3]=scale_axis(-ch_r[3]);

    /* triggers */
    ch_r[4]=clip_dead(SDL_JoystickGetAxis(js,3),dead[4]);
    ch_r[5]=clip_dead(SDL_JoystickGetAxis(js,4),dead[5]);
    ch_s[4]=scale_axis(ch_r[4]);
    ch_s[5]=scale_axis(ch_r[5]);

    /* D-pad */
    int dpx=0,dpy=0;
    if(SDL_JoystickNumHats(js)>0){
        uint8_t h=SDL_JoystickGetHat(js,0);
        dpx=(h&SDL_HAT_RIGHT)?1:(h&SDL_HAT_LEFT)?-1:0;
        dpy=(h&SDL_HAT_UP)?1:(h&SDL_HAT_DOWN)?-1:0;
    }else if(SDL_JoystickNumAxes(js)>=8){
        dpx=SDL_JoystickGetAxis(js,6)/32767;
        dpy=-SDL_JoystickGetAxis(js,7)/32767;
    }else if(SDL_JoystickNumButtons(js)>=15){
        dpy=SDL_JoystickGetButton(js,11)?1:SDL_JoystickGetButton(js,12)?-1:0;
        dpx=SDL_JoystickGetButton(js,13)?-1:SDL_JoystickGetButton(js,14)? 1:0;
    }
    ch_r[6]=dpx; ch_r[7]=dpy;
    ch_s[6]=992+dpx*660;
    ch_s[7]=992+dpy*660;

    /* buttons */
    for(int i=8;i<16;i++){
        int b=SDL_JoystickGetButton(js,i-8);
        ch_r[i]=b;
        ch_s[i]=b?1811:172;
    }
}

/* ───── list parsers ───── */
static void parse_list(const char *str,int out[16],int identity)
{
    for(int i=0;i<16;i++) out[i]=identity?i:0;
    if(!str) return;
    char *dup=strdup(str),*tok,*save;
    for(int idx=0;(tok=strtok_r(idx?NULL:dup,",",&save))&&idx<16;idx++){
        int v=atoi(tok); if(v>=1&&v<=16) out[idx]=v-1;
    }
    free(dup);
}
static void parse_invert(const char *str,int inv[16]){
    for(int i=0;i<16;i++) inv[i]=0;
    if(!str) return;
    char *dup=strdup(str),*tok,*save;
    for(tok=strtok_r(dup,",",&save);tok;tok=strtok_r(NULL,",",&save)){
        int ch=atoi(tok); if(ch>=1&&ch<=16) inv[ch-1]=1;
    }
    free(dup);
}

/* ───── main ───── */
int main(int argc,char **argv)
{
    const char *dev="/dev/ttyUSB0";
    int baud=115200, rate=125;
    int F_stats=0,F_sim=0,F_chan=0;
    const char *mode_str=NULL,*inv_str=NULL,*dead_str=NULL;
    int map[16],inv[16],dead[16];

    static const struct option L[]={
        {"device",required_argument,0,'d'},
        {"baud",required_argument,0,'u'},
        {"rate",required_argument,0,'r'},
        {"mode",required_argument,0,'m'},
        {"invert",required_argument,0,'i'},
        {"deadband",required_argument,0,'b'},
        {"stats",no_argument,0,1},
        {"simulation",no_argument,0,2},
        {"channels",no_argument,0,3},
        {0,0,0,0}
    };
    int o,idx;
    while((o=getopt_long(argc,argv,"d:u:r:m:i:b:",L,&idx))!=-1){
        switch(o){
        case 'd': dev=optarg; break;
        case 'u': baud=atoi(optarg); break;
        case 'r': rate=atoi(optarg); break;
        case 'm': mode_str=optarg; break;
        case 'i': inv_str=optarg;  break;
        case 'b': dead_str=optarg; break;
        case 1:   F_stats=1; break;
        case 2:   F_sim=1; break;
        case 3:   F_chan=1; break;
        default:
            fprintf(stderr,
 "Usage: %s [-d tty] [--baud N] [-r 50|125|250] "
 "[--mode list] [--invert list] [--deadband list] "
 "[--stats] [--channels] [--simulation]\n",argv[0]); return 1;
        }
    }
    if(rate!=50&&rate!=125&&rate!=250){ fprintf(stderr,"rate must be 50,125,250\n");return 1;}

    parse_list(mode_str,map,1);
    parse_invert(inv_str,inv);
    for(int i=0;i<16;i++) dead[i]=0;
    if(dead_str){
        char *dup=strdup(dead_str),*tok,*save;
        for(int i=0;(tok=strtok_r(i?NULL:dup,",",&save))&&i<16;i++)
            dead[i]=abs(atoi(tok));
        free(dup);
    }

    int fd=-1;
    if(!F_sim){
        fd=open_serial(dev,baud); if(fd<0) return 1;
    }

    if(SDL_Init(SDL_INIT_JOYSTICK)<0){ fprintf(stderr,"SDL: %s\n",SDL_GetError()); return 1;}
    SDL_Joystick *js=SDL_NumJoysticks()?SDL_JoystickOpen(0):NULL;
    if(!js){ fprintf(stderr,"No joystick.\n"); return 1;}

    try_rt(10);
    signal(SIGINT,on_sigint);

    struct timespec next; clock_gettime(CLOCK_MONOTONIC,&next);
    const uint64_t every=LOOP_HZ/rate;

    double t_min=1e9,t_max=0,t_sum=0; uint64_t t_cnt=0,loops=0;

    uint8_t frame[CRSF_FRAME_LEN+2];
    frame[0]=CRSF_DEST; frame[1]=CRSF_FRAME_LEN; frame[2]=CRSF_TYPE_CHANNELS;

    char rxbuf[256]; size_t rxlen=0;

    while(g_run){
        SDL_JoystickUpdate();

        uint16_t ch_s[16],ch_out[16];
        int32_t  ch_r[16],raw_out[16];
        build_channels(js,dead,ch_s,ch_r);

        for(int i=0;i<16;i++){
            int src=map[i];
            uint16_t v=ch_s[src];
            if(inv[i]) v=(uint16_t)(1983-v);
            ch_out[i]=v;
            raw_out[i]=ch_r[src];
        }

        if(loops%every==0){
            pack_channels(ch_out,frame+3);
            frame[CRSF_FRAME_LEN+1]=crc8(frame+2,CRSF_FRAME_LEN-1);

            if(F_chan){
                printf("CH:");  for(int i=0;i<16;i++) printf(" %4u",ch_out[i]);
                printf(" | RAW:"); for(int i=0;i<16;i++) printf(" %6d",raw_out[i]);
                putchar('\n');
            }
            if(!F_sim) write(fd,frame,CRSF_FRAME_LEN+2);
        }
        ++loops;

        /* read back any ASCII telemetry (non-blocking) -------------- */
        if(F_stats && !F_sim){
            char tmp[64];
            ssize_t n;
            while((n=read(fd,tmp,sizeof(tmp)))>0){
                for(ssize_t i=0;i<n;i++){
                    if(rxlen<sizeof(rxbuf)-1) rxbuf[rxlen++]=tmp[i];
                    if(tmp[i]=='\n'){ rxbuf[rxlen]=0; fputs(rxbuf,stdout); rxlen=0; }
                }
            }
        }

        /* loop-timing stats ----------------------------------------- */
        if(F_stats){
            struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
            double dt=(now.tv_sec-next.tv_sec)+(now.tv_nsec-next.tv_nsec)/1e9;
            if(dt>0){ if(dt<t_min)t_min=dt; if(dt>t_max)t_max=dt; t_sum+=dt; ++t_cnt;
                if(t_cnt>=LOOP_HZ){
                    printf("loop min %.3f  max %.3f  avg %.3f ms\n",
                           t_min*1e3,t_max*1e3,(t_sum/t_cnt)*1e3);
                    t_min=1e9;t_max=0;t_sum=0;t_cnt=0;
                }}
        }

        /* wait next 4 ms boundary ----------------------------------- */
        next.tv_nsec+=LOOP_NS;
        if(next.tv_nsec>=1000000000L){ next.tv_nsec-=1000000000L; ++next.tv_sec; }
        clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&next,NULL);
    }

    if(fd>=0) close(fd);
    if(js) SDL_JoystickClose(js);
    SDL_Quit();
    return 0;
}
