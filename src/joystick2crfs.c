/**
 * joystick2crfs.c - SDL joystick to CRSF bridge with UART/UDP outputs
 *
 * The utility samples the selected joystick at 250 Hz, maps its controls to
 * 16 CRSF channels, and streams the packed frames either directly over a UART
 * or to a UDP peer. Runtime behaviour is configured exclusively via a config
 * file (default: /etc/joystick2crfs.conf).
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <SDL2/SDL.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
#define LOOP_HZ            250
#define LOOP_NS            4000000L               /* 4 ms */

#define CRSF_DEST          0xC8
#define CRSF_TYPE_CHANNELS 0x16
#define CRSF_PAYLOAD_LEN   22                     /* 16×11-bit */
#define CRSF_FRAME_LEN     24
#define CRSF_MIN           172
#define CRSF_MAX           1811
#define CRSF_RANGE         (CRSF_MAX - CRSF_MIN)
#define CRSF_MID           ((CRSF_MIN + CRSF_MAX + 1) / 2)

#define DEFAULT_CONF       "/etc/joystick2crfs.conf"
#define MAX_LINE_LEN       512

/* ------------------------------------------------------------------------- */
typedef struct {
    int rate;                   /* 50 | 125 | 250 Hz */
    int stats;                  /* print timing stats */
    int simulation;             /* skip serial output */
    int channels;               /* print channels */
    int serial_enabled;
    char serial_device[128];
    int serial_baud;
    int udp_enabled;
    char udp_target[256];
    int map[16];
    int invert[16];
    int dead[16];
    int arm_toggle;             /* -1 disables, otherwise channel index */
    int joystick_index;
    int rescan_interval;        /* seconds */
} config_t;

/* ------------------------------------------------------------------------- */
static volatile int g_run = 1;
static void on_sigint(int sig){ (void)sig; g_run = 0; }

/* ------------------------------------------------------------------------- */
static uint8_t crc8(const uint8_t *d, size_t n)
{
    uint8_t c = 0;
    while (n--) {
        c ^= *d++;
        for (int i = 0; i < 8; i++) {
            if (c & 0x80U) {
                c = (uint8_t)((c << 1) ^ 0xD5U);
            } else {
                c <<= 1;
            }
        }
    }
    return c;
}

static void pack_channels(const uint16_t ch[16], uint8_t out[CRSF_PAYLOAD_LEN])
{
    memset(out, 0, CRSF_PAYLOAD_LEN);
    uint32_t bit = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t byte = bit >> 3;
        uint32_t off = bit & 7U;
        uint32_t v = ch[i] & 0x7FFU;

        out[byte] |= (uint8_t)(v << off);
        if (byte + 1 < CRSF_PAYLOAD_LEN) {
            out[byte + 1] |= (uint8_t)(v >> (8U - off));
        }
        if (off >= 6U && byte + 2 < CRSF_PAYLOAD_LEN) {
            out[byte + 2] |= (uint8_t)(v >> (16U - off));
        }
        bit += 11U;
    }
}

static speed_t baud_const(int baud)
{
    switch (baud) {
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

static int open_serial(const char *dev, int baud)
{
    int fd = open(dev, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
    if (fd < 0) {
        perror("serial");
        return -1;
    }

    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }
    cfmakeraw(&t);

    speed_t sp = baud_const(baud);
    if (!sp) {
        fprintf(stderr, "Unsupported baud %d, falling back to 115200\n", baud);
        sp = B115200;
    }
    if (cfsetspeed(&t, sp) < 0) {
        perror("cfsetspeed");
        close(fd);
        return -1;
    }

    t.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct timespec ts = {0, 1000000L};
            nanosleep(&ts, NULL);
            continue;
        }
        return -1;
    }
    return 0;
}

static int parse_host_port(const char *spec, char **host_out, char **port_out)
{
    if (!spec) {
        return -1;
    }
    char *dup = strdup(spec);
    if (!dup) {
        return -1;
    }

    char *host = dup;
    char *port = NULL;

    if (dup[0] == '[') {
        char *closing = strchr(dup, ']');
        if (!closing || closing[1] != ':' || !closing[2]) {
            free(dup);
            return -1;
        }
        *closing = '\0';
        host = dup + 1;
        port = closing + 2;
    } else {
        char *colon = strrchr(dup, ':');
        if (!colon || !colon[1]) {
            free(dup);
            return -1;
        }
        *colon = '\0';
        host = dup;
        port = colon + 1;
    }

    char *host_copy = strdup(host);
    char *port_copy = strdup(port);
    free(dup);

    if (!host_copy || !port_copy) {
        free(host_copy);
        free(port_copy);
        return -1;
    }

    *host_out = host_copy;
    *port_out = port_copy;
    return 0;
}

static int open_udp_target(const char *target, struct sockaddr_storage *addr, socklen_t *addrlen)
{
    char *host = NULL;
    char *port = NULL;
    if (parse_host_port(target, &host, &port) < 0) {
        fprintf(stderr, "Invalid UDP target '%s' (use host:port or [ipv6]:port)\n", target);
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "UDP getaddrinfo(%s,%s): %s\n", host, port, gai_strerror(rc));
        free(host);
        free(port);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd >= 0) {
            memcpy(addr, ai->ai_addr, ai->ai_addrlen);
            *addrlen = (socklen_t)ai->ai_addrlen;
            break;
        }
    }

    freeaddrinfo(res);
    free(host);
    free(port);

    if (fd < 0) {
        perror("udp socket");
        return -1;
    }
    return fd;
}

static void try_rt(int prio)
{
    struct sched_param sp = { .sched_priority = prio };
    if (!sched_setscheduler(0, SCHED_FIFO, &sp)) {
        fprintf(stderr, "◎ SCHED_FIFO %d\n", prio);
    }
}

static inline uint16_t scale_axis(int32_t v)
{
    double span = (double)CRSF_RANGE;
    double scaled = (double)CRSF_MIN + ((double)v + 32768.0) * span / 65535.0;
    if (scaled < CRSF_MIN) {
        scaled = CRSF_MIN;
    }
    if (scaled > CRSF_MAX) {
        scaled = CRSF_MAX;
    }
    return (uint16_t)(scaled + 0.5);
}

static inline uint16_t scale_bool(int on)
{
    return (uint16_t)(on ? CRSF_MAX : CRSF_MIN);
}

static inline int32_t clip_dead(int32_t v, int thr)
{
    if (thr > 0 && v > -thr && v < thr) {
        return 0;
    }
    return v;
}

static void build_channels(SDL_Joystick *js, const int dead[16], uint16_t ch_s[16], int32_t ch_r[16])
{
    ch_r[0] = SDL_JoystickGetAxis(js, 0);
    ch_r[1] = SDL_JoystickGetAxis(js, 1);
    ch_r[2] = SDL_JoystickGetAxis(js, 2);
    ch_r[3] = SDL_JoystickGetAxis(js, 5);
    for (int i = 0; i < 4; i++) {
        ch_r[i] = clip_dead(ch_r[i], dead[i]);
    }
    ch_s[0] = scale_axis(ch_r[0]);
    ch_s[1] = scale_axis(-ch_r[1]);
    ch_s[2] = scale_axis(ch_r[2]);
    ch_s[3] = scale_axis(-ch_r[3]);

    ch_r[4] = clip_dead(SDL_JoystickGetAxis(js, 3), dead[4]);
    ch_r[5] = clip_dead(SDL_JoystickGetAxis(js, 4), dead[5]);
    ch_s[4] = scale_axis(ch_r[4]);
    ch_s[5] = scale_axis(ch_r[5]);

    int dpx = 0, dpy = 0;
    if (SDL_JoystickNumHats(js) > 0) {
        uint8_t h = SDL_JoystickGetHat(js, 0);
        dpx = (h & SDL_HAT_RIGHT) ? 1 : (h & SDL_HAT_LEFT) ? -1 : 0;
        dpy = (h & SDL_HAT_UP) ? 1 : (h & SDL_HAT_DOWN) ? -1 : 0;
    } else if (SDL_JoystickNumAxes(js) >= 8) {
        dpx = SDL_JoystickGetAxis(js, 6) / 32767;
        dpy = -SDL_JoystickGetAxis(js, 7) / 32767;
    } else if (SDL_JoystickNumButtons(js) >= 15) {
        dpy = SDL_JoystickGetButton(js, 11) ? 1 : SDL_JoystickGetButton(js, 12) ? -1 : 0;
        dpx = SDL_JoystickGetButton(js, 13) ? -1 : SDL_JoystickGetButton(js, 14) ? 1 : 0;
    }
    int32_t dpx_axis = dpx * 32767;
    int32_t dpy_axis = dpy * 32767;
    ch_r[6] = dpx_axis;
    ch_r[7] = dpy_axis;
    ch_s[6] = scale_axis(dpx_axis);
    ch_s[7] = scale_axis(dpy_axis);

    for (int i = 8; i < 16; i++) {
        int b = SDL_JoystickGetButton(js, i - 8);
        ch_r[i] = b;
        ch_s[i] = scale_bool(b);
    }
}

/* --------------------------- Config helpers -------------------------------- */
static void trim(char *s)
{
    if (!s) {
        return;
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    size_t len = (size_t)(end - start);
    if (start != s) {
        memmove(s, start, len);
    }
    s[len] = '\0';
}

static int parse_bool_value(const char *str, int *out)
{
    if (!str || !out) {
        return -1;
    }
    if (!strcasecmp(str, "1") || !strcasecmp(str, "true") || !strcasecmp(str, "yes") || !strcasecmp(str, "on")) {
        *out = 1;
        return 0;
    }
    if (!strcasecmp(str, "0") || !strcasecmp(str, "false") || !strcasecmp(str, "no") || !strcasecmp(str, "off")) {
        *out = 0;
        return 0;
    }
    return -1;
}

static void parse_map_list(const char *str, int out[16])
{
    for (int i = 0; i < 16; i++) {
        out[i] = i;
    }
    if (!str || !*str) {
        return;
    }
    char *dup = strdup(str);
    if (!dup) {
        return;
    }
    char *save = NULL;
    char *tok = strtok_r(dup, ",", &save);
    for (int idx = 0; tok && idx < 16; idx++, tok = strtok_r(NULL, ",", &save)) {
        int v = atoi(tok);
        if (v >= 1 && v <= 16) {
            out[idx] = v - 1;
        }
    }
    free(dup);
}

static void parse_invert_list(const char *str, int out[16])
{
    for (int i = 0; i < 16; i++) {
        out[i] = 0;
    }
    if (!str || !*str) {
        return;
    }
    char *dup = strdup(str);
    if (!dup) {
        return;
    }
    char *save = NULL;
    char *tok = strtok_r(dup, ",", &save);
    while (tok) {
        int ch = atoi(tok);
        if (ch >= 1 && ch <= 16) {
            out[ch - 1] = 1;
        }
        tok = strtok_r(NULL, ",", &save);
    }
    free(dup);
}

static void parse_dead_list(const char *str, int out[16])
{
    for (int i = 0; i < 16; i++) {
        out[i] = 0;
    }
    if (!str || !*str) {
        return;
    }
    char *dup = strdup(str);
    if (!dup) {
        return;
    }
    char *save = NULL;
    char *tok = strtok_r(dup, ",", &save);
    for (int i = 0; tok && i < 16; i++, tok = strtok_r(NULL, ",", &save)) {
        out[i] = abs(atoi(tok));
    }
    free(dup);
}

static void config_defaults(config_t *cfg)
{
    cfg->rate = 125;
    cfg->stats = 0;
    cfg->simulation = 0;
    cfg->channels = 0;
    cfg->serial_enabled = 0;
    snprintf(cfg->serial_device, sizeof(cfg->serial_device), "%s", "/dev/ttyUSB0");
    cfg->serial_baud = 115200;
    cfg->udp_enabled = 1;
    snprintf(cfg->udp_target, sizeof(cfg->udp_target), "%s", "192.168.0.1:14550");
    cfg->arm_toggle = 4;
    cfg->joystick_index = 0;
    cfg->rescan_interval = 5;
    for (int i = 0; i < 16; i++) {
        cfg->map[i] = i;
        cfg->invert[i] = 0;
        cfg->dead[i] = 0;
    }
}

static int config_load(config_t *cfg, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open config %s: %s\n", path, strerror(errno));
        return -1;
    }

    char line[MAX_LINE_LEN];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        trim(line);
        if (!line[0]) {
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: ignoring line without '='\n", path, lineno);
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (!strcasecmp(key, "rate")) {
            cfg->rate = atoi(val);
        } else if (!strcasecmp(key, "stats")) {
            int b;
            if (parse_bool_value(val, &b) == 0) {
                cfg->stats = b;
            }
        } else if (!strcasecmp(key, "simulation")) {
            int b;
            if (parse_bool_value(val, &b) == 0) {
                cfg->simulation = b;
            }
        } else if (!strcasecmp(key, "channels")) {
            int b;
            if (parse_bool_value(val, &b) == 0) {
                cfg->channels = b;
            }
        } else if (!strcasecmp(key, "serial_enabled")) {
            int b;
            if (parse_bool_value(val, &b) == 0) {
                cfg->serial_enabled = b;
            }
        } else if (!strcasecmp(key, "serial_device")) {
            snprintf(cfg->serial_device, sizeof(cfg->serial_device), "%s", val);
        } else if (!strcasecmp(key, "serial_baud")) {
            cfg->serial_baud = atoi(val);
        } else if (!strcasecmp(key, "udp_enabled")) {
            int b;
            if (parse_bool_value(val, &b) == 0) {
                cfg->udp_enabled = b;
            }
        } else if (!strcasecmp(key, "udp_target")) {
            snprintf(cfg->udp_target, sizeof(cfg->udp_target), "%s", val);
        } else if (!strcasecmp(key, "arm_toggle")) {
            int ch = atoi(val);
            if (ch >= 1 && ch <= 16) {
                cfg->arm_toggle = ch - 1;
            } else if (ch <= 0) {
                cfg->arm_toggle = -1;
            } else {
                fprintf(stderr, "%s:%d: arm_toggle must be 1-16 (or 0 to disable)\n", path, lineno);
            }
        } else if (!strcasecmp(key, "map")) {
            parse_map_list(val, cfg->map);
        } else if (!strcasecmp(key, "invert")) {
            parse_invert_list(val, cfg->invert);
        } else if (!strcasecmp(key, "deadband")) {
            parse_dead_list(val, cfg->dead);
        } else if (!strcasecmp(key, "joystick_index")) {
            cfg->joystick_index = atoi(val);
        } else if (!strcasecmp(key, "rescan_interval")) {
            cfg->rescan_interval = atoi(val);
        } else {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key);
        }
    }

    fclose(fp);
    if (cfg->rescan_interval <= 0) {
        cfg->rescan_interval = 5;
    }
    if (cfg->joystick_index < 0) {
        cfg->joystick_index = 0;
    }
    return 0;
}

/* --------------------------- Time helpers ---------------------------------- */
static int timespec_cmp(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec) {
        return (a->tv_sec > b->tv_sec) ? 1 : -1;
    }
    if (a->tv_nsec != b->tv_nsec) {
        return (a->tv_nsec > b->tv_nsec) ? 1 : -1;
    }
    return 0;
}

static struct timespec timespec_add(struct timespec ts, int sec, long nsec)
{
    ts.tv_sec += sec;
    ts.tv_nsec += nsec;
    while (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec += 1;
    }
    while (ts.tv_nsec < 0) {
        ts.tv_nsec += 1000000000L;
        ts.tv_sec -= 1;
    }
    return ts;
}

static int64_t timespec_diff_ms(const struct timespec *start, const struct timespec *end)
{
    int64_t sec = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t nsec = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    if (nsec < 0) {
        sec -= 1;
        nsec += 1000000000L;
    }
    return sec * 1000 + nsec / 1000000L;
}

/* ------------------------------- Main -------------------------------------- */
int main(int argc, char **argv)
{
    const char *conf_path = DEFAULT_CONF;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [config_path]\n", argv[0]);
        return 1;
    }
    if (argc == 2) {
        conf_path = argv[1];
    }

    config_t cfg;
    config_defaults(&cfg);
    if (config_load(&cfg, conf_path) < 0) {
        return 1;
    }
    if (cfg.rate != 50 && cfg.rate != 125 && cfg.rate != 250) {
        fprintf(stderr, "Config rate must be 50, 125, or 250\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }

    int serial_fd = -1;
    if (cfg.serial_enabled && !cfg.simulation) {
        serial_fd = open_serial(cfg.serial_device, cfg.serial_baud);
        if (serial_fd < 0) {
            SDL_Quit();
            return 1;
        }
    }

    int udp_fd = -1;
    struct sockaddr_storage udp_addr;
    socklen_t udp_addrlen = 0;
    if (cfg.udp_enabled) {
        if (cfg.udp_target[0] == '\0') {
            fprintf(stderr, "UDP enabled but udp_target is empty\n");
            if (serial_fd < 0) {
                SDL_Quit();
                return 1;
            }
            fprintf(stderr, "Continuing without UDP output.\n");
        } else {
            udp_fd = open_udp_target(cfg.udp_target, &udp_addr, &udp_addrlen);
            if (udp_fd < 0) {
                if (serial_fd < 0) {
                    SDL_Quit();
                    return 1;
                }
                fprintf(stderr, "Continuing without UDP output.\n");
            }
        }
    }

    if (serial_fd < 0 && (!cfg.udp_enabled || udp_fd < 0)) {
        fprintf(stderr, "Warning: no output destinations configured; frames will stay local.\n");
    }

    if (udp_fd >= 0) {
        char hostbuf[NI_MAXHOST];
        char servbuf[NI_MAXSERV];
        int gi = getnameinfo((struct sockaddr *)&udp_addr, udp_addrlen,
                             hostbuf, sizeof(hostbuf),
                             servbuf, sizeof(servbuf),
                             NI_NUMERICHOST | NI_NUMERICSERV);
        if (gi == 0) {
            fprintf(stderr, "UDP target %s resolved to %s:%s\n",
                    cfg.udp_target, hostbuf, servbuf);
        } else {
            fprintf(stderr, "UDP target %s ready (resolution error: %s)\n",
                    cfg.udp_target, gai_strerror(gi));
        }
    }

    try_rt(10);
    signal(SIGINT, on_sigint);

    SDL_Joystick *js = NULL;
    struct timespec next_rescan;
    clock_gettime(CLOCK_MONOTONIC, &next_rescan);
    struct timespec next_tick = next_rescan;

    const uint64_t every = LOOP_HZ / (uint64_t)cfg.rate;
    uint64_t loops = 0;

    double t_min = 1e9, t_max = 0.0, t_sum = 0.0;
    uint64_t t_cnt = 0;
    uint64_t serial_packets = 0;
    uint64_t udp_packets = 0;
    char rxbuf[256];
    size_t rxlen = 0;

    uint8_t frame[CRSF_FRAME_LEN + 2];
    frame[0] = CRSF_DEST;
    frame[1] = CRSF_FRAME_LEN;
    frame[2] = CRSF_TYPE_CHANNELS;

    int arm_channel = (cfg.arm_toggle >= 0 && cfg.arm_toggle < 16) ? cfg.arm_toggle : -1;
    int arm_sticky = 0;
    int arm_press_active = 0;
    struct timespec arm_press_start = {0, 0};

    while (g_run) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        if (js && !SDL_JoystickGetAttached(js)) {
            fprintf(stderr, "Joystick %d detached\n", cfg.joystick_index);
            SDL_JoystickClose(js);
            js = NULL;
            next_rescan = timespec_add(now, cfg.rescan_interval, 0);
        }

        if (!js && timespec_cmp(&now, &next_rescan) >= 0) {
            int count = SDL_NumJoysticks();
            if (cfg.joystick_index < count) {
                SDL_Joystick *candidate = SDL_JoystickOpen(cfg.joystick_index);
                if (candidate) {
                    js = candidate;
                    const char *name = SDL_JoystickName(js);
                    fprintf(stderr, "Joystick %d connected: %s\n",
                            cfg.joystick_index, name ? name : "unknown");
                } else {
                    fprintf(stderr, "Failed to open joystick %d: %s\n", cfg.joystick_index, SDL_GetError());
                    next_rescan = timespec_add(now, cfg.rescan_interval, 0);
                }
            } else {
                next_rescan = timespec_add(now, cfg.rescan_interval, 0);
            }
        }

        if (js) {
            SDL_JoystickUpdate();
        }

        uint16_t ch_source[16];
        int32_t raw_source[16];
        if (js) {
            build_channels(js, cfg.dead, ch_source, raw_source);
        } else {
            for (int i = 0; i < 16; i++) {
                raw_source[i] = 0;
                if (i < 8) {
                    ch_source[i] = scale_axis(0);
                } else {
                    ch_source[i] = scale_bool(0);
                }
            }
        }

        uint16_t ch_out[16];
        int32_t raw_out[16];
        for (int i = 0; i < 16; i++) {
            int src = cfg.map[i];
            if (src < 0 || src >= 16) {
                src = i;
            }
            uint16_t v = ch_source[src];
            if (cfg.invert[i]) {
                v = (uint16_t)(CRSF_MIN + CRSF_MAX - v);
            }
            ch_out[i] = v;
            raw_out[i] = raw_source[src];
        }

        if (arm_channel >= 0) {
            int src = cfg.map[arm_channel];
            if (src < 0 || src >= 16) {
                src = arm_channel;
            }
            uint16_t arm_input = ch_source[src];
            int arm_high = arm_input > 1709;
            if (arm_high) {
                if (!arm_press_active) {
                    arm_press_start = now;
                    arm_press_active = 1;
                } else if (!arm_sticky) {
                    int64_t held = timespec_diff_ms(&arm_press_start, &now);
                    if (held >= 1000) {
                        arm_sticky = 1;
                    }
                }
            } else if (arm_press_active) {
                int64_t held = timespec_diff_ms(&arm_press_start, &now);
                if (arm_sticky && held < 1000) {
                    arm_sticky = 0;
                }
                arm_press_active = 0;
            }

            uint16_t arm_high_value = cfg.invert[arm_channel] ? CRSF_MIN : CRSF_MAX;
            uint16_t arm_low_value = cfg.invert[arm_channel] ? CRSF_MAX : CRSF_MIN;
            if (arm_sticky || arm_high) {
                ch_out[arm_channel] = arm_high_value;
                raw_out[arm_channel] = 1;
            } else {
                ch_out[arm_channel] = arm_low_value;
                raw_out[arm_channel] = 0;
            }
        }

        if (loops % every == 0) {
            pack_channels(ch_out, frame + 3);
            frame[CRSF_FRAME_LEN + 1] = crc8(frame + 2, CRSF_FRAME_LEN - 1);

            if (cfg.channels) {
                printf("CH:");
                for (int i = 0; i < 16; i++) {
                    printf(" %4u", ch_out[i]);
                }
                printf(" | RAW:");
                for (int i = 0; i < 16; i++) {
                    printf(" %6d", raw_out[i]);
                }
                putchar('\n');
            }

            if (serial_fd >= 0) {
                if (send_all(serial_fd, frame, CRSF_FRAME_LEN + 2) < 0) {
                    perror("serial write");
                    g_run = 0;
                } else {
                    serial_packets++;
                }
            }
            if (udp_fd >= 0) {
                ssize_t sent = sendto(udp_fd, frame, CRSF_FRAME_LEN + 2, 0,
                                       (struct sockaddr *)&udp_addr, udp_addrlen);
                if (sent < 0) {
                    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        perror("udp send");
                        g_run = 0;
                    }
                } else {
                    udp_packets++;
                }
            }
        }
        loops++;

        if (cfg.stats) {
            struct timespec current;
            clock_gettime(CLOCK_MONOTONIC, &current);
            double dt = (current.tv_sec - next_tick.tv_sec) + (current.tv_nsec - next_tick.tv_nsec) / 1e9;
            if (dt > 0.0) {
                if (dt < t_min) {
                    t_min = dt;
                }
                if (dt > t_max) {
                    t_max = dt;
                }
                t_sum += dt;
                t_cnt++;
                if (t_cnt >= LOOP_HZ) {
                    printf("loop min %.3f  max %.3f  avg %.3f ms",
                           t_min * 1e3, t_max * 1e3,
                           (t_sum / (double)t_cnt) * 1e3);
                    if (serial_fd >= 0) {
                        printf("  serial %llu/s",
                               (unsigned long long)serial_packets);
                    }
                    if (udp_fd >= 0) {
                        printf("  udp %llu/s",
                               (unsigned long long)udp_packets);
                    }
                    putchar('\n');
                    t_min = 1e9;
                    t_max = 0.0;
                    t_sum = 0.0;
                    t_cnt = 0;
                    serial_packets = 0;
                    udp_packets = 0;
                }
            }
        }

        if (cfg.stats && serial_fd >= 0) {
            char tmp[64];
            ssize_t n;
            while ((n = read(serial_fd, tmp, sizeof(tmp))) > 0) {
                for (ssize_t i = 0; i < n; i++) {
                    if (rxlen < sizeof(rxbuf) - 1) {
                        rxbuf[rxlen++] = tmp[i];
                    }
                    if (tmp[i] == '\n') {
                        rxbuf[rxlen] = '\0';
                        fputs(rxbuf, stdout);
                        rxlen = 0;
                    }
                }
            }
        }

        next_tick = timespec_add(next_tick, 0, LOOP_NS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
    }

    if (serial_fd >= 0) {
        close(serial_fd);
    }
    if (udp_fd >= 0) {
        close(udp_fd);
    }
    if (js) {
        SDL_JoystickClose(js);
    }
    SDL_Quit();
    return 0;
}
