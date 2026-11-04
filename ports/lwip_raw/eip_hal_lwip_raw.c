/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Socket half of the QuickEIP port contract over the lwIP raw (callback) API.
 *
 * For bare-metal lwIP builds (NO_SYS=1) with no socket layer, such as the
 * Teknic ClearCore. The platform file next to it (ports/clearcore) supplies
 * the clock, the stack poll (eip_hal_yield), and interface information, plus
 * the empty lock and the adapter runner.
 *
 * lwIP callbacks only run inside the stack poll, which this file reaches
 * through eip_hal_yield() in its wait loops, so everything runs in one
 * context and nothing here needs a lock. Do not call these functions from
 * an interrupt handler.
 *
 * Handles index a fixed socket table (EIP_LWIP_MAX_SOCKETS). Received TCP
 * data stays in lwIP pbufs until read, and the receive window is reopened
 * as the library consumes it. Each UDP socket queues up to
 * EIP_LWIP_UDP_QUEUE datagrams; more are dropped.
 *
 * How the callbacks meet the blocking-style contract: the OnXxx callbacks
 * only record events in the Sock (a pbuf queued, an accepted socket, a failed
 * flag), and the contract calls spin on that state with eip_hal_yield()
 * between checks. A "blocking" call therefore runs lwIP callbacks, including
 * ones for other sockets, so state read before a yield may have changed
 * after it: a TCP socket can lose its pcb (OnTcpErr) at any yield, and each
 * loop rechecks. A receive with timeout 0 still yields once when nothing is
 * ready, so a caller that only polls keeps the stack running. In an idle
 * eip_server_poll that is one stack poll per UDP socket and TCP session.
 */
#include "eip_hal.h"

#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

/* Covers listeners, UDP sockets, sessions, and accepted sockets the library
 * has not picked up yet. accept_q stores indexes as int8_t, so keep it under
 * 128. */
#ifndef EIP_LWIP_MAX_SOCKETS
#define EIP_LWIP_MAX_SOCKETS 12
#endif
#ifndef EIP_LWIP_UDP_QUEUE
#define EIP_LWIP_UDP_QUEUE 8
#endif
#ifndef EIP_LWIP_ACCEPT_QUEUE
#define EIP_LWIP_ACCEPT_QUEUE 4
#endif

enum { kFree = 0, kTcp, kListen, kUdp };
enum { kSendBudgetMs = 500 }; /* the bound eip_hal.h asks of eip_hal_tcp_send */

/* A received datagram. The entry owns p until it is read or the socket closes. */
typedef struct {
    struct pbuf *p;
    uint8_t ip[4];
    uint16_t port;
} UdpEntry;

typedef struct {
    uint8_t kind;
    uint8_t connected;
    uint8_t failed;        /* error or peer closed */
    struct tcp_pcb *tcp;   /* 0 once OnTcpErr has run: lwIP has freed the pcb */
    struct udp_pcb *udp;
    struct pbuf *rx;       /* TCP: unread data chain */
    uint16_t rx_off;       /* TCP: bytes already read from rx head */
    uint8_t peer[4];
    int8_t accept_q[EIP_LWIP_ACCEPT_QUEUE]; /* listener: accepted socket indexes */
    uint8_t accept_n;
    UdpEntry udp_q[EIP_LWIP_UDP_QUEUE];
    uint8_t udp_head;
    uint8_t udp_n;
} Sock;

static Sock s_socks[EIP_LWIP_MAX_SOCKETS];

/* The kind check also rejects a closed handle, until Alloc reuses the slot.
 * Handles carry no generation count. */
static Sock *Get(eip_sock_t handle, uint8_t kind)
{
    if (handle < 0 || handle >= EIP_LWIP_MAX_SOCKETS || s_socks[handle].kind != kind) {
        return 0;
    }
    return &s_socks[handle];
}

static int Alloc(uint8_t kind)
{
    int i;
    for (i = 0; i < EIP_LWIP_MAX_SOCKETS; i++) {
        if (s_socks[i].kind == kFree) {
            memset(&s_socks[i], 0, sizeof s_socks[i]);
            s_socks[i].kind = kind;
            return i;
        }
    }
    return -1;
}

static void ToIpAddr(const uint8_t ip[4], ip_addr_t *out)
{
    IP_ADDR4(out, ip[0], ip[1], ip[2], ip[3]);
}

static void FromIpAddr(const ip_addr_t *addr, uint8_t ip[4])
{
    uint32_t raw = ip4_addr_get_u32(ip_2_ip4(addr)); /* network order in memory */
    memcpy(ip, &raw, 4);
}

/* Callbacks carry the table index, not a Sock pointer, as lwIP's void *arg.
 * Index() turns it back and rejects anything out of range. */
static void *Arg(int index)
{
    return (void *)(intptr_t)(index + 1); /* never 0, so a cleared arg is detectable */
}

static int Index(void *arg)
{
    int index = (int)(intptr_t)arg - 1;
    return (index >= 0 && index < EIP_LWIP_MAX_SOCKETS) ? index : -1;
}

/* ---- TCP callbacks ------------------------------------------------------ */

/* Takes ownership of p: it joins the rx chain, or is freed if the slot no
 * longer matches this pcb. tcp_recved() is not called here. The window stays
 * closed by the amount held until CopyOut consumes it, which is the
 * back-pressure on a sender outrunning the library. */
static err_t OnTcpRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    int index = Index(arg);
    Sock *s = (index >= 0) ? &s_socks[index] : 0;

    (void)err;
    if (s == 0 || s->kind != kTcp || s->tcp != pcb) {
        if (p != 0) {
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (p == 0) {
        s->failed = 1; /* orderly close from the peer */
        return ERR_OK;
    }
    if (s->rx == 0) {
        s->rx = p;
    } else {
        pbuf_cat(s->rx, p);
    }
    return ERR_OK;
}

static void OnTcpErr(void *arg, err_t err)
{
    int index = Index(arg);

    (void)err;
    if (index >= 0 && s_socks[index].kind == kTcp) {
        s_socks[index].tcp = 0; /* lwIP already freed the pcb */
        s_socks[index].failed = 1;
    }
}

static err_t OnTcpConnected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    int index = Index(arg);

    (void)pcb;
    if (index >= 0 && s_socks[index].kind == kTcp) {
        if (err == ERR_OK) {
            s_socks[index].connected = 1;
        } else {
            s_socks[index].failed = 1;
        }
    }
    return ERR_OK;
}

/* Common to connected and accepted sockets. Keep-alive is about 45 s (30 s idle
 * + 3 probes x 5 s) and frees the slot of a peer that vanished, but only in
 * builds with LWIP_TCP_KEEPALIVE. The times are in ms. */
static void SetupTcp(int index, struct tcp_pcb *pcb)
{
    s_socks[index].tcp = pcb;
    tcp_arg(pcb, Arg(index));
    tcp_recv(pcb, OnTcpRecv);
    tcp_err(pcb, OnTcpErr);
    tcp_nagle_disable(pcb);
#if LWIP_TCP_KEEPALIVE
    ip_set_option(pcb, SOF_KEEPALIVE);
    pcb->keep_idle = 30000;
    pcb->keep_intvl = 5000;
    pcb->keep_cnt = 3;
#endif
}

/* Runs inside the stack poll, so the socket is created here and parked in
 * accept_q for eip_hal_tcp_accept to collect. It is live from this moment:
 * data or an error arriving before collection lands in its Sock. A full queue
 * or table refuses the connection with tcp_abort; ERR_ABRT tells lwIP that
 * newpcb has been aborted. */
static err_t OnTcpAccept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    int lindex = Index(arg);
    Sock *listener = (lindex >= 0 && s_socks[lindex].kind == kListen) ? &s_socks[lindex] : 0;
    int index;

    if (err != ERR_OK || newpcb == 0) {
        return ERR_VAL;
    }
    if (listener == 0 || listener->accept_n >= EIP_LWIP_ACCEPT_QUEUE || (index = Alloc(kTcp)) < 0) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    SetupTcp(index, newpcb);
    s_socks[index].connected = 1;
    FromIpAddr(&newpcb->remote_ip, s_socks[index].peer);
    listener->accept_q[listener->accept_n++] = (int8_t)index;
    return ERR_OK;
}

/* Release a TCP socket and any unread data. The callbacks are detached first,
 * so nothing can reach this slot once it is reused. If tcp_close fails the pcb
 * is aborted, not leaked. */
static void FreeTcp(Sock *s)
{
    if (s->tcp != 0) {
        tcp_arg(s->tcp, 0);
        tcp_recv(s->tcp, 0);
        tcp_err(s->tcp, 0);
        if (tcp_close(s->tcp) != ERR_OK) {
            tcp_abort(s->tcp);
        }
        s->tcp = 0;
    }
    if (s->rx != 0) {
        pbuf_free(s->rx);
        s->rx = 0;
    }
    s->kind = kFree;
}

/* ---- TCP ---------------------------------------------------------------- */

eip_sock_t eip_hal_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    ip_addr_t addr;
    struct tcp_pcb *pcb;
    uint32_t start = eip_hal_millis();
    int index = Alloc(kTcp);
    Sock *s;

    if (index < 0) {
        return EIP_SOCK_INVALID;
    }
    s = &s_socks[index];
    pcb = tcp_new();
    if (pcb == 0) {
        s->kind = kFree;
        return EIP_SOCK_INVALID;
    }
    SetupTcp(index, pcb);
    ToIpAddr(ip, &addr);
    if (tcp_connect(pcb, &addr, port, OnTcpConnected) != ERR_OK) {
        FreeTcp(s);
        return EIP_SOCK_INVALID;
    }
    while (!s->connected && !s->failed) {
        if ((eip_hal_millis() - start) >= timeout_ms) {
            break;
        }
        eip_hal_yield();
    }
    if (!s->connected || s->failed) {
        /* Abort, not close, as the attempt may still be in progress. With the
         * arg cleared, an error callback from the abort finds no socket. */
        if (s->tcp != 0) {
            tcp_arg(s->tcp, 0);
            tcp_abort(s->tcp);
            s->tcp = 0;
        }
        FreeTcp(s);
        return EIP_SOCK_INVALID;
    }
    return index;
}

eip_sock_t eip_hal_tcp_listen(uint16_t port, uint8_t backlog)
{
    struct tcp_pcb *pcb;
    struct tcp_pcb *lpcb;
    int index = Alloc(kListen);

    if (index < 0) {
        return EIP_SOCK_INVALID;
    }
    pcb = tcp_new();
    if (pcb == 0 || tcp_bind(pcb, IP_ADDR_ANY, port) != ERR_OK) {
        if (pcb != 0) {
            tcp_abort(pcb);
        }
        s_socks[index].kind = kFree;
        return EIP_SOCK_INVALID;
    }
    /* On success lpcb replaces pcb, which must not be used again. On failure
     * pcb is still ours. */
    lpcb = tcp_listen_with_backlog(pcb, backlog ? backlog : 1);
    if (lpcb == 0) {
        tcp_abort(pcb);
        s_socks[index].kind = kFree;
        return EIP_SOCK_INVALID;
    }
    s_socks[index].tcp = lpcb;
    tcp_arg(lpcb, Arg(index));
    tcp_accept(lpcb, OnTcpAccept);
    return index;
}

eip_sock_t eip_hal_tcp_accept(eip_sock_t listener, uint8_t peer_ip[4])
{
    Sock *s = Get(listener, kListen);
    int index;

    if (s == 0 || s->accept_n == 0) {
        return EIP_SOCK_INVALID;
    }
    index = s->accept_q[0];
    memmove(s->accept_q, s->accept_q + 1, (size_t)(s->accept_n - 1u));
    s->accept_n--;
    memcpy(peer_ip, s_socks[index].peer, 4);
    return index;
}

/* Write what fits and poll the stack until the rest does. The budget covers
 * the whole call and is checked only when no progress was made. Data is
 * copied (TCP_WRITE_FLAG_COPY) because the caller reuses its buffer at once.
 * failed also means the peer sent FIN, so a half-closed connection cannot be
 * written to, and bytes already queued are not taken back on a failure. */
int eip_hal_tcp_send(eip_sock_t sock, const uint8_t *data, uint16_t len)
{
    Sock *s = Get(sock, kTcp);
    uint32_t start = eip_hal_millis();
    uint16_t sent = 0;

    if (s == 0) {
        return EIP_ERR_IO;
    }
    while (sent < len) {
        uint16_t room;
        uint16_t chunk;

        if (s->failed || s->tcp == 0) {
            return EIP_ERR_IO;
        }
        room = tcp_sndbuf(s->tcp);
        chunk = (uint16_t)(len - sent);
        if (chunk > room) {
            chunk = room;
        }
        if (chunk > 0 && tcp_write(s->tcp, data + sent, chunk, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            sent = (uint16_t)(sent + chunk);
            tcp_output(s->tcp);
            continue;
        }
        if ((eip_hal_millis() - start) >= kSendBudgetMs) {
            return EIP_ERR_IO;
        }
        tcp_output(s->tcp);
        eip_hal_yield();
    }
    return EIP_OK;
}

/* Copy from the rx chain, starting rx_off bytes into its head, then reopen the
 * receive window by what was taken. tcp_recved is skipped once the pcb is gone,
 * but buffered data can still be read after that. */
static int CopyOut(Sock *s, uint8_t *buf, uint16_t cap)
{
    uint16_t avail = (uint16_t)(s->rx->tot_len - s->rx_off);
    uint16_t n = (avail < cap) ? avail : cap;

    pbuf_copy_partial(s->rx, buf, n, s->rx_off);
    s->rx_off = (uint16_t)(s->rx_off + n);
    /* Drop fully read pbufs from the head of the chain. pbuf_free walks on
     * down a chain, so the extra reference on the next pbuf is what stops it
     * there and keeps the rest. */
    while (s->rx != 0 && s->rx_off >= s->rx->len) {
        struct pbuf *head = s->rx;
        s->rx_off = (uint16_t)(s->rx_off - head->len);
        s->rx = head->next;
        if (s->rx != 0) {
            pbuf_ref(s->rx);
        }
        pbuf_free(head);
    }
    if (s->tcp != 0) {
        tcp_recved(s->tcp, n);
    }
    return n;
}

/* Buffered data comes before failed, so bytes that arrived ahead of a close or
 * an error are still delivered, and only then does the call report EIP_ERR_IO. */
int eip_hal_tcp_recv(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint32_t timeout_ms)
{
    Sock *s = Get(sock, kTcp);
    uint32_t start = eip_hal_millis();

    if (s == 0) {
        return EIP_ERR_IO;
    }
    if (cap == 0) {
        return 0;
    }
    for (;;) {
        if (s->rx != 0) {
            return CopyOut(s, buf, cap);
        }
        if (s->failed) {
            return EIP_ERR_IO;
        }
        if (timeout_ms == 0 || (eip_hal_millis() - start) >= timeout_ms) {
            if (timeout_ms == 0) {
                eip_hal_yield(); /* let a poll-only caller make progress */
                if (s->rx != 0) {
                    continue;
                }
            }
            return 0;
        }
        eip_hal_yield();
    }
}

int eip_hal_tcp_local_ip(eip_sock_t sock, uint8_t ip[4])
{
    Sock *s = Get(sock, kTcp);

    if (s == 0 || s->tcp == 0) {
        return EIP_ERR_IO;
    }
    FromIpAddr(&s->tcp->local_ip, ip);
    return EIP_OK;
}

/* Closes either kind of TCP handle. A listener takes with it the accepted
 * sockets nobody collected. */
void eip_hal_tcp_close(eip_sock_t sock)
{
    Sock *s = Get(sock, kTcp);

    if (s != 0) {
        FreeTcp(s);
        return;
    }
    s = Get(sock, kListen);
    if (s != 0) {
        uint8_t i;
        for (i = 0; i < s->accept_n; i++) {
            FreeTcp(&s_socks[(int)s->accept_q[i]]);
        }
        if (s->tcp != 0) {
            tcp_arg(s->tcp, 0);
            tcp_accept(s->tcp, 0);
            tcp_close(s->tcp);
        }
        s->kind = kFree;
    }
}

/* ---- UDP ---------------------------------------------------------------- */

/* Takes ownership of p, which waits in the ring until read and is freed if the
 * ring is full. Queued datagrams hold their pbufs, so a burst on a small build
 * uses up stack buffers until the library drains them. The sender's address is
 * copied because addr is only good for this call. */
static void OnUdpRecv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    int index = Index(arg);
    Sock *s = (index >= 0 && s_socks[index].kind == kUdp) ? &s_socks[index] : 0;
    UdpEntry *e;

    (void)pcb;
    if (s == 0 || s->udp_n >= EIP_LWIP_UDP_QUEUE) {
        pbuf_free(p);
        return;
    }
    e = &s->udp_q[(s->udp_head + s->udp_n) % EIP_LWIP_UDP_QUEUE];
    e->p = p;
    FromIpAddr(addr, e->ip);
    e->port = port;
    s->udp_n++;
}

eip_sock_t eip_hal_udp_open(uint16_t local_port, uint8_t flags)
{
    struct udp_pcb *pcb;
    int index = Alloc(kUdp);

    if (index < 0) {
        return EIP_SOCK_INVALID;
    }
    pcb = udp_new();
    if (pcb == 0) {
        s_socks[index].kind = kFree;
        return EIP_SOCK_INVALID;
    }
    if (udp_bind(pcb, IP_ADDR_ANY, local_port) != ERR_OK) {
        udp_remove(pcb);
        s_socks[index].kind = kFree;
        return EIP_SOCK_INVALID;
    }
    if (flags & EIP_UDP_BROADCAST) {
        ip_set_option(pcb, SOF_BROADCAST);
    }
    /* Only the TTL is set, and only in builds with this option. No multicast
     * interface is chosen here: that is left to lwIP's routing. */
#if LWIP_MULTICAST_TX_OPTIONS
    if (flags & EIP_UDP_MULTICAST_TX) {
        udp_set_multicast_ttl(pcb, 1);
    }
#endif
    s_socks[index].udp = pcb;
    udp_recv(pcb, OnUdpRecv, Arg(index));
    return index;
}

int eip_hal_udp_sendto(eip_sock_t sock, const uint8_t ip[4], uint16_t port, const uint8_t *data,
                       uint16_t len)
{
    Sock *s = Get(sock, kUdp);
    ip_addr_t addr;
    struct pbuf *p;
    err_t err;

    if (s == 0 || s->udp == 0) {
        return EIP_ERR_IO;
    }
    /* One RAM pbuf per datagram, freed here whatever udp_sendto returns. */
    p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (p == 0) {
        return EIP_ERR_IO;
    }
    if (pbuf_take(p, data, len) != ERR_OK) {
        pbuf_free(p);
        return EIP_ERR_IO;
    }
    ToIpAddr(ip, &addr);
    err = udp_sendto(s->udp, p, &addr, port);
    pbuf_free(p);
    return (err == ERR_OK) ? EIP_OK : EIP_ERR_IO;
}

int eip_hal_udp_recvfrom(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint8_t ip[4],
                         uint16_t *port, uint32_t timeout_ms)
{
    Sock *s = Get(sock, kUdp);
    uint32_t start = eip_hal_millis();

    if (s == 0) {
        return EIP_ERR_IO;
    }
    if (s->udp_n == 0) {
        eip_hal_yield(); /* let a poll-only caller make progress */
    }
    while (s->udp_n == 0) {
        if (timeout_ms == 0 || (eip_hal_millis() - start) >= timeout_ms) {
            return 0;
        }
        eip_hal_yield();
    }
    {
        UdpEntry *e = &s->udp_q[s->udp_head];
        uint16_t n = (e->p->tot_len < cap) ? e->p->tot_len : cap;

        pbuf_copy_partial(e->p, buf, n, 0);
        if (ip != 0) {
            memcpy(ip, e->ip, 4);
        }
        if (port != 0) {
            *port = e->port;
        }
        pbuf_free(e->p);
        e->p = 0;
        s->udp_head = (uint8_t)((s->udp_head + 1u) % EIP_LWIP_UDP_QUEUE);
        s->udp_n--;
        return n;
    }
}

void eip_hal_udp_close(eip_sock_t sock)
{
    Sock *s = Get(sock, kUdp);

    if (s == 0) {
        return;
    }
    while (s->udp_n > 0) {
        pbuf_free(s->udp_q[s->udp_head].p);
        s->udp_head = (uint8_t)((s->udp_head + 1u) % EIP_LWIP_UDP_QUEUE);
        s->udp_n--;
    }
    if (s->udp != 0) {
        udp_remove(s->udp);
        s->udp = 0;
    }
    s->kind = kFree;
}

/* ---- waiting ------------------------------------------------------------ */

static int AnyReady(const eip_sock_t *socks, uint8_t count)
{
    uint8_t i;

    for (i = 0; i < count; i++) {
        eip_sock_t h = socks[i];
        if (h < 0 || h >= EIP_LWIP_MAX_SOCKETS) {
            continue;
        }
        switch (s_socks[h].kind) {
        case kTcp:
            if (s_socks[h].rx != 0 || s_socks[h].failed) {
                return 1;
            }
            break;
        case kListen:
            if (s_socks[h].accept_n != 0) {
                return 1;
            }
            break;
        case kUdp:
            if (s_socks[h].udp_n != 0) {
                return 1;
            }
            break;
        default:
            break;
        }
    }
    return 0;
}

/* There is nothing to sleep on: this spins on the stack poll until a socket has
 * something or the time is up. It polls at least once, even with timeout_ms 0.
 * Unsigned elapsed time is safe across the clock wrap. */
int eip_hal_wait(const eip_sock_t *socks, uint8_t count, uint32_t timeout_ms)
{
    uint32_t start = eip_hal_millis();

    for (;;) {
        eip_hal_yield();
        if (AnyReady(socks, count)) {
            return 1;
        }
        if ((eip_hal_millis() - start) >= timeout_ms) {
            return 0;
        }
    }
}
