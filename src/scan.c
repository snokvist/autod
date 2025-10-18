#define _POSIX_C_SOURCE 200809L
#include "scan.h"
#include "parson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// ================= Tiny HTTP client =================

static int tcp_connect_nb(const char *ip, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) { close(fd); return -1; }

    int r = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
    if (r == 0) return fd;

    if (errno == EINPROGRESS) {
        struct pollfd p = { .fd = fd, .events = POLLOUT };
        r = poll(&p, 1, timeout_ms);
        if (r == 1 && (p.revents & POLLOUT)) {
            int err=0; socklen_t el=sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el)==0 && err==0) return fd;
        }
    }
    close(fd);
    return -1;
}

static int http_get_simple(const char *ip, int port, const char *path,
                           char *buf, size_t buflen, int timeout_ms) {
    int fd = tcp_connect_nb(ip, port, timeout_ms);
    if (fd < 0) return -1;

    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                     path, ip);
    if (n < 0 || write(fd, req, n) != n) { close(fd); return -1; }

    size_t w = 0;
    for (;;) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, timeout_ms);
        if (pr <= 0) break;
        if (p.revents & POLLIN) {
            ssize_t r = read(fd, buf + w, (buflen - 1) - w);
            if (r > 0) { w += (size_t)r; if (w >= buflen - 1) break; }
            else break;
        } else break;
    }
    buf[w] = '\0';
    close(fd);

    if (strncmp(buf, "HTTP/1.1 200", 12) != 0 && strncmp(buf, "HTTP/1.0 200", 12) != 0)
        return -2;
    return 0;
}

static const char* http_body_ptr(const char *resp) {
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? (p + 4) : NULL;
}

// ================= Globals & tuning =================

static pthread_mutex_t g_nodes_mx = PTHREAD_MUTEX_INITIALIZER;

static scan_node_t g_nodes[SCAN_MAX_NODES];
static int         g_nodes_count = 0;

static volatile int      g_scan_in_progress = 0;
static volatile unsigned g_scan_total = 0;
static volatile unsigned g_scan_done  = 0;
static volatile double   g_last_started  = 0.0;
static volatile double   g_last_finished = 0.0;
static volatile unsigned g_scan_seq = 0;

static scan_config_t g_cfg = {0};

typedef struct {
    int      connect_timeout_ms;
    int      health_timeout_ms;
    int      caps_timeout_ms;
    unsigned concurrency;
    unsigned stale_max_misses;
} scan_tun_t;

static scan_tun_t g_tun = {
    .connect_timeout_ms = 200,
    .health_timeout_ms  = 150,
    .caps_timeout_ms    = 400,
    .concurrency        = 16,
    .stale_max_misses   = 2
};

static inline double now_s(void){ return (double)time(NULL); }
static int is_link_local(const char *ip) { return strncmp(ip, "169.254.", 8) == 0; }

static inline int progress_pct(void) {
    unsigned tot = g_scan_total, don = g_scan_done;
    return tot ? (int)((100ULL * don) / tot) : 0;
}

static void nodes_reset(void) {
    pthread_mutex_lock(&g_nodes_mx);
    g_nodes_count = 0;
    pthread_mutex_unlock(&g_nodes_mx);
}

static int nodes_find_idx(const char *ip, int port) {
    for (int i=0;i<g_nodes_count;i++){
        if (g_nodes[i].port==port && strcmp(g_nodes[i].ip, ip)==0) return i;
    }
    return -1;
}

static void nodes_upsert(const scan_node_t *ni) {
    pthread_mutex_lock(&g_nodes_mx);
    int idx = nodes_find_idx(ni->ip, ni->port);
    if (idx >= 0) {
        // update in place, keep is_self/misses if present
        unsigned is_self = g_nodes[idx].is_self;
        g_nodes[idx] = *ni;
        g_nodes[idx].is_self = is_self ? 1u : ni->is_self;
        g_nodes[idx].misses  = 0;
    } else if (g_nodes_count < SCAN_MAX_NODES) {
        g_nodes[g_nodes_count++] = *ni;
    }
    pthread_mutex_unlock(&g_nodes_mx);
}

static void nodes_prune_after_scan(unsigned scan_seq) {
    pthread_mutex_lock(&g_nodes_mx);
    int w = 0;
    for (int i=0;i<g_nodes_count;i++){
        scan_node_t *n = &g_nodes[i];
        if (n->is_self) { g_nodes[w++] = *n; continue; }
        if (n->seen_scan == scan_seq) {
            n->misses = 0;
            g_nodes[w++] = *n;
        } else {
            unsigned m = n->misses + 1;
            if (m < g_tun.stale_max_misses) {
                n->misses = m;
                g_nodes[w++] = *n; // keep for now
            }
            // else drop (do not copy)
        }
    }
    g_nodes_count = w;
    pthread_mutex_unlock(&g_nodes_mx);
}

// ================ Public API ================

void scan_init(void) { /* nop */ }

void scan_set_tuning(const scan_tuning_t *t) {
    if (!t) return;
    if (t->connect_timeout_ms > 0) g_tun.connect_timeout_ms = t->connect_timeout_ms;
    if (t->health_timeout_ms  > 0) g_tun.health_timeout_ms  = t->health_timeout_ms;
    if (t->caps_timeout_ms    > 0) g_tun.caps_timeout_ms    = t->caps_timeout_ms;
    if (t->concurrency        > 0 && t->concurrency <= 256) g_tun.concurrency = t->concurrency;
    if (t->stale_max_misses   > 0) g_tun.stale_max_misses   = t->stale_max_misses;
}

void scan_reset_nodes(void) { nodes_reset(); }

void scan_seed_self_nodes(const scan_config_t *cfg) {
    if (!cfg) return;
    g_cfg = *cfg;

    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr)!=0) return;
    for (struct ifaddrs *ifa=ifaddr; ifa; ifa=ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        char ip[16];
        if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip))) continue;
        if (!strcmp(ip, "127.0.0.1")) continue;

        scan_node_t self; memset(&self, 0, sizeof(self));
        strncpy(self.ip, ip, sizeof(self.ip)-1);
        self.port = cfg->port;
        if (cfg->role[0])    strncpy(self.role,    cfg->role,    sizeof(self.role)-1);
        if (cfg->device[0])  strncpy(self.device,  cfg->device,  sizeof(self.device)-1);
        if (cfg->version[0]) strncpy(self.version, cfg->version, sizeof(self.version)-1);
        self.last_seen = now_s();
        self.is_self   = 1;
        self.seen_scan = g_scan_seq; // mark as current
        nodes_upsert(&self);
    }
    freeifaddrs(ifaddr);
}

int scan_is_running(void) { return g_scan_in_progress ? 1 : 0; }

void scan_get_status(scan_status_t *st) {
    if (!st) return;
    st->scanning      = scan_is_running();
    st->targets       = g_scan_total;
    st->done          = g_scan_done;
    st->progress_pct  = progress_pct();
    st->last_started  = g_last_started;
    st->last_finished = g_last_finished;
}

int scan_get_nodes(scan_node_t *dst, int max) {
    if (!dst || max <= 0) return 0;
    pthread_mutex_lock(&g_nodes_mx);
    int n = g_nodes_count;
    if (n > max) n = max;
    if (n > 0) memcpy(dst, g_nodes, (size_t)n * sizeof(scan_node_t));
    pthread_mutex_unlock(&g_nodes_mx);
    return n;
}

// ================ Target planning helpers ================

typedef struct { uint32_t *ips; unsigned n, cap; } ipvec_t;

static void ipvec_init(ipvec_t *v, uint32_t *buf, unsigned cap){ v->ips=buf; v->n=0; v->cap=cap; }
static int  ipvec_push(ipvec_t *v, uint32_t a){
    if (v->n >= v->cap) return -1;
    v->ips[v->n++] = a; return 0;
}
static int  ipvec_contains(const ipvec_t *v, uint32_t a){
    for (unsigned i=0;i<v->n;i++) if (v->ips[i]==a) return 1;
    return 0;
}

static unsigned add_known_first(ipvec_t *v, int port) {
    unsigned added=0;
    pthread_mutex_lock(&g_nodes_mx);
    for (int i=0;i<g_nodes_count;i++){
        scan_node_t *n = &g_nodes[i];
        if (n->port != port) continue;
        if (n->is_self) continue; // we don't re-probe ourselves
        struct in_addr ia;
        if (inet_pton(AF_INET, n->ip, &ia) != 1) continue;
        uint32_t a = ntohl(ia.s_addr);
        if (!ipvec_contains(v, a)) { if (ipvec_push(v, a)==0) added++; }
    }
    pthread_mutex_unlock(&g_nodes_mx);
    return added;
}

static unsigned add_arp_hits(ipvec_t *v) {
    FILE *f = fopen("/proc/net/arp", "r");
    if (!f) return 0;
    char line[256];
    unsigned added=0;
    // skip header
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    while (fgets(line, sizeof(line), f)) {
        char ip[64], hw[64], flags[8], mask[64], dev[64];
        // IP address HW type Flags HW address Mask Device
        if (sscanf(line, "%63s %63s %7s %63s %63s %63s", ip, hw, flags, hw, mask, dev) < 1)
            continue;
        if (is_link_local(ip) || strcmp(ip, "127.0.0.1")==0) continue;
        struct in_addr ia;
        if (inet_pton(AF_INET, ip, &ia)!=1) continue;
        uint32_t a = ntohl(ia.s_addr);
        if (!ipvec_contains(v, a)) { if (ipvec_push(v, a)==0) added++; }
    }
    fclose(f);
    return added;
}

static void add_subnet_walk_raw(ipvec_t *v, uint32_t a, uint32_t m, uint32_t self_a) {
    if (m == 0xffffffffu) {
        if (a != self_a && !ipvec_contains(v, a)) {
            struct in_addr t; t.s_addr = htonl(a);
            char tip[16];
            if (inet_ntop(AF_INET, &t, tip, sizeof(tip)) && !is_link_local(tip)) {
                (void)ipvec_push(v, a);
            }
        }
        return;
    }
    uint32_t net = a & m;
    uint32_t bcast = net | (~m);
    if (bcast <= net) return;

    // global safety: cap planned targets to avoid giant /16 explosions
    unsigned budget_left = (v->cap > v->n) ? (v->cap - v->n) : 0;
    if (budget_left == 0) return;

    for (uint32_t h = net + 1; h < bcast && v->n < v->cap; h++) {
        if (h == a || h == self_a) continue;
        struct in_addr t; t.s_addr = htonl(h);
        char tip[16]; if (!inet_ntop(AF_INET, &t, tip, sizeof(tip))) continue;
        if (is_link_local(tip)) continue;
        if (!ipvec_contains(v, h)) (void)ipvec_push(v, h);
    }
}

static void add_subnet_walk(ipvec_t *v, const char *ip, const char *mask, uint32_t *self_a) {
    struct in_addr ip4, m4;
    if (inet_pton(AF_INET, ip, &ip4)!=1 || inet_pton(AF_INET, mask, &m4)!=1) return;
    uint32_t a = ntohl(ip4.s_addr);
    uint32_t m = ntohl(m4.s_addr);
    if (*self_a == 0) *self_a = a;
    add_subnet_walk_raw(v, a, m, *self_a);
}

// ================ Worker pool ================

typedef struct {
    uint32_t *targets;
    unsigned  count;
    volatile unsigned *next_idx;
    int       port;
} work_ctx_t;

static void probe_and_maybe_add(uint32_t a, int port) {
    struct in_addr t; t.s_addr = htonl(a);
    char tip[16]; if (!inet_ntop(AF_INET, &t, tip, sizeof(tip))) { __sync_add_and_fetch(&g_scan_done, 1); return; }

    char resp[8192];

    // Quick: /health (allows super short timeout to skip dead hosts fast)
    int r = http_get_simple(tip, port, "/health", resp, sizeof(resp), g_tun.health_timeout_ms);
    if (r != 0) { __sync_add_and_fetch(&g_scan_done, 1); return; }

    // Detail: /caps
    r = http_get_simple(tip, port, "/caps", resp, sizeof(resp), g_tun.caps_timeout_ms);
    if (r == 0) {
        const char *body = http_body_ptr(resp);
        if (body) {
            JSON_Value *v = json_parse_string(body);
            if (v) {
                JSON_Object *o = json_object(v);
                scan_node_t ni; memset(&ni, 0, sizeof(ni));
                strncpy(ni.ip, tip, sizeof(ni.ip)-1);
                ni.port = port;
                const char *role   = json_object_get_string(o, "role");
                const char *device = json_object_get_string(o, "device");
                const char *ver    = json_object_get_string(o, "version");
                if (role)   strncpy(ni.role,    role,   sizeof(ni.role)-1);
                if (device) strncpy(ni.device,  device, sizeof(ni.device)-1);
                if (ver)    strncpy(ni.version, ver,    sizeof(ni.version)-1);
                ni.last_seen = now_s();
                ni.seen_scan = g_scan_seq;
                // keep is_self=0 by default
                nodes_upsert(&ni);
                json_value_free(v);
            }
        }
    }
    __sync_add_and_fetch(&g_scan_done, 1);
}

static void *worker_fn(void *arg) {
    work_ctx_t *wc = (work_ctx_t*)arg;
    for (;;) {
        unsigned i = __sync_fetch_and_add(wc->next_idx, 1);
        if (i >= wc->count) break;
        probe_and_maybe_add(wc->targets[i], wc->port);
    }
    return NULL;
}

// ================ Scan thread ================

typedef struct { scan_config_t cfg; } scan_ctx_t;

static void plan_targets(ipvec_t *vec, const scan_config_t *cfg, uint32_t *self_a_out) {
    // capacity: keep a hard cap to avoid runaway time
    // We reuse vec->cap set by caller; aim for <= 2048 targets total.
    *self_a_out = 0;

    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) != 0) return;

    // First: known hosts (from cache)
    add_known_first(vec, cfg->port);

    // Also: ARP cache (fast wins)
    add_arp_hits(vec);

    // Finally: full subnet sweep per iface
    for (struct ifaddrs *ifa=ifaddr; ifa; ifa=ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_netmask) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        char ip[16], mask[16];
        if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_addr)->sin_addr, ip, sizeof(ip))) continue;
        if (!inet_ntop(AF_INET, &((struct sockaddr_in*)ifa->ifa_netmask)->sin_addr, mask, sizeof(mask))) continue;
        if (!strcmp(ip, "127.0.0.1") || is_link_local(ip)) continue;

        add_subnet_walk(vec, ip, mask, self_a_out);
    }
    freeifaddrs(ifaddr);

    // Plus any additional configured subnets (CIDR strings parsed earlier)
    if (cfg->extra_subnet_count > 0) {
        uint32_t self_a = *self_a_out;
        for (unsigned i = 0; i < cfg->extra_subnet_count && i < SCAN_MAX_EXTRA_SUBNETS; i++) {
            uint32_t net = cfg->extra_subnets[i].network;
            uint32_t mask = cfg->extra_subnets[i].netmask;
            if (mask == 0) continue; // skip /0 (too broad)
            add_subnet_walk_raw(vec, net, mask, self_a);
        }
    }
}

static void *scan_thread(void *arg) {
    scan_ctx_t *sc = (scan_ctx_t*)arg;

    // New scan sequence
    unsigned seq = __sync_add_and_fetch(&g_scan_seq, 1);
    g_last_started = now_s();
    g_last_finished = 0.0;

    // Build target list (known first + ARP + sweep), capped
    uint32_t targets_buf[2048];
    ipvec_t targets; ipvec_init(&targets, targets_buf, (unsigned)(sizeof(targets_buf)/sizeof(targets_buf[0])));
    uint32_t self_a = 0;
    plan_targets(&targets, &sc->cfg, &self_a);

    // publish totals
    __sync_lock_test_and_set(&g_scan_total, targets.n);
    __sync_lock_test_and_set(&g_scan_done,  0);

    // Seed/refresh self nodes (keep them resident)
    scan_seed_self_nodes(&sc->cfg);

    // Worker pool
    unsigned workers = g_tun.concurrency;
    if (workers == 0) workers = 1;
    if (workers > 64) workers = 64; // sane cap

    pthread_t tids[64];
    volatile unsigned next_idx = 0;
    work_ctx_t wc = { .targets = targets.ips, .count = targets.n, .next_idx = &next_idx, .port = sc->cfg.port };

    for (unsigned i=0;i<workers;i++){
        pthread_create(&tids[i], NULL, worker_fn, (void*)&wc);
    }
    for (unsigned i=0;i<workers;i++){
        if (i < workers) pthread_join(tids[i], NULL);
    }

    // Prune stales (nodes not seen in this seq)
    nodes_prune_after_scan(seq);

    g_last_finished = now_s();
    __sync_lock_release(&g_scan_in_progress);
    free(sc);
    return NULL;
}

int scan_start_async(const scan_config_t *cfg) {
    if (!cfg) return -1;
    g_cfg = *cfg;
    if (!__sync_bool_compare_and_swap(&g_scan_in_progress, 0, 1)) {
        return 1; // already running
    }

    pthread_t th;
    scan_ctx_t *sc = (scan_ctx_t*)malloc(sizeof(*sc));
    if (!sc) {
        __sync_lock_release(&g_scan_in_progress);
        return -1;
    }
    sc->cfg = *cfg;
    if (pthread_create(&th, NULL, scan_thread, sc) == 0) {
        pthread_detach(th);
        return 0;
    }
    free(sc);
    __sync_lock_release(&g_scan_in_progress);
    return -1;
}
