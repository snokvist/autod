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
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ------------------------------------------------------------------------- */
#define LOOP_HZ            250
#define LOOP_NS            4000000L               /* 4 ms */
#define SSE_INTERVAL_NS    100000000L             /* 10 Hz */

#define CRSF_DEST          0xC8
#define CRSF_TYPE_CHANNELS 0x16
#define CRSF_PAYLOAD_LEN   22                     /* 16×11-bit */
#define CRSF_FRAME_LEN     24
#define CRSF_MIN           172
#define CRSF_MAX           1811
#define CRSF_RANGE         (CRSF_MAX - CRSF_MIN)
#define CRSF_MID           ((CRSF_MIN + CRSF_MAX + 1) / 2)

#define MAVLINK_STX                     0xFD
#define MAVLINK_MSG_RC_OVERRIDE         70
#define MAVLINK_PAYLOAD_LEN             18
#define MAVLINK_HDR_LEN                 10
#define MAVLINK_FRAME_LEN               (MAVLINK_HDR_LEN + MAVLINK_PAYLOAD_LEN + 2)
#define MAVLINK_RC_CRC_EXTRA            124
#define MAVLINK_MIN_US                  1000
#define MAVLINK_MAX_US                  2000
#define MAVLINK_RANGE_US                (MAVLINK_MAX_US - MAVLINK_MIN_US)
#define FRAME_BUFFER_MAX                ((CRSF_FRAME_LEN + 2) > MAVLINK_FRAME_LEN ? (CRSF_FRAME_LEN + 2) : MAVLINK_FRAME_LEN)

#define PROTOCOL_CRSF       0
#define PROTOCOL_MAVLINK    1

#define DEFAULT_CONF       "/etc/joystick2crfs.conf"
#define MAX_LINE_LEN       512

/* ------------------------------------------------------------------------- */
typedef struct {
    int rate;                   /* 50 | 125 | 250 Hz */
    int stats;                  /* print timing stats */
    int simulation;             /* skip serial output */
    int channels;               /* print channels */
    int protocol;               /* PROTOCOL_* selector */
    int serial_enabled;
    char serial_device[128];
    int serial_baud;
    int udp_enabled;
    char udp_target[256];
    int sse_enabled;
    char sse_bind[256];
    char sse_path[64];
    int mavlink_sysid;
    int mavlink_compid;
    int mavlink_target_sysid;
    int mavlink_target_compid;
    int map[16];
    int invert[16];
    int dead[16];
    int arm_toggle;             /* -1 disables, otherwise channel index */
    int joystick_index;
    int rescan_interval;        /* seconds */
} config_t;

/* ------------------------------------------------------------------------- */
static volatile int g_run = 1;
static volatile sig_atomic_t g_reload = 0;
static void on_sigint(int sig){ (void)sig; g_run = 0; }
static void on_sighup(int sig){ (void)sig; g_reload = 1; }

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

static uint16_t crc_x25_byte(uint16_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) {
        if (crc & 1U) {
            crc = (uint16_t)((crc >> 1) ^ 0x8408U);
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

static uint16_t crc_x25(const uint8_t *d, size_t n)
{
    uint16_t crc = 0xFFFFU;
    while (n--) {
        crc = crc_x25_byte(crc, *d++);
    }
    return crc;
}

static uint16_t crsf_to_mavlink(uint16_t v)
{
    if (v <= CRSF_MIN) {
        return MAVLINK_MIN_US;
    }
    if (v >= CRSF_MAX) {
        return MAVLINK_MAX_US;
    }
    int32_t scaled = (int32_t)(v - CRSF_MIN) * MAVLINK_RANGE_US;
    scaled = (scaled + (CRSF_RANGE / 2)) / CRSF_RANGE;
    int32_t out = MAVLINK_MIN_US + scaled;
    if (out < MAVLINK_MIN_US) {
        out = MAVLINK_MIN_US;
    } else if (out > MAVLINK_MAX_US) {
        out = MAVLINK_MAX_US;
    }
    return (uint16_t)out;
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

static size_t pack_mavlink_rc_override(const config_t *cfg, const uint16_t ch[16], uint8_t *seq, uint8_t out[MAVLINK_FRAME_LEN])
{
    uint8_t packet_seq = *seq;
    *seq = (uint8_t)(packet_seq + 1U);

    out[0] = MAVLINK_STX;
    out[1] = MAVLINK_PAYLOAD_LEN;
    out[2] = 0U; /* incompat flags */
    out[3] = 0U; /* compat flags */
    out[4] = packet_seq;
    out[5] = (uint8_t)cfg->mavlink_sysid;
    out[6] = (uint8_t)cfg->mavlink_compid;
    out[7] = (uint8_t)(MAVLINK_MSG_RC_OVERRIDE & 0xFFU);
    out[8] = (uint8_t)((MAVLINK_MSG_RC_OVERRIDE >> 8) & 0xFFU);
    out[9] = (uint8_t)((MAVLINK_MSG_RC_OVERRIDE >> 16) & 0xFFU);

    size_t off = MAVLINK_HDR_LEN;
    out[off++] = (uint8_t)cfg->mavlink_target_sysid;
    out[off++] = (uint8_t)cfg->mavlink_target_compid;

    for (int i = 0; i < 8; i++) {
        uint16_t mv = crsf_to_mavlink(ch[i]);
        out[off++] = (uint8_t)(mv & 0xFFU);
        out[off++] = (uint8_t)(mv >> 8); /* little endian */
    }

    uint16_t crc = crc_x25(out + MAVLINK_HDR_LEN, MAVLINK_PAYLOAD_LEN);
    crc = crc_x25_byte(crc, (uint8_t)(MAVLINK_MSG_RC_OVERRIDE & 0xFFU));
    crc = crc_x25_byte(crc, (uint8_t)((MAVLINK_MSG_RC_OVERRIDE >> 8) & 0xFFU));
    crc = crc_x25_byte(crc, (uint8_t)((MAVLINK_MSG_RC_OVERRIDE >> 16) & 0xFFU));
    crc = crc_x25_byte(crc, MAVLINK_RC_CRC_EXTRA);

    out[off++] = (uint8_t)(crc & 0xFFU);
    out[off++] = (uint8_t)(crc >> 8);

    return off;
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

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static int sse_send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == 0) {
            return -1;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return -1;
        }
        return -1;
    }
    return 0;
}

static int open_sse_listener(const char *bind_spec)
{
    char *host = NULL;
    char *port = NULL;
    if (parse_host_port(bind_spec, &host, &port) < 0) {
        fprintf(stderr, "Invalid SSE bind '%s' (use host:port or [ipv6]:port)\n", bind_spec);
        return -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const char *host_arg = host;
    if (host && (!host[0] || strcmp(host, "*") == 0)) {
        host_arg = NULL;
    }

    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host_arg, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "SSE getaddrinfo(%s,%s): %s\n", host ? host : "*", port, gai_strerror(rc));
        free(host);
        free(port);
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int one = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            if (listen(fd, 4) == 0) {
                set_nonblock(fd);
                break;
            }
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    free(host);
    free(port);

    if (fd < 0) {
        perror("sse listen");
        return -1;
    }
    return fd;
}

static int sse_handshake(int fd, const char *path)
{
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[1024];
    size_t used = 0;
    while (used < sizeof(req) - 1) {
        ssize_t n = recv(fd, req + used, (sizeof(req) - 1) - used, 0);
        if (n > 0) {
            used += (size_t)n;
            req[used] = '\0';
            if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) {
                break;
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        return -1;
    }

    req[used] = '\0';
    char *line_end = strpbrk(req, "\r\n");
    if (line_end) {
        *line_end = '\0';
    }

    if (strncmp(req, "GET ", 4) != 0) {
        return -1;
    }
    char *uri = req + 4;
    char *space = strchr(uri, ' ');
    if (!space) {
        return -1;
    }
    *space = '\0';

    if (path && path[0]) {
        size_t path_len = strlen(path);
        if (strncmp(uri, path, path_len) != 0 ||
            (uri[path_len] != '\0' && uri[path_len] != '?' && uri[path_len] != '#')) {
            fprintf(stderr, "SSE request for unexpected path '%s'\n", uri);
            return -1;
        }
    }

    static const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n";
    static const char *hello = ": joystick2crfs\n\n";

    if (sse_send_all(fd, headers, strlen(headers)) < 0) {
        return -1;
    }
    if (sse_send_all(fd, hello, strlen(hello)) < 0) {
        return -1;
    }
    set_nonblock(fd);
    return 0;
}

static int sse_accept_pending(int listen_fd, int *client_fd, const char *path)
{
    if (listen_fd < 0) {
        return 0;
    }

    struct sockaddr_storage addr;
    socklen_t addrlen = (socklen_t)sizeof(addr);
    int cfd = accept(listen_fd, (struct sockaddr *)&addr, &addrlen);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        perror("sse accept");
        return -1;
    }

    int prev_fd = *client_fd;

    if (sse_handshake(cfd, path) < 0) {
        static const char *reject =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)sse_send_all(cfd, reject, strlen(reject));
        close(cfd);
        return 0;
    }

    if (prev_fd >= 0) {
        close(prev_fd);
    }

    *client_fd = cfd;
    fprintf(stderr, "SSE client connected\n");
    return 1;
}

static int sse_send_frame(int fd, const uint16_t ch[16], const int32_t raw[16])
{
    if (fd < 0) {
        return 0;
    }

    char buf[512];
    int off = snprintf(buf, sizeof(buf), "data: {\"channels\":[");
    if (off < 0 || off >= (int)sizeof(buf)) {
        return -1;
    }

    for (int i = 0; i < 16; i++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        i ? ",%u" : "%u", (unsigned)ch[i]);
        if (off < 0 || off >= (int)sizeof(buf)) {
            return -1;
        }
    }

    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "],\"raw\":[");
    if (off < 0 || off >= (int)sizeof(buf)) {
        return -1;
    }

    for (int i = 0; i < 16; i++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        i ? ",%d" : "%d", raw[i]);
        if (off < 0 || off >= (int)sizeof(buf)) {
            return -1;
        }
    }

    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "]}\n\n");
    if (off < 0 || off >= (int)sizeof(buf)) {
        return -1;
    }

    if (sse_send_all(fd, buf, (size_t)off) < 0) {
        return -1;
    }
    return 0;
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
    if (v <= -32768) {
        return CRSF_MIN;
    }
    if (v >= 32767) {
        return CRSF_MAX;
    }

    uint32_t shifted = (uint32_t)((int64_t)v + 32768LL);
    uint64_t scaled = (uint64_t)shifted * (uint64_t)CRSF_RANGE;
    uint32_t rounded = (uint32_t)((scaled + 32767ULL) / 65535ULL);
    uint32_t out = (uint32_t)CRSF_MIN + rounded;
    if (out > CRSF_MAX) {
        out = CRSF_MAX;
    }
    return (uint16_t)out;
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

static void build_channels(SDL_Joystick *js, const int dead[16],
                           uint16_t ch_s[16], int32_t ch_r[16],
                           int hat_count, int axis_count, int button_count)
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
    if (hat_count > 0) {
        uint8_t h = SDL_JoystickGetHat(js, 0);
        dpx = (h & SDL_HAT_RIGHT) ? 1 : (h & SDL_HAT_LEFT) ? -1 : 0;
        dpy = (h & SDL_HAT_UP) ? 1 : (h & SDL_HAT_DOWN) ? -1 : 0;
    } else if (axis_count >= 8) {
        dpx = SDL_JoystickGetAxis(js, 6) / 32767;
        dpy = -SDL_JoystickGetAxis(js, 7) / 32767;
    } else if (button_count >= 15) {
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
    cfg->protocol = PROTOCOL_CRSF;
    cfg->serial_enabled = 0;
    snprintf(cfg->serial_device, sizeof(cfg->serial_device), "%s", "/dev/ttyUSB0");
    cfg->serial_baud = 115200;
    cfg->udp_enabled = 1;
    snprintf(cfg->udp_target, sizeof(cfg->udp_target), "%s", "192.168.0.1:14550");
    cfg->sse_enabled = 0;
    snprintf(cfg->sse_bind, sizeof(cfg->sse_bind), "%s", "127.0.0.1:8070");
    snprintf(cfg->sse_path, sizeof(cfg->sse_path), "%s", "/sse");
    cfg->mavlink_sysid = 255;
    cfg->mavlink_compid = 190;
    cfg->mavlink_target_sysid = 1;
    cfg->mavlink_target_compid = 1;
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
        } else if (!strcasecmp(key, "protocol")) {
            if (!strcasecmp(val, "crsf")) {
                cfg->protocol = PROTOCOL_CRSF;
            } else if (!strcasecmp(val, "mavlink")) {
                cfg->protocol = PROTOCOL_MAVLINK;
            } else {
                fprintf(stderr, "%s:%d: protocol must be 'crsf' or 'mavlink'\n", path, lineno);
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
        } else if (!strcasecmp(key, "sse_enabled")) {
            int b;
            if (parse_bool_value(val, &b) == 0) {
                cfg->sse_enabled = b;
            }
        } else if (!strcasecmp(key, "sse_bind")) {
            snprintf(cfg->sse_bind, sizeof(cfg->sse_bind), "%s", val);
        } else if (!strcasecmp(key, "sse_path")) {
            snprintf(cfg->sse_path, sizeof(cfg->sse_path), "%s", val);
        } else if (!strcasecmp(key, "arm_toggle")) {
            int ch = atoi(val);
            if (ch >= 1 && ch <= 16) {
                cfg->arm_toggle = ch - 1;
            } else if (ch <= 0) {
                cfg->arm_toggle = -1;
            } else {
                fprintf(stderr, "%s:%d: arm_toggle must be 1-16 (or 0 to disable)\n", path, lineno);
            }
        } else if (!strcasecmp(key, "mavlink_sysid")) {
            cfg->mavlink_sysid = atoi(val);
        } else if (!strcasecmp(key, "mavlink_compid")) {
            cfg->mavlink_compid = atoi(val);
        } else if (!strcasecmp(key, "mavlink_target_sysid")) {
            cfg->mavlink_target_sysid = atoi(val);
        } else if (!strcasecmp(key, "mavlink_target_compid")) {
            cfg->mavlink_target_compid = atoi(val);
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
    if (cfg->protocol != PROTOCOL_CRSF && cfg->protocol != PROTOCOL_MAVLINK) {
        fprintf(stderr, "%s: unknown protocol, defaulting to CRSF\n", path);
        cfg->protocol = PROTOCOL_CRSF;
    }

    if (cfg->mavlink_sysid < 0 || cfg->mavlink_sysid > 255) {
        fprintf(stderr, "%s: mavlink_sysid must be 0-255; clamping\n", path);
        if (cfg->mavlink_sysid < 0) {
            cfg->mavlink_sysid = 0;
        } else {
            cfg->mavlink_sysid = 255;
        }
    }
    if (cfg->mavlink_compid < 0 || cfg->mavlink_compid > 255) {
        fprintf(stderr, "%s: mavlink_compid must be 0-255; clamping\n", path);
        if (cfg->mavlink_compid < 0) {
            cfg->mavlink_compid = 0;
        } else {
            cfg->mavlink_compid = 255;
        }
    }
    if (cfg->mavlink_target_sysid < 0 || cfg->mavlink_target_sysid > 255) {
        fprintf(stderr, "%s: mavlink_target_sysid must be 0-255; clamping\n", path);
        if (cfg->mavlink_target_sysid < 0) {
            cfg->mavlink_target_sysid = 0;
        } else {
            cfg->mavlink_target_sysid = 255;
        }
    }
    if (cfg->mavlink_target_compid < 0 || cfg->mavlink_target_compid > 255) {
        fprintf(stderr, "%s: mavlink_target_compid must be 0-255; clamping\n", path);
        if (cfg->mavlink_target_compid < 0) {
            cfg->mavlink_target_compid = 0;
        } else {
            cfg->mavlink_target_compid = 255;
        }
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

    signal(SIGINT, on_sigint);
    signal(SIGHUP, on_sighup);

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

    int exit_code = 0;

    try_rt(10);

    while (g_run) {
        config_t cfg;
        config_defaults(&cfg);
        if (config_load(&cfg, conf_path) < 0) {
            exit_code = 1;
            break;
        }
        if (cfg.rate != 50 && cfg.rate != 125 && cfg.rate != 250) {
            fprintf(stderr, "Config rate must be 50, 125, or 250\n");
            exit_code = 1;
            break;
        }

        int serial_fd = -1;
        int udp_fd = -1;
        int sse_fd = -1;
        int sse_client_fd = -1;
        struct sockaddr_storage udp_addr;
        socklen_t udp_addrlen = 0;
        SDL_Joystick *js = NULL;
        int js_axes = 0;
        int js_hats = 0;
        int js_buttons = 0;

        int fatal_error = 0;
        int restart_requested = 0;
        int restart_sleep = 0;

        if (cfg.serial_enabled && !cfg.simulation) {
            serial_fd = open_serial(cfg.serial_device, cfg.serial_baud);
            if (serial_fd < 0) {
                exit_code = 1;
                fatal_error = 1;
            }
        }

        if (!fatal_error && cfg.udp_enabled) {
            if (cfg.udp_target[0] == '\0') {
                fprintf(stderr, "UDP enabled but udp_target is empty\n");
                fprintf(stderr, "Continuing without UDP output.\n");
            } else {
                udp_fd = open_udp_target(cfg.udp_target, &udp_addr, &udp_addrlen);
                if (udp_fd < 0) {
                    fprintf(stderr, "Continuing without UDP output.\n");
                }
            }
        }

        if (!fatal_error && cfg.sse_enabled) {
            if (cfg.sse_bind[0] == '\0') {
                fprintf(stderr, "SSE enabled but sse_bind is empty\n");
                exit_code = 1;
                fatal_error = 1;
            } else {
                sse_fd = open_sse_listener(cfg.sse_bind);
                if (sse_fd < 0) {
                    exit_code = 1;
                    fatal_error = 1;
                } else {
                    fprintf(stderr, "SSE listening on %s%s\n", cfg.sse_bind, cfg.sse_path);
                }
            }
        }

        if (fatal_error) {
            if (js) {
                SDL_JoystickClose(js);
            }
            if (serial_fd >= 0) {
                close(serial_fd);
            }
            if (udp_fd >= 0) {
                close(udp_fd);
            }
            if (sse_client_fd >= 0) {
                close(sse_client_fd);
            }
            if (sse_fd >= 0) {
                close(sse_fd);
            }
            break;
        }

        if (serial_fd < 0 && udp_fd < 0 && (!cfg.sse_enabled || sse_fd < 0)) {
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

        struct timespec next_rescan;
        clock_gettime(CLOCK_MONOTONIC, &next_rescan);
        struct timespec next_tick = next_rescan;
        struct timespec next_sse_emit = timespec_add(next_rescan, 0, SSE_INTERVAL_NS);

        uint64_t loops = 0;
        uint64_t every = LOOP_HZ / (uint64_t)cfg.rate;
        if (every == 0) {
            every = 1;
        }
        uint64_t frame_count = 0;

        double t_min = 1e9, t_max = 0.0, t_sum = 0.0;
        uint64_t t_cnt = 0;
        uint64_t serial_packets = 0;
        uint64_t udp_packets = 0;
        uint64_t sse_packets = 0;
        char rxbuf[256];
        size_t rxlen = 0;

        uint8_t frame[FRAME_BUFFER_MAX];
        size_t frame_len = 0;
        uint8_t mavlink_seq = 0;

        if (cfg.protocol == PROTOCOL_CRSF) {
            frame[0] = CRSF_DEST;
            frame[1] = CRSF_FRAME_LEN;
            frame[2] = CRSF_TYPE_CHANNELS;
        }

        int arm_channel = (cfg.arm_toggle >= 0 && cfg.arm_toggle < 16) ? cfg.arm_toggle : -1;
        int arm_sticky = 0;
        int arm_press_active = 0;
        struct timespec arm_press_start = {0, 0};

        while (g_run && !restart_requested) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            if (g_reload) {
                g_reload = 0;
                fprintf(stderr, "Configuration reload requested; restarting.\n");
                restart_requested = 1;
                break;
            }

            SDL_JoystickUpdate();
            if (js && !SDL_JoystickGetAttached(js)) {
                fprintf(stderr, "Joystick %d detached\n", cfg.joystick_index);
                SDL_JoystickClose(js);
                js = NULL;
                js_axes = 0;
                js_hats = 0;
                js_buttons = 0;
                restart_requested = 1;
                restart_sleep = 1;
                break;
            }

            if (!js && timespec_cmp(&now, &next_rescan) >= 0) {
                int count = SDL_NumJoysticks();
                if (cfg.joystick_index < count) {
                    SDL_Joystick *candidate = SDL_JoystickOpen(cfg.joystick_index);
                    if (candidate) {
                        js = candidate;
                        js_axes = SDL_JoystickNumAxes(js);
                        js_hats = SDL_JoystickNumHats(js);
                        js_buttons = SDL_JoystickNumButtons(js);
                        const char *name = SDL_JoystickName(js);
                        fprintf(stderr, "Joystick %d connected: %s\n",
                                cfg.joystick_index, name ? name : "unknown");
                    } else {
                        fprintf(stderr, "Failed to open joystick %d: %s\n",
                                cfg.joystick_index, SDL_GetError());
                        restart_requested = 1;
                        restart_sleep = 1;
                        break;
                    }
                } else {
                    fprintf(stderr, "Joystick index %d unavailable (only %d detected)\n",
                            cfg.joystick_index, count);
                    restart_requested = 1;
                    restart_sleep = 1;
                    break;
                }
                next_rescan = timespec_add(now, cfg.rescan_interval, 0);
            }

            if (!js) {
                fprintf(stderr, "Joystick %d not available; restarting for rediscovery.\n",
                        cfg.joystick_index);
                restart_requested = 1;
                restart_sleep = 1;
                break;
            }

            uint16_t ch_source[16];
            int32_t raw_source[16];
            build_channels(js, cfg.dead, ch_source, raw_source,
                           js_hats, js_axes, js_buttons);

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

            if (cfg.sse_enabled && sse_fd >= 0) {
                int accepted = sse_accept_pending(sse_fd, &sse_client_fd, cfg.sse_path);
                if (accepted > 0) {
                    next_sse_emit = now;
                }
                if (sse_client_fd >= 0 && timespec_cmp(&now, &next_sse_emit) >= 0) {
                    if (sse_send_frame(sse_client_fd, ch_out, raw_out) < 0) {
                        fprintf(stderr, "SSE client disconnected\n");
                        close(sse_client_fd);
                        sse_client_fd = -1;
                    } else {
                        next_sse_emit = timespec_add(now, 0, SSE_INTERVAL_NS);
                        sse_packets++;
                    }
                }
            }

            frame_count++;
            if (frame_count >= every) {
                frame_count = 0;
                if (cfg.protocol == PROTOCOL_CRSF) {
                    pack_channels(ch_out, frame + 3);
                    frame[CRSF_FRAME_LEN + 1] = crc8(frame + 2, CRSF_FRAME_LEN - 1);
                    frame_len = CRSF_FRAME_LEN + 2;
                } else {
                    frame_len = pack_mavlink_rc_override(&cfg, ch_out, &mavlink_seq, frame);
                }

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

                if (frame_len > 0 && udp_fd >= 0) {
                    ssize_t sent = sendto(udp_fd, frame, frame_len, 0,
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
                if (frame_len > 0 && serial_fd >= 0) {
                    if (send_all(serial_fd, frame, frame_len) < 0) {
                        perror("serial write");
                        g_run = 0;
                    } else {
                        serial_packets++;
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
                        if (cfg.sse_enabled && sse_fd >= 0) {
                            printf("  sse %llu/s",
                                   (unsigned long long)sse_packets);
                        }
                        putchar('\n');
                        t_min = 1e9;
                        t_max = 0.0;
                        t_sum = 0.0;
                        t_cnt = 0;
                        serial_packets = 0;
                        udp_packets = 0;
                        sse_packets = 0;
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
            if (!g_run) {
                break;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL);
        }

        if (js) {
            SDL_JoystickClose(js);
            js = NULL;
        }
        if (serial_fd >= 0) {
            close(serial_fd);
            serial_fd = -1;
        }
        if (udp_fd >= 0) {
            close(udp_fd);
            udp_fd = -1;
        }
        if (sse_client_fd >= 0) {
            close(sse_client_fd);
            sse_client_fd = -1;
        }
        if (sse_fd >= 0) {
            close(sse_fd);
            sse_fd = -1;
        }

        if (fatal_error || !g_run) {
            break;
        }
        if (!restart_requested) {
            break;
        }
        if (restart_sleep) {
            fprintf(stderr, "Waiting 2 seconds before attempting to rediscover joystick...\n");
            struct timespec delay = { .tv_sec = 2, .tv_nsec = 0 };
            nanosleep(&delay, NULL);
        }
    }

    SDL_Quit();
    return exit_code;
}
