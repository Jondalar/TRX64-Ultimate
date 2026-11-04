/*
 * net_test_main.c — the one-connection rule of trxmon (T21-monitor.md §7.4) over real sockets: app_main
 * (src/main.c) runs as in server_main.c, in a thread, and this file is its WebSocket / plain TCP client.
 * Checks: a first client is served (ping with build ids and capabilities); a second one while it is open
 * is refused with the reason, as a WebSocket client (upgrade, JSON-RPC error, close 1013 + text) and as a
 * plain TCP / HTTP client (503 + text), both naming port 4312 and the holder's address; the holder is
 * unharmed; when the holder leaves the next client is served; a holder that never completes the
 * handshake does not keep the port.
 */
#define main server_main
#include "server_main.c"
#undef main

#include <pthread.h>
#include <time.h>
#include <poll.h>

static int checks, fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(c)) {                                                                                                    \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
        }                                                                                                              \
    } while (0)

static int dial(void)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)s_port);
    for (int i = 0; i < 200; i++) {
        /* a new socket for every try: on BSD a socket whose connect() was refused cannot connect again, and the
         * first try can come before the server thread listens */
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
            struct timeval tv = { 3, 0 };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return s;
        }
        close(s);
        usleep(10000);
    }
    return -1;
}

static const char UPGRADE[] = "GET /?av=0 HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

/* reads until `want` is seen or the peer closes / times out; returns the bytes read */
static int slurp(int s, char *buf, int cap, const char *want)
{
    int n = 0;
    while (n < cap - 1) {
        ssize_t r = recv(s, buf + n, (size_t)(cap - 1 - n), 0);
        if (r <= 0) {
            break;
        }
        n += (int)r;
        buf[n] = 0;
        if (want && memmem(buf, (size_t)n, want, strlen(want))) {
            break;
        }
    }
    buf[n] = 0;
    return n;
}

/* strstr over bytes that may hold NULs (a frame header) */
#define HAS(buf, n, str) (memmem((buf), (size_t)(n), (str), strlen(str)) != 0)

static void ws_send_text(int s, const char *t)
{
    uint8_t f[4096];
    int n = (int)strlen(t);
    int h = 0;
    f[h++] = 0x81;
    f[h++] = (uint8_t)(0x80 | n); /* n < 126 */
    const uint8_t mask[4] = { 1, 2, 3, 4 };
    memcpy(f + h, mask, 4);
    h += 4;
    for (int i = 0; i < n; i++) {
        f[h + i] = (uint8_t)t[i] ^ mask[i & 3];
    }
    send(s, f, (size_t)(h + n), 0);
}

/* one unmasked server text frame (the 101 head is read already): returns its payload length, text in out */
static int ws_read_text(int s, char *out, int cap)
{
    uint8_t b[8192];
    int have = 0;
    int need = 2;
    int hl = 2;
    int len = 0;
    while (have < need) {
        ssize_t r = recv(s, b + have, sizeof(b) - (size_t)have, 0);
        if (r <= 0) {
            return -1;
        }
        have += (int)r;
        if (have >= 2) {
            len = b[1];
            hl = 2;
            if (len == 126 && have >= 4) {
                len = (b[2] << 8) | b[3];
                hl = 4;
            } else if (len == 126) {
                continue;
            } else if (len == 127) {
                return -1;
            }
            need = hl + len;
        }
    }
    snprintf(out, (size_t)cap, "%.*s", len, (char *)b + hl);
    return len;
}

/* Reads for ms with poll; answers every server ping (0x89) with a masked pong. Returns the pings seen, -1 when the
 * server closed the connection. */
static int serve_pings(int s, int ms, int pong)
{
    int pings = 0;
    for (int waited = 0; waited < ms;) {
        struct pollfd p = { s, POLLIN, 0 };
        int r = poll(&p, 1, 50);
        waited += 50;
        if (r <= 0) {
            continue;
        }
        uint8_t b[256];
        ssize_t n = recv(s, b, sizeof(b), 0);
        if (n <= 0) {
            return -1;
        }
        for (ssize_t i = 0; i + 1 < n;) {
            int len = b[i + 1] & 0x7F;
            if (b[i] == 0x89) {
                pings++;
                if (pong) {
                    const uint8_t pg[] = { 0x8A, 0x80, 1, 2, 3, 4 };
                    send(s, pg, sizeof(pg), 0);
                }
            }
            i += 2 + len;
        }
    }
    return pings;
}

static void *server_thread(void *arg)
{
    (void)arg;
    char *argv[] = { "net_test", 0 };
    server_main(1, argv);
    return 0;
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    s_port = 20000 + (int)(((unsigned)getpid() * 2654435761u + (unsigned)time(0)) % 20000u); /* not the same port every run: a listener left over from an earlier one would refuse this one */
    pthread_t th;
    pthread_create(&th, 0, server_thread, 0);

    /* 0. a connection that never completes the handshake holds the port for HANDSHAKE_MS only */
    int idle = dial();
    CHECK(idle >= 0);
    char all[8192];
    int n = 0;
    {
        int p = dial();
        n = slurp(p, all, sizeof(all), 0);
        CHECK(HAS(all, n, "HTTP/1.1 503 ") && HAS(all, n, "port 4312 is held by 127.0.0.1:"));
        close(p);
    }
    usleep(5500000);
    n = slurp(idle, all, sizeof(all), 0); /* the server dropped it */
    CHECK(n == 0);
    close(idle);

    /* 1. the first client is served; its ping carries the build ids and the capabilities */
    int a = dial();
    CHECK(a >= 0);
    send(a, UPGRADE, sizeof(UPGRADE) - 1, 0);
    char head[1024];
    slurp(a, head, sizeof(head), "\r\n\r\n");
    CHECK(strstr(head, "101 Switching Protocols") != 0);
    ws_send_text(a, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
    char msg[8192];
    CHECK(ws_read_text(a, msg, sizeof(msg)) > 0);
    printf("ping: %.400s...\n", msg);
    CHECK(strstr(msg, "\"version\":\"trxmon hosttest\"") != 0);
    CHECK(strstr(msg, "\"firmware_git\":\"fw1234ab\"") != 0);
    CHECK(strstr(msg, "\"capabilities\":{\"methods\":[\"ping\"") != 0);

    /* 2. a WebSocket client while it is open: upgrade, error, close 1013 + reason */
    int b = dial();
    CHECK(b >= 0);
    send(b, UPGRADE, sizeof(UPGRADE) - 1, 0);
    n = slurp(b, all, sizeof(all), 0); /* until the server closes */
    printf("WS refusal (%d bytes): %s\n", n, all);
    CHECK(HAS(all, n, "101 Switching Protocols"));
    CHECK(HAS(all, n, "\"id\":null,\"error\":{\"code\":-32001,\"message\":\"port 4312 is held by 127.0.0.1:"));
    char *body = strstr(all, "\r\n\r\n");
    CHECK(body != 0);
    if (body) {
        /* text frame (0x81, len), then close frame (0x88, len, 0x03 0xF5 = 1013, reason) */
        uint8_t *f = (uint8_t *)body + 4;
        int tl = f[1];
        int fh = 2;
        if (tl == 126) { /* the error text is longer than 125 bytes */
            tl = (f[2] << 8) | f[3];
            fh = 4;
        }
        CHECK(f[0] == 0x81 && tl > 100);
        uint8_t *c = f + fh + tl;
        CHECK(c[0] == 0x88 && c[1] > 2);
        CHECK(c[2] == 0x03 && c[3] == 0xF5);
        CHECK(memcmp(c + 4, "port 4312 is held by 127.0.0.1:", 31) == 0);
        printf("close 1013, reason: %.*s\n", c[1] - 2, c + 4);
    }
    close(b);

    /* 3. a plain HTTP client */
    b = dial();
    send(b, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 27, 0);
    n = slurp(b, all, sizeof(all), 0);
    printf("HTTP refusal: %s", all);
    CHECK(HAS(all, n, "HTTP/1.1 503 "));
    CHECK(HAS(all, n, "port 4312 is held by 127.0.0.1:"));
    close(b);

    /* 4. a plain TCP client that says nothing hears the same after a second */
    b = dial();
    n = slurp(b, all, sizeof(all), 0);
    CHECK(HAS(all, n, "HTTP/1.1 503 ") && HAS(all, n, "port 4312 is held by"));
    close(b);

    /* 5. the holder is unharmed */
    ws_send_text(a, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"session/state\"}");
    char r[8192];
    CHECK(slurp(a, r, sizeof(r), "runState") > 0);

    /* 5b. keepalive (built with KEEPALIVE_IDLE_MS 1500, DEAD 3000): a holder that sends nothing and answers no ping
     * is dropped, and the next client is served without waiting for TCP to give up */
    {
        int pings = serve_pings(a, 2000, 0); /* the ping comes at 1.5 s; no pong */
        CHECK(pings >= 1);
        int closed = serve_pings(a, 3000, 0);
        CHECK(closed == -1); /* the server dropped it (it also closed its end) */
        int e = dial();
        send(e, UPGRADE, sizeof(UPGRADE) - 1, 0);
        slurp(e, head, sizeof(head), "\r\n\r\n");
        CHECK(strstr(head, "101 Switching Protocols") != 0);
        ws_send_text(e, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"ping\"}");
        CHECK(ws_read_text(e, msg, sizeof(msg)) > 0);
        /* 5c. a holder that pongs stays, however long it is silent otherwise */
        int pings2 = serve_pings(e, 5000, 1);
        CHECK(pings2 >= 2);
        int d = dial();
        send(d, UPGRADE, sizeof(UPGRADE) - 1, 0);
        n = slurp(d, all, sizeof(all), 0);
        CHECK(HAS(all, n, "port 4312 is held by 127.0.0.1:"));
        close(d);
        close(e);
    }

    /* 6. the holder leaves; the next connection is accepted (retry: the server notices the close a tick later) */
    close(a);
    int ok = 0;
    for (int i = 0; i < 50 && !ok; i++) {
        usleep(50000);
        int c = dial();
        send(c, UPGRADE, sizeof(UPGRADE) - 1, 0);
        slurp(c, head, sizeof(head), "\r\n\r\n");
        ws_send_text(c, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}");
        if (ws_read_text(c, msg, sizeof(msg)) > 0 && strstr(msg, "\"runtime_version\"")) {
            ok = 1;
            /* 7. and it is the holder now: another is refused again */
            int d = dial();
            send(d, UPGRADE, sizeof(UPGRADE) - 1, 0);
            n = slurp(d, all, sizeof(all), 0);
            CHECK(HAS(all, n, "port 4312 is held by 127.0.0.1:"));
            close(d);
            ws_send_text(c, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"trxmon/quit\"}");
            slurp(c, r, sizeof(r), 0);
        }
        close(c);
    }
    CHECK(ok);

    pthread_join(th, 0);
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
