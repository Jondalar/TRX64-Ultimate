/*
 * server_main.c — runs trxmon's own app_main (src/main.c) on the host: the
 * firmware's tables are stood in for by POSIX sockets and the fake debug
 * block, whose machine runs a little loop. tests/trxmon_client.py (in the
 * TRX64-Ultimate repository) can then talk to it as to the board:
 *
 *   ./trxmon_server [port]      (default 4312)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "u2p_app.h"
#include "fake_hw.h"

static int s_port = 4312;

static uint32_t h_tick_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static int h_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    return n;
}

static int h_listen(uint16_t port, int backlog)
{
    (void)port;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)s_port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, backlog) < 0) {
        close(s);
        return U2P_ERR_BUSY;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    return s;
}

static int h_accept(int l)
{
    int s = accept(l, 0, 0);
    if (s < 0) {
        return U2P_ERR_BUSY;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return s;
}

static int h_peer(int h, char *buf, int len)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    if (getpeername(h, (struct sockaddr *)&a, &al) < 0) {
        return U2P_ERR_ARG;
    }
    snprintf(buf, (size_t)len, "%s:%u", inet_ntoa(a.sin_addr), (unsigned)ntohs(a.sin_port));
    return (int)strlen(buf);
}

static int h_recv(int h, void *buf, int len)
{
    ssize_t n = recv(h, buf, (size_t)len, 0);
    if (n > 0) {
        return (int)n;
    }
    if (n == 0) {
        return 0;
    }
    return ((errno == EAGAIN) || (errno == EWOULDBLOCK)) ? U2P_ERR_BUSY : U2P_ERR_IO;
}

static int h_send(int h, const void *buf, int len)
{
    const char *p = buf;
    int done = 0;
    while (done < len) {
        ssize_t n = send(h, p + done, (size_t)(len - done), 0);
        if (n > 0) {
            done += (int)n;
        } else if ((n < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
            usleep(1000);
        } else {
            return U2P_ERR_IO;
        }
    }
    return done;
}

static int h_close(int h) { return close(h) == 0 ? U2P_OK : U2P_ERR_ARG; }

static int h_wait(const int *handles, int n, uint32_t timeout_ms)
{
    fake_tick(2000); /* the machine runs between two looks at the sockets */
    struct pollfd p[16];
    for (int i = 0; i < n; i++) {
        p[i].fd = handles[i];
        p[i].events = POLLIN;
        p[i].revents = 0;
    }
    int r = poll(p, (nfds_t)n, (int)timeout_ms);
    if (r <= 0) {
        return 0;
    }
    int mask = 0;
    for (int i = 0; i < n; i++) {
        if (p[i].revents & (POLLIN | POLLHUP | POLLERR)) {
            mask |= 1 << i;
        }
    }
    return mask;
}

static int h_source(int role, char *buf, int len)
{
    (void)role;
    (void)buf;
    (void)len;
    return 0;
}

static int h_acquire(uint32_t f) { (void)f; return 0; }
static void h_release(void) {}
static void h_announce(uint16_t p) { printf("announce rpc %u\n", p); }

static void h_popup(const char *t, const char *m) { printf("popup: %s: %s\n", t, m); }
static int h_reply(const char *t) { printf("reply: %s", t); return 0; }

int main(int argc, char **argv)
{
    if (argc > 1) {
        s_port = atoi(argv[1]);
    }
    fake_reset();
    /* the machine: $c000 lda #$01 / jsr $c010 / jmp $c000 ; $c010 inx / rts */
    static const uint8_t prg[] = { 0xA9, 0x01, 0x20, 0x10, 0xC0, 0x4C, 0x00, 0xC0 };
    memcpy(F.ram + 0xC000, prg, sizeof(prg));
    F.ram[0xC010] = 0xE8;
    F.ram[0xC011] = 0x60;

    static u2p_sys_api_t sys;
    sys.size = sizeof(sys);
    sys.log = h_log;
    sys.tick_ms = h_tick_ms;
    static u2p_ui_api_t ui;
    ui.size = sizeof(ui);
    ui.popup = h_popup;
    static u2p_media_api_t media;
    media.size = sizeof(media);
    media.source = h_source;
    static u2p_net_api_t net = { sizeof(u2p_net_api_t), h_listen, h_accept, h_recv, h_send, h_close, h_wait, h_peer };
    static u2p_trx_api_t trx;
    trx.size = sizeof(trx);
    trx.regs = (volatile uint8_t *)1;
    trx.caps = 0xFF;
    trx.acquire = h_acquire;
    trx.release = h_release;
    trx.announce = h_announce;
    static u2p_api_t api;
    api.abi_major = U2P_ABI_MAJOR;
    api.abi_minor = U2P_ABI_MINOR;
    api.size = sizeof(api);
    api.firmware_version = "3.15";
    api.firmware_git = "fw1234ab";
    api.sys = &sys;
    api.ui = &ui;
    api.media = &media;
    api.net = &net;
    api.trx = &trx;
    static u2p_app_context_t ctx;
    ctx.size = sizeof(ctx);
    ctx.app_path = "host";
    ctx.has_ui = 0;
    ctx.reply = h_reply;
    printf("trxmon host server on 127.0.0.1:%d\n", s_port);
    fflush(stdout);
    return app_main(&api, &ctx);
}
