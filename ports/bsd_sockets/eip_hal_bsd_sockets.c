/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Socket half of the QuickEIP port contract over BSD sockets.
 *
 * Shared by ports/posix (Linux, macOS, WSL) and ports/esp-idf (lwIP socket
 * API through ESP-IDF's VFS). The platform file next to it supplies the
 * clock, the lock, interface information and the adapter task.
 *
 * Every socket is non-blocking; waits use select(). Handles are file
 * descriptors, so 0 is a valid one. select() limits them to below FD_SETSIZE,
 * and nothing here checks that.
 *
 * Nothing here needs eip_hal_lock. The only shared state is the multicast
 * interface cache below, which only multicast senders touch.
 */
#include "eip_hal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* A send on a connection the peer has reset must return an error, not raise
 * SIGPIPE. Linux uses this flag; NoSigPipe below covers stacks that have the
 * socket option instead. */
#ifdef MSG_NOSIGNAL
#define EIP_SEND_FLAGS MSG_NOSIGNAL
#else
#define EIP_SEND_FLAGS 0
#endif

enum { kSendBudgetMs = 500 }; /* the bound eip_hal.h asks of eip_hal_tcp_send */

static void ToSockaddr(const uint8_t ip[4], uint16_t port, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(port);
    memcpy(&sa->sin_addr.s_addr, ip, 4);
}

static void FromSockaddr(const struct sockaddr_in *sa, uint8_t ip[4], uint16_t *port)
{
    if (ip != 0) {
        memcpy(ip, &sa->sin_addr.s_addr, 4);
    }
    if (port != 0) {
        *port = ntohs(sa->sin_port);
    }
}

static void SetNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static void NoSigPipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

/* True when the last call failed only for now. EINTR counts, so a signal
 * makes a receive report "nothing yet" and a send wait and try again. */
static int WouldBlock(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

/* Wait for one fd. write 0: readable, 1: writable. >0 ready, 0 timeout, <0 error.
 * An EINTR retry reuses tv as select() left it, without recomputing it. */
static int WaitOne(int fd, int write, uint32_t timeout_ms)
{
    fd_set set;
    struct timeval tv;
    int rc;

    FD_ZERO(&set);
    FD_SET(fd, &set);
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    do {
        rc = select(fd + 1, write ? 0 : &set, write ? &set : 0, 0, &tv);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

/* ---- TCP ---------------------------------------------------------------- */

eip_sock_t eip_hal_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    int rc;

    if (fd < 0) {
        return EIP_SOCK_INVALID;
    }
    SetNonBlocking(fd);
    NoSigPipe(fd);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    ToSockaddr(ip, port, &sa);
    rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        close(fd);
        return EIP_SOCK_INVALID;
    }
    if (rc != 0) {
        /* Writable means the attempt finished; SO_ERROR says how it ended. */
        int err = 0;
        socklen_t len = sizeof err;
        if (WaitOne(fd, 1, timeout_ms) <= 0 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            close(fd);
            return EIP_SOCK_INVALID;
        }
    }
    return fd;
}

eip_sock_t eip_hal_tcp_listen(uint16_t port, uint8_t backlog)
{
    static const uint8_t any[4] = {0, 0, 0, 0};
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;

    if (fd < 0) {
        return EIP_SOCK_INVALID;
    }
    /* Lets a restarted adapter rebind while old connections linger in
     * TIME_WAIT. UDP deliberately does the opposite: see eip_hal_udp_open. */
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    ToSockaddr(any, port, &sa);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, backlog ? backlog : 1) != 0) {
        close(fd);
        return EIP_SOCK_INVALID;
    }
    SetNonBlocking(fd);
    return fd;
}

eip_sock_t eip_hal_tcp_accept(eip_sock_t listener, uint8_t peer_ip[4])
{
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;
    int one = 1;
    int fd;

    if (listener < 0) {
        return EIP_SOCK_INVALID;
    }
    fd = accept(listener, (struct sockaddr *)&sa, &len);
    if (fd < 0) {
        return EIP_SOCK_INVALID; /* EAGAIN and real errors alike: nobody to serve */
    }
    /* Not every stack passes O_NONBLOCK on from the listener. */
    SetNonBlocking(fd);
    NoSigPipe(fd);
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    /* Keep-alive frees the slot of a peer that vanished: about 45 s
     * (30 s idle + 3 probes x 5 s) where the stack supports the knobs. */
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
    {
        int idle = 30;
        int intvl = 5;
        int cnt = 3;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
        (void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    }
#endif
    FromSockaddr(&sa, peer_ip, 0);
    return fd;
}

/* The budget covers the whole call, not each attempt. Progress does not extend
 * it, and unsigned elapsed time is safe across the clock wrap. A partial send
 * that runs out of budget still reports EIP_ERR_IO; the bytes already queued
 * stay queued, so the caller must drop the connection. */
int eip_hal_tcp_send(eip_sock_t sock, const uint8_t *data, uint16_t len)
{
    uint32_t start = eip_hal_millis();
    uint16_t sent = 0;

    if (sock < 0) {
        return EIP_ERR_IO;
    }
    while (sent < len) {
        ssize_t n = send(sock, data + sent, (size_t)(len - sent), EIP_SEND_FLAGS);
        if (n > 0) {
            sent = (uint16_t)(sent + (uint16_t)n);
            continue;
        }
        if (n < 0 && WouldBlock()) {
            uint32_t elapsed = eip_hal_millis() - start;
            if (elapsed >= kSendBudgetMs || WaitOne(sock, 1, kSendBudgetMs - elapsed) < 0) {
                return EIP_ERR_IO;
            }
            continue;
        }
        return EIP_ERR_IO;
    }
    return EIP_OK;
}

int eip_hal_tcp_recv(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint32_t timeout_ms)
{
    ssize_t n;

    if (sock < 0) {
        return EIP_ERR_IO;
    }
    if (cap == 0) {
        return 0;
    }
    n = recv(sock, buf, cap, 0);
    if (n < 0 && WouldBlock() && timeout_ms != 0) {
        int ready = WaitOne(sock, 0, timeout_ms);
        if (ready < 0) {
            return EIP_ERR_IO;
        }
        if (ready == 0) {
            return 0;
        }
        n = recv(sock, buf, cap, 0);
    }
    if (n > 0) {
        return (int)n;
    }
    if (n < 0 && WouldBlock()) {
        return 0;
    }
    return EIP_ERR_IO; /* 0 = orderly close, or a hard error */
}

int eip_hal_tcp_local_ip(eip_sock_t sock, uint8_t ip[4])
{
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;

    if (sock < 0 || getsockname(sock, (struct sockaddr *)&sa, &len) != 0) {
        return EIP_ERR_IO;
    }
    FromSockaddr(&sa, ip, 0);
    return EIP_OK;
}

void eip_hal_tcp_close(eip_sock_t sock)
{
    if (sock >= 0) {
        (void)shutdown(sock, SHUT_RDWR);
        (void)close(sock);
    }
}

/* ---- UDP ---------------------------------------------------------------- */

/* The socket and address IP_MULTICAST_IF was last set for. One entry, so a
 * second multicast socket would make the two take turns setting it. */
static int s_mcast_fd = -1;
static uint8_t s_mcast_ip[4];

eip_sock_t eip_hal_udp_open(uint16_t local_port, uint8_t flags)
{
    static const uint8_t any[4] = {0, 0, 0, 0};
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;

    if (fd < 0) {
        return EIP_SOCK_INVALID;
    }
    /* No SO_REUSEADDR: a second bind of a busy port must fail. The originator
     * skips 2222 on its own (eip_io.c), so this is what stops an explicit
     * local_port 2222 sharing it with an adapter on this host, and what moves
     * the automatic search past a taken port. */
    if (flags & EIP_UDP_BROADCAST) {
        (void)setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    }
    if (flags & EIP_UDP_MULTICAST_TX) {
        uint8_t ttl = 1; /* CIP multicast default TTL */
        (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl);
    }
    ToSockaddr(any, local_port, &sa);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return EIP_SOCK_INVALID;
    }
    SetNonBlocking(fd);
    return fd;
}

/* Send multicast out of interface 0 (the contract's primary interface), and
 * follow its address if it changes. This asks the platform file for interface
 * 0 before every multicast send, whatever EipAdapterConfig.netif_index says.
 * If that fails the socket keeps its previous setting. */
static void PickMulticastInterface(int fd)
{
    EipNetifInfo info;

    memset(&info, 0, sizeof info);
    if (eip_hal_netif_get(0, &info) != EIP_OK) {
        return;
    }
    if (fd == s_mcast_fd && memcmp(info.ip, s_mcast_ip, 4) == 0) {
        return;
    }
    {
        struct in_addr ifa;
        memcpy(&ifa.s_addr, info.ip, 4);
        (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
    }
    s_mcast_fd = fd;
    memcpy(s_mcast_ip, info.ip, 4);
}

int eip_hal_udp_sendto(eip_sock_t sock, const uint8_t ip[4], uint16_t port, const uint8_t *data,
                       uint16_t len)
{
    struct sockaddr_in sa;
    ssize_t n;

    if (sock < 0) {
        return EIP_ERR_IO;
    }
    if ((ip[0] & 0xF0u) == 0xE0u) {
        PickMulticastInterface(sock);
    }
    ToSockaddr(ip, port, &sa);
    n = sendto(sock, data, len, 0, (struct sockaddr *)&sa, sizeof sa);
    return (n == (ssize_t)len) ? EIP_OK : EIP_ERR_IO;
}

int eip_hal_udp_recvfrom(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint8_t ip[4],
                         uint16_t *port, uint32_t timeout_ms)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;
    ssize_t n;

    if (sock < 0) {
        return EIP_ERR_IO;
    }
    n = recvfrom(sock, buf, cap, 0, (struct sockaddr *)&sa, &len);
    if (n < 0 && WouldBlock() && timeout_ms != 0) {
        int ready = WaitOne(sock, 0, timeout_ms);
        if (ready <= 0) {
            return (ready < 0) ? EIP_ERR_IO : 0;
        }
        len = sizeof sa;
        n = recvfrom(sock, buf, cap, 0, (struct sockaddr *)&sa, &len);
    }
    if (n < 0) {
        /* ECONNREFUSED from an ICMP port-unreachable is not fatal for UDP. */
        return (WouldBlock() || errno == ECONNREFUSED) ? 0 : EIP_ERR_IO;
    }
    if (n == 0) {
        return 0; /* an empty datagram reads as "nothing arrived" */
    }
    FromSockaddr(&sa, ip, port);
    return (int)n;
}

void eip_hal_udp_close(eip_sock_t sock)
{
    if (sock >= 0) {
        if (sock == s_mcast_fd) {
            s_mcast_fd = -1;
        }
        (void)close(sock);
    }
}

/* ---- QoS ---------------------------------------------------------------- */

/* DSCP is the top six bits of the TOS byte; the two ECN bits stay 0. */
int eip_hal_set_dscp(eip_sock_t sock, uint8_t dscp)
{
    int tos = (int)(dscp & 0x3Fu) << 2;

    if (sock < 0) {
        return EIP_ERR_ARG;
    }
    return (setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos) == 0) ? EIP_OK : EIP_ERR_STATE;
}

/* ---- waiting ------------------------------------------------------------ */

/* A select() error, EINTR included, reads as a timeout: the contract allows an
 * early return. With no valid handle there is nothing to select on, so sleep
 * in yields instead; that yields at least once, even with timeout_ms 0. */
int eip_hal_wait(const eip_sock_t *socks, uint8_t count, uint32_t timeout_ms)
{
    fd_set set;
    struct timeval tv;
    int maxfd = -1;
    uint8_t i;
    int rc;

    FD_ZERO(&set);
    for (i = 0; i < count; i++) {
        if (socks[i] >= 0) {
            FD_SET(socks[i], &set);
            if (socks[i] > maxfd) {
                maxfd = socks[i];
            }
        }
    }
    if (maxfd < 0) {
        uint32_t start = eip_hal_millis();
        do {
            eip_hal_yield();
        } while ((eip_hal_millis() - start) < timeout_ms);
        return 0;
    }
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    rc = select(maxfd + 1, &set, 0, 0, &tv);
    return (rc > 0) ? rc : 0;
}
