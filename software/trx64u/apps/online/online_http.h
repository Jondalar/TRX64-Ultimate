// The HTTP client of the online sub-app: one GET over api->net (svctab.h SvcNet: connect, send, recv, wait, close), answered as the bytes of the body. Ours: the firmware's client
// (http_request.cc, assembly.cc) sits on BSD sockets, a server library and a body parser, none of which an application image reaches. Every server gets the request assembly.cc
// sends (HTTP/1.1, "Connection: close"; for Assembly64 and CommoServe the "Client-Id" header that picks the library). The answer is read until its body is complete
// (Content-Length, chunked, or the close of the connection), so every answer a server may give is read the same way. Beyond64's API is written for HTTP/1.0 clients too, but
// its HTTP/1.0 answers have no length and end with the close, and in ue2emu (slirp) 4 of 6 pages of 20 failed on one such read each, broken after the header (U21, 2026-11-25);
// to HTTP/1.1 it answers chunked, whose last chunk ends the body before the close (550 reads, none failed).
#ifndef APPS_ONLINE_HTTP_H
#define APPS_ONLINE_HTTP_H

#include <stdint.h>
#include "svctab.h"

struct HttpEnv {
    const SvcNet *net;
    const SvcMem *mem;
    const SvcSys *sys;
    bool (*cancelled)(void *user);   // optional: asked while the answer is quiet; true gives the request up (HTTP_BROKEN)
    void *user;
};

enum HttpRc {
    HTTP_OK = 0,
    HTTP_NO_NET = -1,            // no net table, no memory, or the connection was not made (no network, name not resolved, refused, no answer)
    HTTP_BROKEN = -2,            // the connection broke or went quiet before the answer was complete, or the answer is no HTTP
    HTTP_STATUS = -3,            // the server answered, but not with 2xx: HttpOut::status says which
    HTTP_TOO_BIG = -4,           // the body is longer than the caller allows
    HTTP_NO_ROOM = -5            // no memory for the answer
};

// One request. `path` is already URL encoded and begins with "/".
struct HttpReq {
    const char *host;            // the name in the Host header, and the name connected to unless `addr` is set
    const char *addr;            // null, or the address `host` resolved to (a dotted address): a job that asks one server many times resolves it once
    const char *path;
    const char *client_id;       // null: no Client-Id header
    const char *agent;           // the User-Agent
    uint32_t max_body;           // a longer body is refused (HTTP_TOO_BIG)
};

struct HttpOut {
    uint8_t *body;               // the body, from env.mem->malloc; the caller frees it with env.mem->free. Null unless HTTP_OK.
    uint32_t len;
    int status;                  // the status code of the answer (0: none read)
    int conn;                    // HTTP_NO_NET because the connection was not made: what SvcNet::connect returned (api.md 4.8.13.4), else 0
};

// GET q.path from q.host:80. Blocks the calling task, for up to HTTP_CONNECT_MS to connect and HTTP_IDLE_MS for each quiet gap of the answer. SvcNet::connect forbids the menu task and
// the executor task (api.md 1.3.9, 4.8.13.2): the caller is the task of a job (online_search.cc starts one for every request), never the menu task or the executor task.
int http_get(const HttpEnv &env, const HttpReq &q, HttpOut *out);

#define HTTP_CONNECT_MS 10000    // the time a connection may take
#define HTTP_IDLE_MS 15000       // the time an answer may be quiet

#endif
