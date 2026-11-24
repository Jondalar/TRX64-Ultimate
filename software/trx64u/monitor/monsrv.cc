// The RPC server of the monitor library (monsrv.h). PROVENANCE: the network half of trxmon's main.c (TREX/1541ultimate:apps/trxmon/src; Gideon
// Zweijtzer's firmware with the TRX64 line's additions, GPL-3.0) as MultiCore's core application had it in monsrv.cc (TRX64U-MultiCore 3b8e2955), here over
// the base's net table (api->net) as main.c is over the firmware's. Changed against main.c: no blocking wait (the library's poll calls monsrv_poll), the
// holder's buffers come from api->mem while it is connected, the confirmation gate (gate_refused); the dispatch is trxmon's rpc.c unchanged (srv_send,
// srv_broadcast, srv_cork, srv_holder_idle_ms).
#include "libapi.h"
#include "monsrv.h"
#include "monitor.h"
#include "monlib.h"
#include <stddef.h>

extern "C" {
#include "trxmon/mlib.h"
#include "trxmon/mjson.h"
#include "trxmon/ws.h"
#include "trxmon/rpc.h"
#include "trxmon/mach.h"
}

#define RPC_PORT      4312
#define MAX_REFUSE    2                  // connections being refused at the same time
#define REFUSE_RX     1024
#define REFUSE_MS     1000               // a refused client that sends no request head is answered after this long
#define HANDSHAKE_MS  5000               // the holder must finish the HTTP upgrade within this, or it does not hold the port
#define KEEPALIVE_IDLE_MS 20000          // a holder that vanished sends no FIN: after this much silence a ping, after KEEPALIVE_DEAD_MS it is gone
#define KEEPALIVE_DEAD_MS 50000
#define RX_SIZE       (20 * 1024)
#define MSG_SIZE      (16 * 1024)
#define SEND_SLOW_MS  1000u              // a holder that stopped reading fills the send buffer: the first send that took this long drops it
#define CORK_MAX      3072
#define NET_NONE      (-5)               // SvcNet recv: nothing waiting
#define GATE_TOK      512                // rpc.c's MAX_TOK: what rpc_request can parse, the gate parses too

namespace {

// The holder, allocated from api->mem while it is connected (39 KB)
struct Client {
    int h;
    int ws, dead, rxlen, msglen;
    uint32_t t0, t_rx, t_ping;
    char peer[24];
    int cork_n;
    uint8_t cork[CORK_MAX];
    uint8_t rx[RX_SIZE];
    char msg[MSG_SIZE + 1];
};

struct Refuse {
    int h;                               // -1: free
    int rxlen;
    uint32_t t0;
    char rx[REFUSE_RX];
};

Client *C;                               // the one holder
Refuse X[MAX_REFUSE];                    // set free by monsrv_start(): in use only while the listener stands
int s_listener = -1;
bool s_taken;                            // the port could not be bound
int s_cork_on;

void log_line(const char *a, const char *b, uint32_t n, const char *c)
{
    char t[200];
    sbuf_t s;
    sb_init(&s, t, sizeof(t));
    sb_put(&s, a);
    if (b) {
        sb_put(&s, b);
        sb_dec(&s, n);
        sb_put(&s, c);
    }
    monlib_log(sb_str(&s));
}

// 0 bytes: closed by the peer; -1: nothing now; -2: error
int net_recv(int h, void *buf, int len)
{
    int n = g_api->net->recv(h, buf, len);
    if (n >= 0) {
        return n;
    }
    return n == NET_NONE ? -1 : -2;
}

// All len bytes (the base's send waits while the send buffer drains), or -2
int net_send(int h, const void *buf, int len)
{
    return g_api->net->send(h, buf, len) == len ? len : -2;
}

void net_close(int h) { g_api->net->close(h); }

void net_peer(int h, char *out, int n)
{
    out[0] = 0;
    if (g_api->net->peer && g_api->net->peer(h, out, n) < 0) {
        out[0] = 0;
    }
}

int send_all(Client *c, const void *p, int n)
{
    if (c->dead) {
        return -1;
    }
    uint32_t t0 = plat_ms();
    if (net_send(c->h, p, n) < 0) {
        c->dead = 1;
        return -1;
    }
    if ((uint32_t)(plat_ms() - t0) >= SEND_SLOW_MS) {
        char t[80];
        sbuf_t s;
        sb_init(&s, t, sizeof(t));
        sb_put(&s, "the RPC holder ");
        sb_put(&s, c->peer);
        log_line(sb_str(&s), " does not read (a send took ", plat_ms() - t0, " ms): dropped");
        c->dead = 1;
        return -1;
    }
    return 0;
}

// Every packet costs the network stack a few milliseconds: a frame goes out in one send (header and payload together), and frames sent between srv_cork(1) and srv_cork(0) in one more.
void cork_flush(void)
{
    if (C && C->cork_n && C->h >= 0) {
        send_all(C, C->cork, C->cork_n);
    }
    if (C) {
        C->cork_n = 0;
    }
}

void send_frame(Client *c, int opcode, const void *p, int n)
{
    uint8_t buf[10 + 1400];
    int hl = ws_frame_header(buf, opcode, (uint64_t)n);
    if (n <= 1400) {
        memcpy(buf + hl, p, (size_t)n);
        if (s_cork_on) {
            if (c->cork_n + hl + n > CORK_MAX) {
                cork_flush();
            }
            if (hl + n <= CORK_MAX) {
                memcpy(c->cork + c->cork_n, buf, (size_t)(hl + n));
                c->cork_n += hl + n;
                return;
            }
        }
        send_all(c, buf, hl + n);
        return;
    }
    if (send_all(c, buf, hl) == 0 && n > 0) {
        send_all(c, p, n);
    }
}

// D19a on the monitor's socket (T192 M1): it has no token and cannot hold a question, so while the core's UI service gates the network (the state
// net.gated) the methods of monsrv_gated() are refused with the reason. A core without the state is as before.
bool gated(void)
{
    const char *v = g_api->ops && g_api->ops->state ? g_api->ops->state("net.gated", 0) : 0;
    return v && v[0] && strcmp(v, "0") && strcmp(v, "no");
}

bool gate_refused(Client *c)
{
    if (!gated()) {
        return false;
    }
    jtok_t *t = (jtok_t *)g_api->mem->malloc(GATE_TOK * sizeof(jtok_t));
    if (!t) {                                              // the request cannot be read: refused, so nothing gated slips through
        static const char nomem[] = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,\"message\":\"no memory to check the request against the "
                                    "confirmation gate\"}}";
        send_frame(c, WS_TEXT, nomem, (int)sizeof(nomem) - 1);
        return true;
    }
    bool refused = false;
    int n = json_parse(c->msg, c->msglen, t, GATE_TOK);
    int mi = n > 0 && json_is(t, 0, JT_OBJ) ? json_get(c->msg, t, 0, "method") : -1;
    char m[64];                                            // rpc.c's: a method rpc_request can read, the gate reads too
    if (mi >= 0 && json_str(c->msg, t, mi, m, sizeof(m)) >= 0 && monsrv_gated(m)) {
        refused = true;
        char em[110], eid[80], id[40];
        monsrv_json_esc(m, em, sizeof(em));
        int ii = json_get(c->msg, t, 0, "id");
        bool str = false;
        memcpy(eid, "null", 5);
        if (ii >= 0 && json_is(t, ii, JT_STR) && json_str(c->msg, t, ii, id, sizeof(id)) >= 0) {
            monsrv_json_esc(id, eid, sizeof(eid));
            str = true;
        } else if (ii >= 0 && json_is(t, ii, JT_NUM) && t[ii].end - t[ii].start < (int)sizeof(eid)) {
            memcpy(eid, c->msg + t[ii].start, (size_t)(t[ii].end - t[ii].start));     // a number is echoed as written, anything else answers null
            eid[t[ii].end - t[ii].start] = 0;
        }
        char b[360];
        sbuf_t s;
        sb_init(&s, b, sizeof(b));
        sb_put(&s, "{\"jsonrpc\":\"2.0\",\"id\":");
        sb_put(&s, str ? "\"" : "");
        sb_put(&s, eid);
        sb_put(&s, str ? "\"" : "");
        sb_put(&s, ",\"error\":{\"code\":-32001,\"message\":\"");
        sb_put(&s, em);
        sb_put(&s, " changes the machine: it needs the confirmation gate (POST /v1/ops with the token); this socket has no token\"}}");
        if (!s.full) {
            send_frame(c, WS_TEXT, b, s.len);
        }
    }
    g_api->mem->free(t);
    return refused;
}

void drop_client(void)
{
    if (!C) {
        return;
    }
    cork_flush();
    if (C->h >= 0) {
        net_close(C->h);
    }
    C->h = -1;
    rpc_client_gone(0);
    g_api->mem->free(C);
    C = 0;
}

void close_ws(Client *c, int code)
{
    uint8_t b[2] = { (uint8_t)(code >> 8), (uint8_t)code };
    send_frame(c, WS_CLOSE, b, 2);
    c->dead = 1;
}

void http_head(Client *c)
{
    http_req_t r;
    int n = ws_http_parse((const char *)c->rx, c->rxlen, &r);
    if (n == 0) {
        return;
    }
    if (n < 0 || !r.upgrade) {
        static const char no[] = "HTTP/1.1 426 Upgrade Required\r\nUpgrade: websocket\r\nConnection: close\r\n"
                                 "Content-Type: text/plain\r\n\r\n"
                                 "trxmon: the TRX64 runtime contract, WebSocket JSON-RPC 2.0 (ws://<host>:4312/?av=0)\n";
        send_all(c, no, (int)sizeof(no) - 1);
        c->dead = 1;
        return;
    }
    char out[256];
    int ol = ws_http_accept(&r, out, sizeof(out));
    send_all(c, out, ol);
    c->ws = 1;
    memmove(c->rx, c->rx + n, (size_t)(c->rxlen - n));
    c->rxlen -= n;
}

void ws_frames(Client *c)
{
    while (!c->dead) {
        ws_frame_t f;
        int h = ws_frame_parse(c->rx, c->rxlen, &f);
        if (h == 0) {
            if (c->rxlen >= RX_SIZE) {
                close_ws(c, 1009);
            }
            return;
        }
        if (h < 0 || !f.masked) {
            close_ws(c, 1002);
            return;
        }
        if (f.len > (uint64_t)(RX_SIZE - h)) {
            close_ws(c, 1009);
            return;
        }
        int total = h + (int)f.len;
        if (c->rxlen < total) {
            return;                                              // the payload is not all here yet
        }
        uint8_t *p = c->rx + h;
        ws_unmask(p, (int)f.len, f.mask);
        switch (f.opcode) {
        case WS_TEXT:
        case WS_CONT:
            if (c->msglen + (int)f.len > MSG_SIZE) {
                close_ws(c, 1009);
                return;
            }
            memcpy(c->msg + c->msglen, p, (size_t)f.len);
            c->msglen += (int)f.len;
            if (f.fin) {
                c->msg[c->msglen] = 0;
                if (!mon_claim()) {                              // no debug block in this bitstream: nothing to talk to
                    close_ws(c, 1011);
                    return;
                }
                if (gate_refused(c)) {
                    c->msglen = 0;
                    break;
                }
                rpc_request(0, c->msg, c->msglen);
                c->msglen = 0;
            }
            break;
        case WS_PING: send_frame(c, WS_PONG, p, (int)f.len); break;
        case WS_CLOSE:
            send_frame(c, WS_CLOSE, p, (int)(f.len >= 2 ? 2 : 0));
            c->dead = 1;
            break;
        default: break;                                          // binary and pong frames are not for us
        }
        memmove(c->rx, c->rx + total, (size_t)(c->rxlen - total));
        c->rxlen -= total;
    }
}

void client_read(Client *c)
{
    int room = RX_SIZE - c->rxlen;
    if (room <= 0) {
        c->dead = 1;
        return;
    }
    int n = net_recv(c->h, c->rx + c->rxlen, room);
    if (n == -1) {
        return;
    }
    if (n <= 0) {
        c->dead = 1;
        return;
    }
    c->rxlen += n;
    c->t_rx = plat_ms();
    if (!c->ws) {
        http_head(c);
    }
    if (c->ws) {
        ws_frames(c);
    }
}

// The text a refused client reads: the port, who holds it, how to get in
int refuse_text(char *out, int n)
{
    sbuf_t b;
    sb_init(&b, out, n);
    sb_put(&b, "port 4312 is held");
    if (C && C->peer[0]) {
        sb_put(&b, " by ");
        sb_put(&b, C->peer);
    }
    sb_put(&b, " (trxmon serves one RPC connection at a time)");
    sb_str(&b);
    return b.len;
}

// The refusal over the answer a client can read: a WebSocket client gets the upgrade, one JSON-RPC error (id null, code RPC_BUSY_CODE), a close frame with code 1013 and the
// reason; anything else gets HTTP 503 with the text. The connection is closed afterwards.
void refuse_answer(Refuse *x, int upgrade, const http_req_t *r)
{
    char text[160];
    refuse_text(text, sizeof(text));
    uint8_t hdr[10];
    if (upgrade) {
        char out[256];
        int ol = ws_http_accept(r, out, sizeof(out));
        net_send(x->h, out, ol);
        char msg[320];
        sbuf_t b;
        sb_init(&b, msg, sizeof(msg));
        sb_put(&b, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":" RPC_BUSY_CODE_STR ",\"message\":\"");
        sb_put(&b, text);
        sb_put(&b, "\",\"data\":{\"port\":4312}}}");
        int hl = ws_frame_header(hdr, WS_TEXT, (uint64_t)b.len);
        net_send(x->h, hdr, hl);
        net_send(x->h, msg, b.len);
        uint8_t cl[2 + 123];
        int tl = m_strlen(text);
        if (tl > 123) {
            tl = 123;
        }
        cl[0] = (uint8_t)(WS_BUSY_CLOSE >> 8);
        cl[1] = (uint8_t)WS_BUSY_CLOSE;
        memcpy(cl + 2, text, (size_t)tl);
        hl = ws_frame_header(hdr, WS_CLOSE, (uint64_t)(2 + tl));
        net_send(x->h, hdr, hl);
        net_send(x->h, cl, 2 + tl);
    } else {
        char out[400];
        sbuf_t b;
        sb_init(&b, out, sizeof(out));
        sb_put(&b, "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 5\r\nConnection: close\r\n"
                   "Content-Type: text/plain\r\nContent-Length: ");
        sb_dec(&b, (uint64_t)(m_strlen(text) + 1));
        sb_put(&b, "\r\n\r\n");
        sb_put(&b, text);
        sb_putc(&b, '\n');
        net_send(x->h, out, b.len);
    }
    net_close(x->h);
    x->h = -1;
}

void refuse_read(Refuse *x)
{
    int n = net_recv(x->h, x->rx + x->rxlen, REFUSE_RX - x->rxlen);
    if (n == -1) {
        return;
    }
    if (n <= 0) {                                                // gone before it heard anything
        net_close(x->h);
        x->h = -1;
        return;
    }
    x->rxlen += n;
    http_req_t r;
    int hn = ws_http_parse(x->rx, x->rxlen, &r);
    if (hn == 0 && x->rxlen < REFUSE_RX) {
        return;                                                  // the head is not all here yet
    }
    refuse_answer(x, hn > 0 && r.upgrade, &r);
}

void refuse_start(int h)
{
    for (int i = 0; i < MAX_REFUSE; i++) {
        if (X[i].h < 0) {
            X[i].h = h;
            X[i].rxlen = 0;
            X[i].t0 = plat_ms();
            return;
        }
    }
    net_close(h);                                                // a flood of refused connections: the extra ones are dropped
}

// The base's accept gives the connection non-blocking and with TCP_NODELAY.
void accept_one(void)
{
    int h = g_api->net->accept(s_listener);
    if (h < 0) {
        return;
    }
    if (!C) {
        C = (Client *)g_api->mem->malloc(sizeof(Client));
        if (C) {
            memset(C, 0, offsetof(Client, cork));
            C->h = h;
            C->t0 = C->t_rx = C->t_ping = plat_ms();
            net_peer(h, C->peer, sizeof(C->peer));
            return;
        }
        net_close(h);                                            // no memory for a holder
        return;
    }
    refuse_start(h);
}

}   // namespace

extern "C" {
void srv_cork(int on)
{
    if (!on) {
        cork_flush();
    }
    s_cork_on = on;
}

uint32_t srv_holder_idle_ms(void)
{
    if (C && C->h >= 0 && C->ws && !C->dead) {
        return plat_ms() - C->t_rx;
    }
    return 0xFFFFFFFFu;
}

void srv_send(int cid, const char *text, int len)
{
    if (cid == 0 && C && C->h >= 0 && C->ws) {
        send_frame(C, WS_TEXT, text, len);
    }
}

void srv_broadcast(const char *text, int len) { srv_send(0, text, len); }
}

void monsrv_start(void)
{
    if (s_listener >= 0 || !g_api->net) {
        return;
    }
    for (int i = 0; i < MAX_REFUSE; i++) {
        X[i].h = -1;
    }
    int s = g_api->net->listen(RPC_PORT, 2);
    if (s < 0) {
        if (!s_taken) {                                          // said once, not at every try
            monlib_log("TCP port 4312 is taken: no RPC");
        }
        s_taken = true;
        return;
    }
    s_taken = false;
    s_listener = s;
    if (g_api->trx && g_api->trx->announce) {
        g_api->trx->announce(RPC_PORT);                          // the port the ident service and GET /v1/info report as trx64.rpc
    }
    monlib_log("serving the TRX64 runtime contract on TCP 4312");
}

void monsrv_poll(void)
{
    if (s_listener < 0) {
        return;
    }
    accept_one();
    if (C && C->h >= 0 && !C->dead) {
        client_read(C);
    }
    uint32_t now = plat_ms();
    for (int i = 0; i < MAX_REFUSE; i++) {
        if (X[i].h < 0) {
            continue;
        }
        refuse_read(&X[i]);
        if (X[i].h >= 0 && (uint32_t)(now - X[i].t0) >= REFUSE_MS) {
            http_req_t none;
            memset(&none, 0, sizeof(none));
            refuse_answer(&X[i], 0, &none);                      // a client that never says anything: plain text
        }
    }
    if (C) {
        if (!C->ws && !C->dead && (uint32_t)(now - C->t0) >= HANDSHAKE_MS) {
            C->dead = 1;                                         // no upgrade: it must not hold the one place
        }
        if (C->ws && !C->dead) {
            if ((uint32_t)(now - C->t_rx) >= KEEPALIVE_DEAD_MS) {
                char t[80];
                sbuf_t s;
                sb_init(&s, t, sizeof(t));
                sb_put(&s, "the RPC holder ");
                sb_put(&s, C->peer);
                log_line(sb_str(&s), " sent nothing for ", now - C->t_rx, " ms and did not answer a ping: dropped");
                C->dead = 1;
            } else if ((uint32_t)(now - C->t_rx) >= KEEPALIVE_IDLE_MS && (uint32_t)(now - C->t_ping) >= KEEPALIVE_IDLE_MS) {
                send_frame(C, WS_PING, 0, 0);
                C->t_ping = now;
            }
        }
        if (C->dead) {
            drop_client();
        }
    }
}

void monsrv_stop(void)
{
    if (C) {
        if (C->ws && !C->dead) {
            close_ws(C, 1001);
        }
        drop_client();
    }
    if (s_listener >= 0) {
        for (int i = 0; i < MAX_REFUSE; i++) {
            if (X[i].h >= 0) {
                net_close(X[i].h);
                X[i].h = -1;
            }
        }
        net_close(s_listener);
        s_listener = -1;
        if (g_api->trx && g_api->trx->announce) {
            g_api->trx->announce(0);
        }
    }
}

const char *monsrv_status(void)
{
    if (s_listener >= 0) {
        return "RPC on TCP 4312";
    }
    return s_taken ? "TCP port 4312 is taken" : "";
}

bool monsrv_listening(void) { return s_listener >= 0; }
bool monsrv_has_client(void) { return C != 0; }
void monsrv_drop_client(void)
{
    if (C) {
        if (C->ws && !C->dead) {
            close_ws(C, 1001);
        }
        drop_client();
    }
}
