// sse_tail.c — ultra-tiny SSE HTTP server that runs a command and streams its
// stdout/stderr as Server-Sent Events (JSON per line), no external deps.
//
// Build:   gcc -O2 -std=c11 -Wall -Wextra -o sse_tail sse_tail.c
// Usage:   ./sse_tail [-p PORT] [-h HOST] -- <program> [args...]
// Example: ./sse_tail -p 8080 -- tail -f /var/log/syslog
//          curl -N http://127.0.0.1:8080/events
//
// Notes:
//  * Multiple subscribers supported (simple select() loop).
//  * Each completed line is pushed as one SSE frame {ts, stream, line}.
//  * CORS enabled so a browser UI can connect directly.
//  * Heartbeats (SSE comments) keep idle connections alive.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS    64
#define REQ_BUFSZ      2048
#define LINE_BUFSZ     4096
#define OUT_BUFSZ      8192
#define HEARTBEAT_MS   15000
#define LIFO_DEFAULT   20
#define LIFO_THROTTLE_DEFAULT_MS 1000
#define STREAM_NAME_MAX 128

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_stop_sig = 0;
static void on_sig(int sig) { g_stop = 1; g_stop_sig = sig; }

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static uint64_t now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

struct queued_line {
    char *line;
    uint64_t ts;
};

struct line_queue {
    struct queued_line **items;
    size_t cap;
    size_t count;
    size_t head;
};

static void queue_init(struct line_queue *q, size_t cap) {
    q->cap = cap;
    q->count = 0;
    q->head = 0;
    q->items = cap ? calloc(cap, sizeof(*q->items)) : NULL;
    if (cap && !q->items) q->cap = 0;
}

static void queue_free(struct line_queue *q) {
    if (!q->items) return;
    for (size_t i = 0; i < q->cap; i++) {
        if (q->items[i]) {
            free(q->items[i]->line);
            free(q->items[i]);
        }
    }
    free(q->items);
    q->items = NULL;
    q->cap = 0;
    q->count = 0;
    q->head = 0;
}

static void queue_push(struct line_queue *q, const char *line, size_t len) {
    if (!q->cap) return;

    struct queued_line *ql = calloc(1, sizeof(*ql));
    if (!ql) return;
    ql->line = strndup(line, len);
    if (!ql->line) { free(ql); return; }
    ql->ts = now_ms();

    if (q->count == q->cap) {
        struct queued_line *old = q->items[q->head];
        if (old) {
            free(old->line);
            free(old);
        }
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }

    size_t pos = (q->head + q->count) % q->cap;
    q->items[pos] = ql;
    q->count++;
}

// JSON-escape: escapes ", \, \n, \r, \t and control chars (<0x20). Returns bytes written.
static size_t json_escape(const char *in, size_t inlen, char *out, size_t outcap) {
    size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)in[i];
        const char *rep = NULL;
        char tmp[7];
        switch (c) {
            case '\"': rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            default:
                if (c < 0x20) {
                    // \u00XX
                    (void)snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
                    rep = tmp;
                }
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl >= outcap) break;
            memcpy(out + o, rep, rl); o += rl;
        } else {
            if (o + 1 >= outcap) break;
            out[o++] = (char)c;
        }
    }
    if (o < outcap) out[o] = '\0';
    return o;
}

struct client {
    int fd;
    uint64_t last_send_ms;
};

static int send_str(int fd, const char *s) {
    size_t off = 0;
    size_t total = strlen(s);
    while (off < total) {
        ssize_t w = send(fd, s + off, total - off, MSG_NOSIGNAL);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static int send_heartbeat(struct client *c, uint64_t now) {
    // SSE comment line (just a colon) + blank line
    static const char *hb = ":\n\n";
    if (send_str(c->fd, hb) < 0) return -1;
    c->last_send_ms = now;
    return 0;
}

static const char *HTTP_HEADERS =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "X-Accel-Buffering: no\r\n"
    "\r\n";

static const char *HTTP_404 =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 10\r\n"
    "\r\n"
    "Not Found\n";

static const char *HTTP_ROOT =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-cache\r\n"
    "\r\n"
    "<!doctype html><meta charset=\"utf-8\"><title>sse_tail</title>"
    "<style>body{font-family:system-ui,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
    "background:#0b0d10;color:#eaeef2;padding:20px}code{background:#11151a;"
    "padding:2px 6px;border-radius:6px}</style>"
    "<h1>sse_tail</h1><p>Subscribe at <code>/events</code>. "
    "Example: <code>curl -N http://HOST:PORT/events</code></p>";

static int open_listener(const char *host, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int yes = 1; (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!host || strcmp(host, "0.0.0.0") == 0) addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid host %s\n", host); close(fd); return -1;
    }
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, 16) < 0) { perror("listen"); close(fd); return -1; }
    (void)set_nonblock(fd);
    return fd;
}

static void drop_client(struct client *clients, int *nclients, int idx) {
    if (idx < 0 || idx >= *nclients) return;
    close(clients[idx].fd);
    if (idx != *nclients - 1) clients[idx] = clients[*nclients - 1];
    (*nclients)--;
}

static int accept_http_or_404(int lfd, struct client *clients, int *nclients) {
    struct sockaddr_in ca; socklen_t cl = (socklen_t)sizeof(ca);
    int cfd = accept(lfd, (struct sockaddr*)&ca, &cl);
    if (cfd < 0) return -1;

    // Block for a short time to receive the first request line
    struct timeval to = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    char req[REQ_BUFSZ];
    int got = 0;
    // Read until we see a newline or buffer is full or timeout
    for (;;) {
        ssize_t r = recv(cfd, req + got, (sizeof(req) - 1) - got, 0);
        if (r > 0) {
            got += (int)r;
            if (memchr(req, '\n', (size_t)got)) break;
            if (got >= (int)sizeof(req) - 1) break;
            continue;
        } else if (r == 0) {
            break; // peer closed
        } else {
            if (errno == EINTR) continue;
            // timeout or other recv error
            close(cfd);
            return -1;
        }
    }
    req[got] = '\0';

    // Extract first line
    char *line_end = strpbrk(req, "\r\n");
    if (line_end) *line_end = '\0';

    // Debug
    // fprintf(stderr, "HTTP req: %s\n", req);

    // Parse: expect "GET <path> ..."
    const char *p = req;
    if (strncmp(p, "GET ", 4) != 0) {
        (void)send_str(cfd, HTTP_404);
        close(cfd);
        return 0;
    }
    p += 4;
    const char *sp = strchr(p, ' ');
    size_t path_len = sp ? (size_t)(sp - p) : strlen(p);

    bool is_root = (path_len == 1 && p[0] == '/');
    bool is_health = (path_len == 7 && strncmp(p, "/health", 7) == 0);
    bool is_events = false;
    if (path_len >= 7 && strncmp(p, "/events", 7) == 0) {
        if (path_len == 7) is_events = true;
        else {
            char c = p[7];
            if (c == '/' || c == '?') is_events = true;
        }
    }

    if (is_health) {
        const char *ok =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Cache-Control: no-cache\r\n"
            "Content-Length: 3\r\n"
            "\r\n"
            "ok\n";
        (void)send_str(cfd, ok);
        close(cfd);
        return 0;
    }

    if (is_events) {
        // Switch to non-blocking ONLY after we decide to keep it
        set_nonblock(cfd);
        (void)send_str(cfd, HTTP_HEADERS);
        // Optional: advertise retry interval to EventSource clients
        (void)send_str(cfd, "retry: 2000\n\n");
        if (*nclients < MAX_CLIENTS) {
            clients[*nclients].fd = cfd;
            clients[*nclients].last_send_ms = now_ms();
            (*nclients)++;
        } else {
            close(cfd);
        }
    } else if (is_root) {
        (void)send_str(cfd, HTTP_ROOT);
        close(cfd);
    } else {
        (void)send_str(cfd, HTTP_404);
        close(cfd);
    }
    return 0;
}



static void broadcast(struct client *clients, int *nclients,
                      const char *event, uint64_t id, const char *json) {
    for (int i = 0; i < *nclients; ) {
        char frame[OUT_BUFSZ];
        int fl = snprintf(frame, sizeof(frame),
                          "event: %s\n"
                          "id: %llu\n"
                          "data: %s\n"
                          "\n",
                          event, (unsigned long long)id, json);
        if (fl < 0) fl = 0;
        if ((size_t)fl >= sizeof(frame)) fl = (int)sizeof(frame) - 1;
        frame[(size_t)fl] = '\0';
        if (send_str(clients[i].fd, frame) < 0) {
            drop_client(clients, nclients, i);
            continue;
        }
        clients[i].last_send_ms = now_ms();
        i++;
    }
}

static void queue_drain_lifo(struct line_queue *q, const char *stream,
                             struct client *clients, int *nclients,
                             uint64_t *msg_id) {
    while (q->count > 0) {
        size_t idx = (q->head + q->count - 1) % q->cap;
        struct queued_line *ql = q->items[idx];
        if (ql) {
            char esc[LINE_BUFSZ * 2];
            size_t ll = strlen(ql->line);
            (void)json_escape(ql->line, ll, esc, sizeof(esc));
            char json[OUT_BUFSZ];
            int jl = snprintf(json, sizeof(json),
                              "{\"ts\":%llu,\"stream\":\"%s\",\"line\":\"%s\"}",
                              (unsigned long long)ql->ts, stream, esc);
            if (jl < 0) jl = 0;
            json[(size_t)jl] = '\0';
            broadcast(clients, nclients, stream, ++(*msg_id), json);
        }
        if (ql) {
            free(ql->line);
            free(ql);
        }
        q->items[idx] = NULL;
        q->count--;
    }
    q->head = 0;
}

static void flush_line(const char *stream, char *buf, size_t *len,
                       bool lifo_mode, struct line_queue *q,
                       struct client *clients, int *nclients, uint64_t *msg_id) {
    if (*len == 0) return;
    size_t L = *len;
    if (L && buf[L - 1] == '\n') L--;  // trim trailing newline
    if (lifo_mode) {
        queue_push(q, buf, L);
    } else {
        char esc[LINE_BUFSZ * 2];
        (void)json_escape(buf, L, esc, sizeof(esc));
        char json[OUT_BUFSZ];
        int jl = snprintf(json, sizeof(json),
                          "{\"ts\":%llu,\"stream\":\"%s\",\"line\":\"%s\"}",
                          (unsigned long long)now_ms(), stream, esc);
        if (jl < 0) jl = 0;
        json[(size_t)jl] = '\0';
        broadcast(clients, nclients, stream, ++(*msg_id), json);
    }
    *len = 0;
}

int main(int argc, char **argv) {
    const char *host = "0.0.0.0";
    uint16_t port = 8080;
    bool lifo_mode = false;
    size_t lifo_cap = LIFO_DEFAULT;
    uint64_t lifo_throttle_ms = LIFO_THROTTLE_DEFAULT_MS;
    const char *prog_name = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "p:h:l:Lt:n:")) != -1) {
        switch (opt) {
            case 'p': port = (uint16_t)atoi(optarg); break;
            case 'h': host = optarg; break;
            case 'L': lifo_mode = true; break;
            case 'l': lifo_mode = true; lifo_cap = (size_t)atoi(optarg); if (lifo_cap == 0) lifo_cap = 1; break;
            case 't':
                lifo_mode = true;
                lifo_throttle_ms = (uint64_t)strtoull(optarg, NULL, 10);
                if (lifo_throttle_ms == 0) lifo_throttle_ms = 1;
                break;
            case 'n':
                prog_name = optarg;
                break;
            default:
                fprintf(stderr,
                        "Usage: %s [-p PORT] [-h HOST] [-L] [-l N] [-t MS] [-n NAME] -- <program> [args...]\n"
                        "  -L       enable LIFO drop mode (default cap %d)\n"
                        "  -l N     set LIFO cap to N and enable drop mode\n"
                        "  -t MS    throttle LIFO flushes to once every MS (default %d)\n"
                        "  -n NAME  append NAME to stream identifiers (stdout:NAME, stderr:NAME, status:NAME)\n",
                        argv[0], LIFO_DEFAULT, LIFO_THROTTLE_DEFAULT_MS);
                return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "Missing program after --\n");
        return 1;
    }

    char stream_out[STREAM_NAME_MAX];
    char stream_err[STREAM_NAME_MAX];
    char stream_status[STREAM_NAME_MAX];
    if (prog_name && prog_name[0]) {
        (void)snprintf(stream_out, sizeof(stream_out), "stdout:%s", prog_name);
        (void)snprintf(stream_err, sizeof(stream_err), "stderr:%s", prog_name);
        (void)snprintf(stream_status, sizeof(stream_status), "status:%s", prog_name);
    } else {
        (void)snprintf(stream_out, sizeof(stream_out), "stdout");
        (void)snprintf(stream_err, sizeof(stream_err), "stderr");
        (void)snprintf(stream_status, sizeof(stream_status), "status");
    }

    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    int outp[2], errp[2];
    if (pipe(outp) < 0 || pipe(errp) < 0) { perror("pipe"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        // Child: redirect stdout/stderr to pipes, exec program
        close(outp[0]); close(errp[0]);
        dup2(outp[1], STDOUT_FILENO);
        dup2(errp[1], STDERR_FILENO);
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IOLBF, 0);
        execvp(argv[optind], &argv[optind]);
        perror("execvp");
        _exit(127);
    }

    // Parent
    close(outp[1]); close(errp[1]);
    (void)set_nonblock(outp[0]);
    (void)set_nonblock(errp[0]);

    int lfd = open_listener(host, port);
    if (lfd < 0) return 1;

    struct client clients[MAX_CLIENTS];
    int nclients = 0;

    struct line_queue q_out; memset(&q_out, 0, sizeof(q_out));
    struct line_queue q_err; memset(&q_err, 0, sizeof(q_err));
    if (lifo_mode) {
        queue_init(&q_out, lifo_cap);
        queue_init(&q_err, lifo_cap);
    }

    char oline[LINE_BUFSZ] = {0}; size_t olen = 0;
    char eline[LINE_BUFSZ] = {0}; size_t elen = 0;
    uint64_t msg_id = 0;

    fprintf(stderr, "sse_tail: listening on %s:%u (pid=%d)\n", host, port, (int)getpid());

    uint64_t last_hb = now_ms();
    uint64_t next_lifo_flush = lifo_mode ? last_hb + lifo_throttle_ms : 0;
    bool child_done = false;
    int child_status = 0;

    for (;;) {
        if (g_stop) break;

        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = lfd; FD_SET(lfd, &rfds);
        FD_SET(outp[0], &rfds); if (outp[0] > maxfd) maxfd = outp[0];
        FD_SET(errp[0], &rfds); if (errp[0] > maxfd) maxfd = errp[0];

        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200 * 1000; // 200ms
        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (FD_ISSET(lfd, &rfds)) {
            while (accept_http_or_404(lfd, clients, &nclients) == 0) {}
        }

        if (FD_ISSET(outp[0], &rfds)) {
            char tmp[1024]; ssize_t r = read(outp[0], tmp, sizeof(tmp));
            if (r > 0) {
                for (ssize_t i = 0; i < r; i++) {
                    if (olen + 1 >= sizeof(oline)) {
                        oline[olen++] = '\n';
                        flush_line(stream_out, oline, &olen, lifo_mode, &q_out,
                                   clients, &nclients, &msg_id);
                    }
                    oline[olen++] = tmp[i];
                    if (tmp[i] == '\n') flush_line(stream_out, oline, &olen, lifo_mode, &q_out,
                                                  clients, &nclients, &msg_id);
                }
            }
        }

        if (FD_ISSET(errp[0], &rfds)) {
            char tmp[1024]; ssize_t r = read(errp[0], tmp, sizeof(tmp));
            if (r > 0) {
                for (ssize_t i = 0; i < r; i++) {
                    if (elen + 1 >= sizeof(eline)) {
                        eline[elen++] = '\n';
                        flush_line(stream_err, eline, &elen, lifo_mode, &q_err,
                                   clients, &nclients, &msg_id);
                    }
                    eline[elen++] = tmp[i];
                    if (tmp[i] == '\n') flush_line(stream_err, eline, &elen, lifo_mode, &q_err,
                                                  clients, &nclients, &msg_id);
                }
            }
        }

        uint64_t now = now_ms();

        if (lifo_mode && now >= next_lifo_flush) {
            queue_drain_lifo(&q_out, stream_out, clients, &nclients, &msg_id);
            queue_drain_lifo(&q_err, stream_err, clients, &nclients, &msg_id);
            next_lifo_flush = now + lifo_throttle_ms;
        }
        if (now - last_hb >= HEARTBEAT_MS) {
            for (int i = 0; i < nclients; ) {
                if (send_heartbeat(&clients[i], now) < 0) {
                    drop_client(clients, &nclients, i);
                    continue;
                }
                i++;
            }
            last_hb = now;
        }

        int status = 0; pid_t pr = waitpid(pid, &status, WNOHANG);
        if (pr == pid) {
            child_done = true;
            child_status = status;
            break;
        }
    }

    if (olen) {
        oline[olen++] = '\n';
        flush_line(stream_out, oline, &olen, lifo_mode, &q_out, clients, &nclients, &msg_id);
    }
    if (elen) {
        eline[elen++] = '\n';
        flush_line(stream_err, eline, &elen, lifo_mode, &q_err, clients, &nclients, &msg_id);
    }
    if (lifo_mode) {
        queue_drain_lifo(&q_out, stream_out, clients, &nclients, &msg_id);
        queue_drain_lifo(&q_err, stream_err, clients, &nclients, &msg_id);
    }

    char json[256];
    if (child_done) {
        (void)snprintf(json, sizeof(json),
                       "{\"ts\":%llu,\"stream\":\"%s\",\"line\":\"child exited (%d)\"}",
                       (unsigned long long)now_ms(), stream_status, child_status);
        broadcast(clients, &nclients, stream_status, ++msg_id, json);
    } else {
        const char *sigdesc = g_stop_sig ? strsignal(g_stop_sig) : "";
        if (g_stop_sig && sigdesc && sigdesc[0]) {
            (void)snprintf(json, sizeof(json),
                           "{\"ts\":%llu,\"stream\":\"%s\",\"line\":\"sse_tail stopping (signal %d: %s)\"}",
                           (unsigned long long)now_ms(), stream_status, g_stop_sig, sigdesc);
        } else if (g_stop_sig) {
            (void)snprintf(json, sizeof(json),
                           "{\"ts\":%llu,\"stream\":\"%s\",\"line\":\"sse_tail stopping (signal %d)\"}",
                           (unsigned long long)now_ms(), stream_status, g_stop_sig);
        } else {
            (void)snprintf(json, sizeof(json),
                           "{\"ts\":%llu,\"stream\":\"%s\",\"line\":\"sse_tail stopping\"}",
                           (unsigned long long)now_ms(), stream_status);
        }
        broadcast(clients, &nclients, stream_status, ++msg_id, json);
    }

    if (child_done) {
        (void)waitpid(pid, NULL, 0);
    }

    for (int i = 0; i < nclients; i++) close(clients[i].fd);
    close(lfd); close(outp[0]); close(errp[0]);
    if (lifo_mode) {
        queue_free(&q_out);
        queue_free(&q_err);
    }
    kill(0, SIGTERM);
    return 0;
}
