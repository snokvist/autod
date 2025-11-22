/**
 * joystick2crsf.c - SDL joystick to CRSF bridge with UDP/SSE outputs
 *
 * The utility samples the selected joystick at 250 Hz, maps its controls to
 * 16 CRSF channels, and streams the packed frames to a UDP peer. Runtime
 * behaviour is configured exclusively via a config
 * file (default: /etc/joystick2crsf.conf).
 */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include <SDL2/SDL.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
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
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ------------------------------------------------------------------------- */
#define NS_PER_SEC         1000000000L
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

#define KEY_TRIGGER_HIGH    1700
#define KEY_TRIGGER_LOW     1500
#define KEY_TRIGGER_NEG_HIGH (CRSF_MIN + (CRSF_MAX - KEY_TRIGGER_HIGH))
#define KEY_TRIGGER_NEG_LOW  (CRSF_MIN + (CRSF_MAX - KEY_TRIGGER_LOW))
#define KEY_LONG_DEFAULT_MS 700

#define DEFAULT_CONF       "/etc/joystick2crsf.conf"
#define MAX_LINE_LEN       512

/* ------------------------------------------------------------------------- */
typedef struct {
    int rate;                   /* 50 | 125 | 250 Hz */
    int stats;                  /* print timing stats */
    int channels;               /* print channels */
    int protocol;               /* PROTOCOL_* selector */
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
    int use_gamecontroller;
    int key_short[16];
    int key_long[16];
    int key_short_low[16];
    int key_long_low[16];
    int key_long_threshold_ms;
    int key_debug;
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
    static const char *hello = ": joystick2crsf\n\n";

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

static inline int32_t normalize_trigger_axis(int32_t v)
{
    if (v >= 0) {
        int32_t scaled = (v * 2) - 32768;
        if (scaled < -32768) {
            scaled = -32768;
        } else if (scaled > 32767) {
            scaled = 32767;
        }
        return scaled;
    }
    return v;
}

static void build_channels_joystick(SDL_Joystick *js, const int dead[16],
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

static void build_channels_gamecontroller(SDL_GameController *gc,
                                          SDL_Joystick *js,
                                          const int dead[16],
                                          uint16_t ch_s[16], int32_t ch_r[16],
                                          int hat_count, int axis_count, int button_count)
{
    int32_t left_x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    int32_t left_y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
    int32_t right_x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
    int32_t right_y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);

    ch_r[0] = clip_dead(left_x, dead[0]);
    ch_r[1] = clip_dead(left_y, dead[1]);
    ch_r[2] = clip_dead(right_x, dead[2]);
    ch_r[3] = clip_dead(right_y, dead[3]);

    ch_s[0] = scale_axis(ch_r[0]);
    ch_s[1] = scale_axis(-ch_r[1]);
    ch_s[2] = scale_axis(ch_r[2]);
    ch_s[3] = scale_axis(-ch_r[3]);

    int32_t trigger_left = normalize_trigger_axis(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
    int32_t trigger_right = normalize_trigger_axis(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));

    ch_r[4] = clip_dead(trigger_left, dead[4]);
    ch_r[5] = clip_dead(trigger_right, dead[5]);
    ch_s[4] = scale_axis(ch_r[4]);
    ch_s[5] = scale_axis(ch_r[5]);

    int dpx = 0, dpy = 0;
    int up = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP);
    int down = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    int left = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    int right = SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    if (up || down || left || right) {
        dpy = up ? 1 : down ? -1 : 0;
        dpx = left ? -1 : right ? 1 : 0;
    } else if (js && hat_count > 0) {
        uint8_t h = SDL_JoystickGetHat(js, 0);
        dpx = (h & SDL_HAT_RIGHT) ? 1 : (h & SDL_HAT_LEFT) ? -1 : 0;
        dpy = (h & SDL_HAT_UP) ? 1 : (h & SDL_HAT_DOWN) ? -1 : 0;
    } else if (js && axis_count >= 8) {
        dpx = SDL_JoystickGetAxis(js, 6) / 32767;
        dpy = -SDL_JoystickGetAxis(js, 7) / 32767;
    } else if (js && button_count >= 15) {
        dpy = SDL_JoystickGetButton(js, 11) ? 1 : SDL_JoystickGetButton(js, 12) ? -1 : 0;
        dpx = SDL_JoystickGetButton(js, 13) ? -1 : SDL_JoystickGetButton(js, 14) ? 1 : 0;
    }

    int32_t dpx_axis = dpx * 32767;
    int32_t dpy_axis = dpy * 32767;
    ch_r[6] = dpx_axis;
    ch_r[7] = dpy_axis;
    ch_s[6] = scale_axis(dpx_axis);
    ch_s[7] = scale_axis(dpy_axis);

    static const SDL_GameControllerButton button_order[8] = {
        SDL_CONTROLLER_BUTTON_A,
        SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X,
        SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_BACK,
        SDL_CONTROLLER_BUTTON_START
    };
    for (int i = 0; i < 8; i++) {
        int pressed = SDL_GameControllerGetButton(gc, button_order[i]) ? 1 : 0;
        if (!pressed && js && i < button_count) {
            pressed = SDL_JoystickGetButton(js, i);
        }
        ch_r[8 + i] = pressed;
        ch_s[8 + i] = scale_bool(pressed);
    }
}

static void build_channels(SDL_GameController *gc, SDL_Joystick *js,
                           const int dead[16], uint16_t ch_s[16], int32_t ch_r[16],
                           int hat_count, int axis_count, int button_count)
{
    if (gc) {
        build_channels_gamecontroller(gc, js, dead, ch_s, ch_r, hat_count, axis_count, button_count);
    } else if (js) {
        build_channels_joystick(js, dead, ch_s, ch_r, hat_count, axis_count, button_count);
    } else {
        memset(ch_s, 0, sizeof(uint16_t) * 16);
        memset(ch_r, 0, sizeof(int32_t) * 16);
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

static int keycode_from_name(const char *name)
{
    if (!name || !*name) {
        return -1;
    }

    if (strlen(name) == 1 && isalpha((unsigned char)name[0])) {
        char lower = (char)tolower((unsigned char)name[0]);
        return KEY_A + (lower - 'a');
    }

    if (!strcasecmp(name, "up")) {
        return KEY_UP;
    }
    if (!strcasecmp(name, "down")) {
        return KEY_DOWN;
    }
    if (!strcasecmp(name, "left")) {
        return KEY_LEFT;
    }
    if (!strcasecmp(name, "right")) {
        return KEY_RIGHT;
    }
    if (!strcasecmp(name, "enter") || !strcasecmp(name, "return")) {
        return KEY_ENTER;
    }

    return -1;
}

static const char *keycode_name(int code)
{
    switch (code) {
    case KEY_UP: return "up";
    case KEY_DOWN: return "down";
    case KEY_LEFT: return "left";
    case KEY_RIGHT: return "right";
    case KEY_ENTER: return "enter";
    default:
        if (code >= KEY_A && code <= KEY_Z) {
            static char buf[2];
            buf[0] = (char)('a' + (code - KEY_A));
            buf[1] = '\0';
            return buf;
        }
        return "unknown";
    }
}

static void parse_key_binding(const char *val, int *dst, const char *path, int lineno)
{
    int code = keycode_from_name(val);
    if (code < 0) {
        fprintf(stderr, "%s:%d: unsupported key '%s' (use up,down,left,right,enter,a-z)\n",
                path, lineno, val);
        return;
    }
    *dst = code;
}

static void config_defaults(config_t *cfg)
{
    cfg->rate = 125;
    cfg->stats = 0;
    cfg->channels = 0;
    cfg->protocol = PROTOCOL_CRSF;
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
    cfg->use_gamecontroller = 1;
    cfg->key_long_threshold_ms = KEY_LONG_DEFAULT_MS;
    cfg->key_debug = 0;
    for (int i = 0; i < 16; i++) {
        cfg->map[i] = i;
        cfg->invert[i] = 0;
        cfg->dead[i] = 0;
        cfg->key_short[i] = -1;
        cfg->key_long[i] = -1;
        cfg->key_short_low[i] = -1;
        cfg->key_long_low[i] = -1;
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
        } else if (!strcasecmp(key, "use_gamecontroller")) {
            int b;
            if (parse_bool_value(val, &b) == 0) {
                cfg->use_gamecontroller = b;
            }
        } else if (!strcasecmp(key, "key_long_threshold_ms")) {
            cfg->key_long_threshold_ms = atoi(val);
        } else if (!strcasecmp(key, "key_debug")) {
            int b;
            if (parse_bool_value(val, &b) == 0) {
                cfg->key_debug = b;
            }
        } else if (!strncasecmp(key, "key_short_low_", 14)) {
            int ch = atoi(key + 14);
            if (ch >= 1 && ch <= 16) {
                parse_key_binding(val, &cfg->key_short_low[ch - 1], path, lineno);
            } else {
                fprintf(stderr, "%s:%d: key_short_low_N expects 1-16\n", path, lineno);
            }
        } else if (!strncasecmp(key, "key_short_", 10)) {
            int ch = atoi(key + 10);
            if (ch >= 1 && ch <= 16) {
                parse_key_binding(val, &cfg->key_short[ch - 1], path, lineno);
            } else {
                fprintf(stderr, "%s:%d: key_short_N expects 1-16\n", path, lineno);
            }
        } else if (!strncasecmp(key, "key_long_low_", 13)) {
            int ch = atoi(key + 13);
            if (ch >= 1 && ch <= 16) {
                parse_key_binding(val, &cfg->key_long_low[ch - 1], path, lineno);
            } else {
                fprintf(stderr, "%s:%d: key_long_low_N expects 1-16\n", path, lineno);
            }
        } else if (!strncasecmp(key, "key_long_", 9)) {
            int ch = atoi(key + 9);
            if (ch >= 1 && ch <= 16) {
                parse_key_binding(val, &cfg->key_long[ch - 1], path, lineno);
            } else {
                fprintf(stderr, "%s:%d: key_long_N expects 1-16\n", path, lineno);
            }
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
    if (cfg->key_long_threshold_ms <= 0) {
        cfg->key_long_threshold_ms = KEY_LONG_DEFAULT_MS;
    }
    return 0;
}

/* --------------------------- Uinput helpers -------------------------------- */
static int uinput_emit_event(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    return (int)write(fd, &ev, sizeof(ev));
}

static void uinput_send_key(int fd, uint16_t code)
{
    if (fd < 0) {
        return;
    }
    uinput_emit_event(fd, EV_KEY, code, 1);
    uinput_emit_event(fd, EV_SYN, SYN_REPORT, 0);
    uinput_emit_event(fd, EV_KEY, code, 0);
    uinput_emit_event(fd, EV_SYN, SYN_REPORT, 0);
}

static int uinput_open_keyboard(void)
{
    static const uint16_t supported_keys[] = {
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER,
        KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
        KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
        KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
    };

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("uinput open");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
        perror("uinput EV_KEY");
        close(fd);
        return -1;
    }
    for (size_t i = 0; i < sizeof(supported_keys) / sizeof(supported_keys[0]); i++) {
        if (ioctl(fd, UI_SET_KEYBIT, supported_keys[i]) < 0) {
            perror("uinput KEYBIT");
            close(fd);
            return -1;
        }
    }

#ifdef UI_SET_EVBIT
    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0) {
        perror("uinput EV_SYN");
        close(fd);
        return -1;
    }
#endif

#ifdef UI_DEV_SETUP
    struct uinput_setup usetup;
    memset(&usetup, 0, sizeof(usetup));
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "joystick2crsf-keys");
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1d50;
    usetup.id.product = 0x615e;
    usetup.id.version = 1;
    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("uinput setup");
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("uinput create");
        close(fd);
        return -1;
    }
#else
    struct uinput_user_dev udev;
    memset(&udev, 0, sizeof(udev));
    snprintf(udev.name, UINPUT_MAX_NAME_SIZE, "joystick2crsf-keys");
    udev.id.bustype = BUS_USB;
    udev.id.vendor = 0x1d50;
    udev.id.product = 0x615e;
    udev.id.version = 1;
    if (write(fd, &udev, sizeof(udev)) < 0) {
        perror("uinput write");
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("uinput create");
        close(fd);
        return -1;
    }
#endif

    return fd;
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

static int64_t timespec_diff_ns(const struct timespec *start, const struct timespec *end)
{
    int64_t sec = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t nsec = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    if (nsec < 0) {
        sec -= 1;
        nsec += NS_PER_SEC;
    }
    return sec * NS_PER_SEC + nsec;
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

    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
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

        int key_fd = -1;
        int key_enabled = 0;
        for (int i = 0; i < 16; i++) {
            if (cfg.key_short[i] >= 0 || cfg.key_long[i] >= 0 ||
                cfg.key_short_low[i] >= 0 || cfg.key_long_low[i] >= 0) {
                key_enabled = 1;
                break;
            }
        }
        if (key_enabled) {
            key_fd = uinput_open_keyboard();
            if (key_fd < 0) {
                fprintf(stderr, "Keyboard bindings requested but /dev/uinput is unavailable; disabling.\n");
                key_enabled = 0;
            }
        }

        int udp_fd = -1;
        int sse_fd = -1;
        int sse_client_fd = -1;
        struct sockaddr_storage udp_addr;
        socklen_t udp_addrlen = 0;
        SDL_GameController *gc = NULL;
        SDL_Joystick *js = NULL;
        int js_owned = 0;
        int js_axes = 0;
        int js_hats = 0;
        int js_buttons = 0;

        int fatal_error = 0;
        int restart_requested = 0;
        int restart_sleep = 0;

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
            if (gc) {
                SDL_GameControllerClose(gc);
                gc = NULL;
            } else if (js && js_owned) {
                SDL_JoystickClose(js);
            }
            js = NULL;
            js_owned = 0;
            if (udp_fd >= 0) {
                close(udp_fd);
            }
            if (sse_client_fd >= 0) {
                close(sse_client_fd);
            }
            if (sse_fd >= 0) {
                close(sse_fd);
            }
            if (key_fd >= 0) {
                ioctl(key_fd, UI_DEV_DESTROY);
                close(key_fd);
            }
            break;
        }

        if (udp_fd < 0 && (!cfg.sse_enabled || sse_fd < 0)) {
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
        struct timespec next_frame = next_rescan;
        struct timespec next_sse_emit = timespec_add(next_rescan, 0, SSE_INTERVAL_NS);

        const long frame_interval_ns = NS_PER_SEC / (long)cfg.rate;

        double t_min = 1e9, t_max = 0.0, t_sum = 0.0;
        uint64_t t_cnt = 0;
        uint64_t udp_packets = 0;
        uint64_t sse_packets = 0;

        struct timespec stats_window_start = next_rescan;
        struct timespec last_loop = next_rescan;

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

        int key_press_active[16] = {0};
        int key_press_low_active[16] = {0};
        struct timespec key_press_start[16];
        struct timespec key_press_low_start[16];
        memset(key_press_start, 0, sizeof(key_press_start));
        memset(key_press_low_start, 0, sizeof(key_press_low_start));

        while (g_run && !restart_requested) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            struct timespec deadline = next_frame;
            if (!js && timespec_cmp(&next_rescan, &deadline) < 0) {
                deadline = next_rescan;
            }
            if (cfg.sse_enabled && sse_client_fd >= 0 &&
                timespec_cmp(&next_sse_emit, &deadline) < 0) {
                deadline = next_sse_emit;
            }

            int wait_ms = 0;
            if (timespec_cmp(&now, &deadline) < 0) {
                int64_t wait_ns = timespec_diff_ns(&now, &deadline);
                if (wait_ns > 0) {
                    wait_ms = (int)((wait_ns + 999999L) / 1000000L);
                }
            }

            SDL_Event ev;
            int have_event = SDL_WaitEventTimeout(&ev, wait_ms);
            if (have_event) {
                if (ev.type == SDL_JOYDEVICEADDED ||
                    ev.type == SDL_JOYDEVICEREMOVED ||
                    ev.type == SDL_CONTROLLERDEVICEADDED ||
                    ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    next_rescan = now;
                }
            }

            clock_gettime(CLOCK_MONOTONIC, &now);

            if (!have_event && timespec_cmp(&now, &deadline) < 0) {
                continue;
            }

            if (g_reload) {
                g_reload = 0;
                fprintf(stderr, "Configuration reload requested; restarting.\n");
                restart_requested = 1;
                break;
            }

            SDL_GameControllerUpdate();
            SDL_JoystickUpdate();
            if (gc && !SDL_GameControllerGetAttached(gc)) {
                fprintf(stderr, "Game controller %d detached\n", cfg.joystick_index);
                SDL_GameControllerClose(gc);
                gc = NULL;
                js = NULL;
                js_owned = 0;
                js_axes = 0;
                js_hats = 0;
                js_buttons = 0;
                restart_requested = 1;
                restart_sleep = 1;
                break;
            }
            if (!gc && js && !SDL_JoystickGetAttached(js)) {
                fprintf(stderr, "Joystick %d detached\n", cfg.joystick_index);
                if (js_owned) {
                    SDL_JoystickClose(js);
                }
                js = NULL;
                js_owned = 0;
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
                    const char *name = NULL;
                    int has_mapping = SDL_IsGameController(cfg.joystick_index);
                    if (cfg.use_gamecontroller && has_mapping) {
                        SDL_GameController *candidate = SDL_GameControllerOpen(cfg.joystick_index);
                        if (candidate) {
                            gc = candidate;
                            js = SDL_GameControllerGetJoystick(gc);
                            js_owned = 0;
                            if (!js) {
                                fprintf(stderr, "Game controller %d missing joystick backend\n",
                                        cfg.joystick_index);
                                SDL_GameControllerClose(gc);
                                gc = NULL;
                                restart_requested = 1;
                                restart_sleep = 1;
                                break;
                            }
                            js_axes = SDL_JoystickNumAxes(js);
                            js_hats = SDL_JoystickNumHats(js);
                            js_buttons = SDL_JoystickNumButtons(js);
                            name = SDL_GameControllerName(gc);
                            if (!name) {
                                name = SDL_JoystickName(js);
                            }
                            fprintf(stderr, "Game controller %d connected: %s\n",
                                    cfg.joystick_index, name ? name : "unknown");
                        } else {
                            fprintf(stderr, "Failed to open game controller %d: %s\n",
                                    cfg.joystick_index, SDL_GetError());
                            restart_requested = 1;
                            restart_sleep = 1;
                            break;
                        }
                    } else {
                        SDL_Joystick *candidate = SDL_JoystickOpen(cfg.joystick_index);
                        if (candidate) {
                            js = candidate;
                            js_owned = 1;
                            js_axes = SDL_JoystickNumAxes(js);
                            js_hats = SDL_JoystickNumHats(js);
                            js_buttons = SDL_JoystickNumButtons(js);
                            name = SDL_JoystickName(js);
                            const char *reason = NULL;
                            if (!cfg.use_gamecontroller && has_mapping) {
                                reason = "game controller mapping disabled";
                            } else if (cfg.use_gamecontroller && !has_mapping) {
                                reason = "no game controller mapping";
                            }
                            if (reason) {
                                fprintf(stderr, "Joystick %d connected (%s): %s\n",
                                        cfg.joystick_index, reason, name ? name : "unknown");
                            } else {
                                fprintf(stderr, "Joystick %d connected: %s\n",
                                        cfg.joystick_index, name ? name : "unknown");
                            }
                        } else {
                            fprintf(stderr, "Failed to open joystick %d: %s\n",
                                    cfg.joystick_index, SDL_GetError());
                            restart_requested = 1;
                            restart_sleep = 1;
                            break;
                        }
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
            build_channels(gc, js, cfg.dead, ch_source, raw_source,
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

            if (key_enabled && key_fd >= 0) {
                for (int i = 0; i < 16; i++) {
                    if (cfg.key_short[i] >= 0 || cfg.key_long[i] >= 0) {
                        int pressed = ch_out[i] >= KEY_TRIGGER_HIGH;
                        if (pressed) {
                            if (!key_press_active[i]) {
                                key_press_start[i] = now;
                                key_press_active[i] = 1;
                            }
                        } else if (key_press_active[i] && ch_out[i] <= KEY_TRIGGER_LOW) {
                            int64_t held = timespec_diff_ms(&key_press_start[i], &now);
                            int code = -1;
                            if (cfg.key_long[i] >= 0 && held >= cfg.key_long_threshold_ms) {
                                code = cfg.key_long[i];
                            } else if (cfg.key_short[i] >= 0) {
                                code = cfg.key_short[i];
                            } else if (cfg.key_long[i] >= 0) {
                                code = cfg.key_long[i];
                            }
                            if (code >= 0) {
                                uinput_send_key(key_fd, (uint16_t)code);
                                if (cfg.key_debug) {
                                    const char *kind = (held >= cfg.key_long_threshold_ms) ? "long" : "short";
                                    fprintf(stderr, "CH%d %s press (%lld ms) -> key %s\n",
                                            i + 1, kind, (long long)held, keycode_name(code));
                                }
                            }
                            key_press_active[i] = 0;
                        }
                    }

                    if (cfg.key_short_low[i] >= 0 || cfg.key_long_low[i] >= 0) {
                        int pressed_low = ch_out[i] <= KEY_TRIGGER_NEG_HIGH;
                        if (pressed_low) {
                            if (!key_press_low_active[i]) {
                                key_press_low_start[i] = now;
                                key_press_low_active[i] = 1;
                            }
                        } else if (key_press_low_active[i] && ch_out[i] >= KEY_TRIGGER_NEG_LOW) {
                            int64_t held = timespec_diff_ms(&key_press_low_start[i], &now);
                            int code = -1;
                            if (cfg.key_long_low[i] >= 0 && held >= cfg.key_long_threshold_ms) {
                                code = cfg.key_long_low[i];
                            } else if (cfg.key_short_low[i] >= 0) {
                                code = cfg.key_short_low[i];
                            } else if (cfg.key_long_low[i] >= 0) {
                                code = cfg.key_long_low[i];
                            }
                            if (code >= 0) {
                                uinput_send_key(key_fd, (uint16_t)code);
                                if (cfg.key_debug) {
                                    const char *kind = (held >= cfg.key_long_threshold_ms) ? "long" : "short";
                                    fprintf(stderr, "CH%d low %s press (%lld ms) -> key %s\n",
                                            i + 1, kind, (long long)held, keycode_name(code));
                                }
                            }
                            key_press_low_active[i] = 0;
                        }
                    }
                }
            }

            int cmp = timespec_cmp(&now, &next_frame);
            int ready_for_frame = (cmp >= 0);
            if (have_event && cmp < 0) {
                ready_for_frame = 1;
                next_frame = now;
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

            if (ready_for_frame) {
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
            }

            if (cfg.stats) {
                double dt = (double)timespec_diff_ns(&last_loop, &now) / 1e9;
                if (dt > 0.0) {
                    if (dt < t_min) {
                        t_min = dt;
                    }
                    if (dt > t_max) {
                        t_max = dt;
                    }
                    t_sum += dt;
                    t_cnt++;
                    if (timespec_diff_ms(&stats_window_start, &now) >= 1000) {
                        printf("loop min %.3f  max %.3f  avg %.3f ms",
                               t_min * 1e3, t_max * 1e3,
                               (t_sum / (double)t_cnt) * 1e3);
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
                        udp_packets = 0;
                        sse_packets = 0;
                        stats_window_start = now;
                    }
                }
                last_loop = now;
            }

            if (timespec_cmp(&now, &next_frame) >= 0) {
                do {
                    next_frame = timespec_add(next_frame, 0, frame_interval_ns);
                } while (timespec_cmp(&now, &next_frame) >= 0);
            }
        }

        if (gc) {
            SDL_GameControllerClose(gc);
            gc = NULL;
            js = NULL;
            js_owned = 0;
        } else if (js) {
            if (js_owned) {
                SDL_JoystickClose(js);
            }
            js = NULL;
            js_owned = 0;
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
        if (key_fd >= 0) {
            ioctl(key_fd, UI_DEV_DESTROY);
            close(key_fd);
            key_fd = -1;
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
