// Portable UDP counter with monotonic timing and interval stats.
// - Binds 0.0.0.0:14550 by default.
// - Prefers kernel RX timestamps; falls back to CLOCK_MONOTONIC locally.
// - Reports per-interval (default 1000ms): pkts, bytes, sources, pps, kbps,
//   avg Δt, jitter (stdev), min/max Δt, rxq_drops (if available).
//
// Build:  gcc -O2 -Wall -Wextra -std=c11 udp_counter.c -o udp_counter
// Run:    ./udp_counter [-b bind_ip] [-p port] [-i interval_ms]

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// Optional kernel headers. Not all toolchains ship these.
#ifdef __has_include
#  if __has_include(<linux/net_tstamp.h>)
#    include <linux/net_tstamp.h>
#  endif
#  if __has_include(<linux/errqueue.h>)
#    include <linux/errqueue.h>
#  endif
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof((x)[0]))
#endif

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static inline int64_t ts_to_ns(const struct timespec *t) {
    return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}
static inline double ns_to_ms(int64_t ns) { return (double)ns / 1e6; }

static inline struct timespec now_monotonic(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t;
}
static inline int64_t tsdiff_ns(struct timespec a, struct timespec b) {
    return (int64_t)(a.tv_sec - b.tv_sec) * 1000000000LL + (a.tv_nsec - b.tv_nsec);
}

typedef struct {
    uint32_t addr; // IPv4 (network byte order)
    uint16_t port; // network byte order
    bool used;
} src_key_t;

typedef struct {
    uint64_t pkts, bytes;
    uint32_t uniq_sources;
    double   sum_dt_ms, sum_dt2_ms;
    double   min_dt_ms, max_dt_ms;
    uint64_t dt_samples;
    uint64_t rxq_drop_incr;
} interval_stats_t;

static void stats_reset(interval_stats_t *s) {
    memset(s, 0, sizeof(*s));
    s->min_dt_ms = INFINITY;
    s->max_dt_ms = 0.0;
}

static void src_table_clear(src_key_t *t, size_t cap) {
    for (size_t i=0;i<cap;i++) t[i].used = false;
}
static bool src_add_or_seen(src_key_t *t, size_t cap, uint32_t a, uint16_t p) {
    for (size_t i=0;i<cap;i++) if (t[i].used && t[i].addr==a && t[i].port==p) return true;
    for (size_t i=0;i<cap;i++) if (!t[i].used){ t[i].used=true; t[i].addr=a; t[i].port=p; return false; }
    return true; // table full: don't overcount
}

static void print_stats_line(double t_since_start_s, const interval_stats_t *s, uint32_t interval_ms) {
    double avg_dt = s->dt_samples ? (s->sum_dt_ms / (double)s->dt_samples) : 0.0;
    double variance = s->dt_samples ? (s->sum_dt2_ms / (double)s->dt_samples) - (avg_dt*avg_dt) : 0.0;
    if (variance < 0) variance = 0;
    double jitter = sqrt(variance);
    double pps  = (double)s->pkts * 1000.0 / (double)interval_ms;
    double kbps = (double)s->bytes * 8.0 / 1000.0 * (1000.0 / (double)interval_ms);

    printf("[+%8.3fs] pkts=%" PRIu64 ", bytes=%" PRIu64
           ", src=%u, pps=%.1f, kbps=%.1f, avgΔt=%.3f ms, jitter=%.3f ms"
           ", minΔt=%.3f ms, maxΔt=%.3f ms, rxq_drops=%" PRIu64 "\n",
           t_since_start_s, s->pkts, s->bytes, s->uniq_sources,
           pps, kbps, avg_dt, jitter, s->min_dt_ms, s->max_dt_ms, s->rxq_drop_incr);
    fflush(stdout);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-b bind_addr] [-p port] [-i interval_ms]\n"
        "  Default: bind_addr=0.0.0.0, port=14550, interval=1000 ms\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *bind_ip = "0.0.0.0";
    uint16_t port = 14550;
    uint32_t interval_ms = 1000;

    int opt;
    while ((opt = getopt(argc, argv, "b:p:i:h")) != -1) {
        switch (opt) {
            case 'b': bind_ip = optarg; break;
            case 'p': port = (uint16_t)strtoul(optarg, NULL, 10); break;
            case 'i': interval_ms = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'h': default: usage(argv[0]); return (opt=='h')?0:1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif

    int rcvbuf = 4*1024*1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

#ifdef SO_RXQ_OVFL
    setsockopt(fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one));
#endif

    // Timestamp capability detection
    bool use_timestamping = false;
    bool use_timestampns  = false;

#ifdef SO_TIMESTAMPING
    {
        // Build up only flags that exist in your headers.
        int tflags = 0;
#  ifdef SOF_TIMESTAMPING_SOFTWARE
        tflags |= SOF_TIMESTAMPING_SOFTWARE;
#  endif
#  ifdef SOF_TIMESTAMPING_RX_SOFTWARE
        tflags |= SOF_TIMESTAMPING_RX_SOFTWARE;
#  endif
#  ifdef SOF_TIMESTAMPING_MONOTONIC
        tflags |= SOF_TIMESTAMPING_MONOTONIC;
#  endif
        if (tflags != 0) {
            if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &tflags, sizeof(tflags)) == 0) {
                use_timestamping = true;
            }
        }
    }
#endif

#ifndef SO_TIMESTAMPING
    (void)use_timestamping; // silence if not compiled
#endif

#if defined(SO_TIMESTAMPNS)
    if (!use_timestamping) {
        if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one)) == 0) {
            use_timestampns = true; // realtime but high-res
        }
    }
#endif

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", bind_ip);
        return 1;
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    struct pollfd pfd = { .fd = fd, .events = POLLIN };

    interval_stats_t S; stats_reset(&S);
    enum { SRC_CAP = 256 };
    src_key_t srcs[SRC_CAP]; src_table_clear(srcs, SRC_CAP);

    uint64_t last_rxq_count = 0;

    struct timespec t_start = now_monotonic();
    struct timespec t_prev  = t_start;
    struct timespec t_window_end = t_start;
    t_window_end.tv_nsec += (long)interval_ms * 1000000L;
    while (t_window_end.tv_nsec >= 1000000000L) {
        t_window_end.tv_sec += 1; t_window_end.tv_nsec -= 1000000000L;
    }

    char data[65536];
    char cbuf[1024];
    struct sockaddr_in src;
    struct iovec iov = { .iov_base = data, .iov_len = sizeof(data) };

    while (!g_stop) {
        struct timespec t_now = now_monotonic();
        int64_t ms_left = tsdiff_ns(t_window_end, t_now) / 1000000LL;
        if (ms_left < 0) ms_left = 0;

        int pr = poll(&pfd, 1, (int)ms_left);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll"); break;
        }

        if (pr > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                struct msghdr msg; memset(&msg, 0, sizeof(msg));
                memset(&src, 0, sizeof(src));
                msg.msg_name = &src;
                msg.msg_namelen = sizeof(src);
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;

                // Only provide ancillary space if we asked for timestamps.
                if (use_timestamping || use_timestampns) {
                    msg.msg_control = cbuf;
                    msg.msg_controllen = sizeof(cbuf);
                }

                ssize_t n = recvmsg(fd, &msg, 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    perror("recvmsg"); g_stop = 1; break;
                }

                struct timespec ts_rx = now_monotonic(); // default/fallback

                if (use_timestamping || use_timestampns) {
                    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
                         cm != NULL; cm = CMSG_NXTHDR(&msg, cm)) {

#ifdef SO_TIMESTAMPING
                        if (use_timestamping &&
                            cm->cmsg_level == SOL_SOCKET &&
                            cm->cmsg_type  == SO_TIMESTAMPING &&
                            cm->cmsg_len   >= CMSG_LEN(sizeof(struct timespec)*3)) {
                            struct timespec *ts = (struct timespec*)CMSG_DATA(cm);
                            // ts[0] is software timestamp; if MONOTONIC flag wasn’t available,
                            // this may be realtime—but Δt math still works.
                            if (ts[0].tv_sec || ts[0].tv_nsec) ts_rx = ts[0];
                            continue;
                        }
#endif
#ifdef SO_TIMESTAMPNS
                        if (use_timestampns &&
                            cm->cmsg_level == SOL_SOCKET &&
                            cm->cmsg_type  == SO_TIMESTAMPNS &&
                            cm->cmsg_len   >= CMSG_LEN(sizeof(struct timespec))) {
                            struct timespec *ts = (struct timespec*)CMSG_DATA(cm);
                            // CLOCK_REALTIME(hi-res). Δt math still fine.
                            if (ts->tv_sec || ts->tv_nsec) ts_rx = *ts;
                            continue;
                        }
#endif
#ifdef SO_RXQ_OVFL
                        if (cm->cmsg_level == SOL_SOCKET &&
                            cm->cmsg_type  == SO_RXQ_OVFL &&
                            cm->cmsg_len   >= CMSG_LEN(sizeof(uint32_t))) {
                            uint32_t cur = *(uint32_t*)CMSG_DATA(cm);
                            if (cur < last_rxq_count) {
                                S.rxq_drop_incr += (uint64_t)cur + (uint64_t)(UINT32_MAX - last_rxq_count) + 1ULL;
                            } else {
                                S.rxq_drop_incr += (uint64_t)(cur - last_rxq_count);
                            }
                            last_rxq_count = cur;
                            continue;
                        }
#endif
                    }
                }

                // Tally
                S.pkts++; S.bytes += (uint64_t)n;

                bool already = src_add_or_seen(srcs, SRC_CAP, src.sin_addr.s_addr, src.sin_port);
                if (!already) S.uniq_sources++;

                if (S.pkts > 1) {
                    double dt_ms = ns_to_ms(tsdiff_ns(ts_rx, t_prev));
                    if (dt_ms < 0) dt_ms = 0;
                    S.sum_dt_ms  += dt_ms;
                    S.sum_dt2_ms += dt_ms * dt_ms;
                    if (dt_ms < S.min_dt_ms) S.min_dt_ms = dt_ms;
                    if (dt_ms > S.max_dt_ms) S.max_dt_ms = dt_ms;
                    S.dt_samples++;
                }
                t_prev = ts_rx;
            }
        }

        t_now = now_monotonic();
        if (tsdiff_ns(t_now, t_window_end) >= 0) {
            double t_since_start_s = (double)tsdiff_ns(t_now, t_start) / 1e9;
            print_stats_line(t_since_start_s, &S, interval_ms);

            stats_reset(&S);
            src_table_clear(srcs, SRC_CAP);

            // Advance by whole intervals in case we lagged
            do {
                t_window_end.tv_nsec += (long)interval_ms * 1000000L;
                while (t_window_end.tv_nsec >= 1000000000L) {
                    t_window_end.tv_sec += 1; t_window_end.tv_nsec -= 1000000000L;
                }
            } while (tsdiff_ns(t_now, t_window_end) >= 0);
        }
    }

    close(fd);
    return 0;
}
