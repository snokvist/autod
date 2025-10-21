// crsf_dump_net3.c
// UART (420000 8N1 via termios2) + UDP CRSF parser.
// Adds per-source CRC XOROUT support (--crc-xor 0xNN) and auto-learn when a constant XOR is detected.
//
// Build:  gcc -O2 -Wall crsf_dump_net3.c -o crsf_dump
// Usage:  ./crsf_dump [--hex] [--stats] [--udp-port N] [--udp-bind IP] [--reuseport]
//                    [--udp-crsf-offset N] [--crc-xor 0xNN] [--no-uart] /dev/ttyS2
//
// Example (UDP only):
//   ./crsf_dump --no-uart --udp-port 14550 --udp-bind 192.168.2.242 --reuseport --hex --stats
//
// Notes:
// - If your bridge XORs the CRC byte (like your sample: XOR with 0x88), either pass --crc-xor 0x88
//   or rely on auto-learn (it will detect a stable XOR and adopt it).
// - Prints source prefixes ("UART" or "UDP a.b.c.d:port").
// - Decodes RC (0x16) to ~µs; prints other frames as hex if --hex.
// - Per-source stats split by UART/UDP and show CRC fails.
//
// © CC0 / Public Domain.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <asm/termbits.h>
#ifndef TCGETS2
#define TCGETS2 _IOR('T', 0x2A, struct termios2)
#endif
#ifndef TCSETS2
#define TCSETS2 _IOW('T', 0x2B, struct termios2)
#endif

// ===== CRSF =====
#define CRSF_MAX_FRAME 64
#define CRSF_TYPE_RC_CHANNELS 0x16

static volatile int g_stop = 0;
static int g_show_hex = 0;
static int g_show_stats = 0;

// ===== Signal =====
static void on_sigint(int s){ (void)s; g_stop = 1; }

// ===== Serial config (420000 8N1 RAW via termios2) =====
static int set_serial_420k(int fd){
    struct termios2 tio;
    if (ioctl(fd, TCGETS2, &tio) != 0){ perror("TCGETS2"); return -1; }
    tio.c_cflag &= ~(CBAUD | PARENB | CSTOPB | CRTSCTS);
    tio.c_cflag |= (BOTHER | CS8 | CREAD | CLOCAL);
    tio.c_iflag = 0; tio.c_oflag = 0; tio.c_lflag = 0;
    tio.c_ispeed = 420000; tio.c_ospeed = 420000;
    tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 0;
    if (ioctl(fd, TCSETS2, &tio) != 0){ perror("TCSETS2"); return -1; }
    return 0;
}

// ===== CRC-8 DVB-S2 =====
static uint8_t crc8_dvb_s2(const uint8_t *data, int len){
    uint8_t crc = 0;
    for (int i=0;i<len;i++){
        crc ^= data[i];
        for (int b=0;b<8;b++){
            if (crc & 0x80) crc = (uint8_t)((crc<<1) ^ 0xD5);
            else crc <<= 1;
        }
    }
    return crc;
}

// ===== RC unpack =====
static int unpack_11bit(const uint8_t *p, int plen, uint16_t ch[16]){
    if (plen < 22) return 0;
    int bits=0; unsigned acc=0; int out=0;
    for (int i=0;i<22;i++){
        acc |= ((unsigned)p[i]) << bits;
        bits += 8;
        while(bits >= 11 && out < 16){
            ch[out++] = (uint16_t)(acc & 0x7FF);
            acc >>= 11; bits -= 11;
        }
    }
    return out;
}

static void print_hex(const uint8_t *p, int n){
    for (int i=0;i<n;i++){
        printf("%02X", p[i]);
        if (i+1<n) putchar(' ');
    }
}

static void print_rc(const char *src, const uint8_t *payload, int plen){
    uint16_t ch[16]={0};
    int n = unpack_11bit(payload, plen, ch);
    int us[16];
    for (int i=0;i<n;i++) us[i] = (int)(ch[i] * 0.624 + 880);
    printf("%s RC:", src);
    for (int i=0;i<n && i<8;i++) printf(" %d", us[i]);
    if (n>8) printf(" …");
    printf("\n"); fflush(stdout);
}

// ===== Stats =====
struct stat_item { unsigned cnt; unsigned bytes; unsigned crc_fail; };
struct source_stats {
    struct stat_item per_type[256];
    unsigned frames_ok;
    unsigned packets;
    unsigned bytes_total;
    // CRC XOR handling
    int      crc_xor_forced;  // -1 none; else 0..255
    int      crc_xor_auto;    // -1 none; else 0..255
    int      xor_candidate;   // last observed delta
    unsigned xor_hits;        // how many times we saw same delta in a row
};

static void reset_stats(struct source_stats *s){
    memset(s, 0, sizeof(*s));
    s->crc_xor_forced = -1;
    s->crc_xor_auto   = -1;
    s->xor_candidate  = -1;
    s->xor_hits       = 0;
}

static void add_stats_ok(struct source_stats *s, uint8_t ftype, int total){
    s->per_type[ftype].cnt++;
    s->per_type[ftype].bytes += (unsigned)total;
    s->frames_ok++;
}

static void add_stats_crc_fail(struct source_stats *s, uint8_t ftype){
    s->per_type[ftype].crc_fail++;
}

static void print_stats_one(const char *label, const struct source_stats *s){
    printf("%s packets=%u bytes=%u frames_ok=%u",
           label, s->packets, s->bytes_total, s->frames_ok);
    if (s->crc_xor_forced >= 0) printf(" crc_xor(forced)=0x%02X", s->crc_xor_forced);
    if (s->crc_xor_auto   >= 0) printf(" crc_xor(auto)=0x%02X",   s->crc_xor_auto);
    for (int t=0;t<256;t++){
        if (s->per_type[t].cnt || s->per_type[t].crc_fail){
            printf(" 0x%02X:ok=%u,crc=%u", t, s->per_type[t].cnt, s->per_type[t].crc_fail);
        }
    }
    printf("\n");
}

// ===== Parser =====
struct parser {
    uint8_t buf[512];
    int have;
    const char *name_prefix; // "UART" or "UDP ip:port"
    struct source_stats *stats;
};

static int crc_ok_with_xor(const struct source_stats *st, const uint8_t *fr){
    const uint8_t len = fr[1];
    const uint8_t rx  = fr[len+1];  // last byte
    uint8_t calc = crc8_dvb_s2(fr+2, len-1);
    int xorv = (st->crc_xor_forced >= 0) ? st->crc_xor_forced
              : (st->crc_xor_auto   >= 0) ? st->crc_xor_auto : 0;
    return (uint8_t)(calc ^ xorv) == rx;
}

static void consider_auto_xor(struct source_stats *st, const uint8_t *fr){
    const uint8_t len = fr[1];
    const uint8_t rx  = fr[len+1];
    uint8_t calc = crc8_dvb_s2(fr+2, len-1);
    uint8_t delta = (uint8_t)(calc ^ rx);

    if (st->crc_xor_forced >= 0 || st->crc_xor_auto >= 0) return; // already decided

    if (st->xor_candidate < 0){
        st->xor_candidate = delta;
        st->xor_hits = 1;
    } else if (st->xor_candidate == delta){
        st->xor_hits++;
        // after a few consistent hits, adopt it
        if (st->xor_hits >= 5 && delta != 0){
            st->crc_xor_auto = delta;
            fprintf(stderr, "[info] adopting CRC XOR 0x%02X for this source\n", delta);
        }
    } else {
        // not stable; reset
        st->xor_candidate = delta;
        st->xor_hits = 1;
    }
}

static void on_frame(struct parser *P, const uint8_t *fr, int total){
    const uint8_t len   = fr[1];
    const uint8_t ftype = fr[2];
    const uint8_t *pl   = fr + 3;
    const int     plen  = len - 2;
    const uint8_t addr  = fr[0]; (void)addr;

    add_stats_ok(P->stats, ftype, total);

    if (ftype == CRSF_TYPE_RC_CHANNELS && plen == 22){
        print_rc(P->name_prefix, pl, plen);
    } else if (g_show_hex) {
        printf("%s FTYPE=0x%02X LEN=%d ADDR=0x%02X : ", P->name_prefix, ftype, plen, fr[0]);
        print_hex(pl, plen);
        printf("\n"); fflush(stdout);
    } else {
        printf("%s FTYPE=0x%02X LEN=%d ADDR=0x%02X\n", P->name_prefix, ftype, plen, fr[0]);
        fflush(stdout);
    }
}

static void feed_parser(struct parser *P, const uint8_t *in, int n){
    if (n <= 0) return;
    if (P->have + n > (int)sizeof(P->buf)) P->have = 0; // clamp on garbage
    memcpy(P->buf + P->have, in, (size_t)n);
    P->have += n;

    int off = 0;
    while (P->have - off >= 2){
        uint8_t len = P->buf[off+1];
        int total = len + 2;
        if (len == 0 || total > CRSF_MAX_FRAME){ off++; continue; }
        if (P->have - off < total) break;

        uint8_t *fr = P->buf + off;

        if (crc_ok_with_xor(P->stats, fr)){
            on_frame(P, fr, total);
        } else {
            consider_auto_xor(P->stats, fr);
            uint8_t ftype = fr[2];
            add_stats_crc_fail(P->stats, ftype);
            if (g_show_hex){
                printf("%s CRC_FAIL t=0x%02X len=%d : ", P->name_prefix, ftype, len-2);
                print_hex(fr, total);
                printf("\n"); fflush(stdout);
            }
        }

        off += total;
    }

    if (off){
        memmove(P->buf, P->buf + off, (size_t)(P->have - off));
        P->have -= off;
    }
}

// ===== UDP =====
static int open_udp_listener(uint16_t port, const char *bind_ip, int reuseport){
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0){ perror("socket"); return -1; }
    int yes = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0){
        perror("setsockopt(SO_REUSEADDR)");
    }
#ifdef SO_REUSEPORT
    if (reuseport){
        if (setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)) != 0){
            perror("setsockopt(SO_REUSEPORT)");
        }
    }
#endif

    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = bind_ip ? inet_addr(bind_ip) : htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0){
        perror("bind");
        close(s); return -1;
    }

    int rcv = 256*1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
    return s;
}

static void udp_src_label(char *out, size_t outsz, const struct sockaddr_in *peer){
    uint32_t ip = ntohl(peer->sin_addr.s_addr);
    unsigned a=(ip>>24)&0xFF, b=(ip>>16)&0xFF, c=(ip>>8)&0xFF, d=ip&0xFF;
    unsigned p = ntohs(peer->sin_port);
    snprintf(out, outsz, "UDP %u.%u.%u.%u:%u", a,b,c,d,p);
}

// ===== Main =====
int main(int argc, char **argv){
    const char *dev = NULL;
    int want_uart = 1;
    int udp_port = 14550;
    const char *udp_bind_ip = NULL;
    int reuseport = 0;
    int udp_offset = 0;
    int force_crc_xor = -1; // -1 = none

    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i],"--hex")) g_show_hex=1;
        else if (!strcmp(argv[i],"--stats")) g_show_stats=1;
        else if (!strcmp(argv[i],"--no-uart")) want_uart=0;
        else if (!strcmp(argv[i],"--udp-port") && i+1<argc) udp_port=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--udp-bind") && i+1<argc) udp_bind_ip=argv[++i];
        else if (!strcmp(argv[i],"--reuseport")) reuseport=1;
        else if (!strcmp(argv[i],"--udp-crsf-offset") && i+1<argc) udp_offset=atoi(argv[++i]);
        else if (!strcmp(argv[i],"--crc-xor") && i+1<argc){
            const char *s = argv[++i];
            force_crc_xor = (int)strtol(s, NULL, 0); // accepts 0xNN or decimal
            if (force_crc_xor < 0 || force_crc_xor > 255){ fprintf(stderr,"bad --crc-xor\n"); return 2; }
        }
        else if (argv[i][0] != '-') dev = argv[i];
        else { fprintf(stderr,"Unknown option: %s\n", argv[i]); return 2; }
    }

    if (want_uart && !dev){
        fprintf(stderr,"Usage: %s [--hex] [--stats] [--udp-port N] [--udp-bind IP] [--reuseport] [--udp-crsf-offset N] [--crc-xor 0xNN] [--no-uart] /dev/ttyS2\n", argv[0]);
        return 2;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int fd_uart = -1;
    if (want_uart){
        fd_uart = open(dev, O_RDONLY | O_NOCTTY);
        if (fd_uart < 0){ perror("open(uart)"); return 1; }
        int flags = fcntl(fd_uart, F_GETFL);
        if (flags >= 0) fcntl(fd_uart, F_SETFL, flags & ~O_NONBLOCK);
        if (set_serial_420k(fd_uart) != 0){
            fprintf(stderr,"Failed to set 420000 baud on %s\n", dev);
            close(fd_uart); return 1;
        }
    }

    int fd_udp = open_udp_listener((uint16_t)udp_port, udp_bind_ip, reuseport);
    if (fd_udp < 0){
        if (!want_uart){
            fprintf(stderr,"UDP open failed and --no-uart set; nothing to do.\n");
            return 1;
        }
        fprintf(stderr,"Warning: UDP listener failed; continuing with UART only.\n");
    } else {
        printf("Listening UDP on %s:%d\n", udp_bind_ip?udp_bind_ip:"0.0.0.0", udp_port);
    }

    struct source_stats stats_uart, stats_udp;
    reset_stats(&stats_uart); reset_stats(&stats_udp);
    if (force_crc_xor >= 0){
        stats_uart.crc_xor_forced = force_crc_xor;
        stats_udp .crc_xor_forced = force_crc_xor;
    }

    struct parser P_uart = { .have=0, .name_prefix="UART", .stats=&stats_uart };
    struct parser P_udp  = { .have=0, .name_prefix="UDP",  .stats=&stats_udp  };

    struct pollfd pfds[2]; int nfds=0;
    if (want_uart){ pfds[nfds].fd = fd_uart; pfds[nfds].events=POLLIN; nfds++; }
    if (fd_udp>=0){ pfds[nfds].fd = fd_udp; pfds[nfds].events=POLLIN; nfds++; }

    uint8_t rbuf[4096];
    time_t t_last = time(NULL);

    while(!g_stop){
        int pr = poll(pfds, nfds, 250);
        if (pr < 0){ if (errno==EINTR) continue; perror("poll"); break; }

        int idx=0;
        if (want_uart && (pfds[idx].revents & POLLIN)){
            int n = (int)read(fd_uart, rbuf, sizeof(rbuf));
            if (n > 0){
                stats_uart.packets++; stats_uart.bytes_total += (unsigned)n;
                feed_parser(&P_uart, rbuf, n);
            }
            idx++;
        } else if (want_uart) idx++;

        if (fd_udp>=0 && (pfds[idx].revents & POLLIN)){
            struct sockaddr_in peer; socklen_t pl=sizeof(peer);
            int n = (int)recvfrom(fd_udp, rbuf, sizeof(rbuf), 0, (struct sockaddr*)&peer, &pl);
            if (n > 0){
                stats_udp.packets++; stats_udp.bytes_total += (unsigned)n;
                // label per datagram
                char label[64]; udp_src_label(label, sizeof(label), &peer);
                P_udp.name_prefix = label;
                if (g_show_hex){
                    printf("%s datagram %d bytes\n", label, n);
                    int m = n<64? n:64; print_hex(rbuf, m); printf("%s\n", (n>m)?" ...":"");
                }
                const uint8_t *p = rbuf; int left = n;
                if (udp_offset > 0 && udp_offset < left){ p += udp_offset; left -= udp_offset; }
                feed_parser(&P_udp, p, left);
                P_udp.name_prefix = "UDP";

                // Adopt auto XOR if discovered
                if (stats_udp.crc_xor_auto >= 0 && force_crc_xor < 0){
                    // nothing to do; already used via crc_ok_with_xor()
                }
            }
        }

        if (g_show_stats){
            time_t now = time(NULL);
            if (now - t_last >= 1){
                if (want_uart) print_stats_one("UART:", &stats_uart);
                if (fd_udp>=0) print_stats_one("UDP :", &stats_udp);
                reset_stats(&stats_uart); reset_stats(&stats_udp);
                if (force_crc_xor >= 0){
                    stats_uart.crc_xor_forced = force_crc_xor;
                    stats_udp .crc_xor_forced = force_crc_xor;
                }
                t_last = now;
            }
        }
    }

    if (want_uart) close(fd_uart);
    if (fd_udp>=0) close(fd_udp);
    return 0;
}
