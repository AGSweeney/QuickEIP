/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP port: Windows (Winsock 2), Windows 7 and later, 32- and 64-bit.
 *
 * The whole contract in one file. Link with ws2_32, iphlpapi and winmm (CMake
 * does this; for your own project see docs/INTEGRATION.md).
 *
 * How it differs from ports/bsd_sockets, and why:
 *   - A SOCKET is a pointer-sized kernel handle, not a small int, so handles
 *     are indexes into a table here.
 *   - SO_REUSEADDR on Windows lets two sockets share a port, and without
 *     SO_EXCLUSIVEADDRUSE another process can still take one over. Every bind
 *     is exclusive, so a busy port fails, as the contract requires.
 *   - A failed non-blocking connect() is reported in the except set of
 *     select(), not the write set.
 *   - recvfrom() fails with WSAECONNRESET after an ICMP port-unreachable and
 *     with WSAEMSGSIZE on a truncated datagram.
 *   - The default system tick is about 15.6 ms, too coarse for Class 1 RPIs:
 *     the clock is QueryPerformanceCounter and the process runs with a 1 ms
 *     timer period (timeBeginPeriod) from the first HAL call on.
 *   - GetAdaptersAddresses can take many milliseconds and the adapter asks for
 *     interfaces every 100 ms, so results are cached and refreshed on the
 *     system thread pool. The adapter thread never waits for it.
 *
 * eip_adapter_start() runs the adapter on its own high-priority thread. Build
 * with EIP_WIN_POLL_MODE to run it from your own loop with eip_adapter_poll().
 *
 * Set EIP_NETIF in the environment to put one interface first. It matches the
 * adapter's friendly name ("Ethernet 2") or its IPv4 address ("10.168.41.1").
 */
#ifndef EIP_WIN_MAX_SOCKETS
#define EIP_WIN_MAX_SOCKETS 64
#endif
#ifndef EIP_WIN_MAX_INTERFACES
#define EIP_WIN_MAX_INTERFACES 16
#endif
#ifndef EIP_WIN_NETIF_REFRESH_MS
#define EIP_WIN_NETIF_REFRESH_MS 2000u
#endif

/* select() can wait on every socket in the table. */
#define FD_SETSIZE EIP_WIN_MAX_SOCKETS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <mmsystem.h>
#include <process.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eip_hal.h"
#include "eip_server.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

enum { kSendBudgetMs = 500 };

/* ---- one-time setup ----------------------------------------------------- */

static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER s_qpc_freq;

static BOOL CALLBACK StartupOnce(PINIT_ONCE once, PVOID param, PVOID *context)
{
    WSADATA wsa;

    (void)once;
    (void)param;
    (void)context;
    (void)QueryPerformanceFrequency(&s_qpc_freq);
    (void)WSAStartup(MAKEWORD(2, 2), &wsa);
    (void)timeBeginPeriod(1);
    return TRUE;
}

/* Neither WSACleanup nor timeEndPeriod is called: both are released at
 * process exit, and the library has no shutdown call of its own. */
static void Startup(void)
{
    (void)InitOnceExecuteOnce(&s_once, StartupOnce, 0, 0);
}

/* ---- time and scheduling ------------------------------------------------ */

static SRWLOCK s_lock = SRWLOCK_INIT;

uint32_t eip_hal_millis(void)
{
    LARGE_INTEGER now;
    uint64_t freq;
    uint64_t ticks;

    Startup();
    (void)QueryPerformanceCounter(&now);
    ticks = (uint64_t)now.QuadPart;
    freq = (uint64_t)s_qpc_freq.QuadPart;
    /* Split so ticks * 1000 cannot overflow after long uptimes. */
    return (uint32_t)((ticks / freq) * 1000u + ((ticks % freq) * 1000u) / freq);
}

uint32_t eip_hal_clock_resolution_ms(void)
{
    return 1u;
}

void eip_hal_yield(void)
{
    Startup();
    Sleep(1);
}

void eip_hal_lock(void)
{
    AcquireSRWLockExclusive(&s_lock);
}

void eip_hal_unlock(void)
{
    ReleaseSRWLockExclusive(&s_lock);
}

/* ---- socket handles ----------------------------------------------------- */

static SRWLOCK s_table_lock = SRWLOCK_INIT;
static SOCKET s_socks[EIP_WIN_MAX_SOCKETS];
static uint8_t s_used[EIP_WIN_MAX_SOCKETS];

/* Takes ownership of s: closes it when the table is full. */
static eip_sock_t AddSocket(SOCKET s)
{
    eip_sock_t handle = EIP_SOCK_INVALID;
    int i;

    AcquireSRWLockExclusive(&s_table_lock);
    for (i = 0; i < EIP_WIN_MAX_SOCKETS; i++) {
        if (!s_used[i]) {
            s_used[i] = 1;
            s_socks[i] = s;
            handle = i;
            break;
        }
    }
    ReleaseSRWLockExclusive(&s_table_lock);
    if (handle == EIP_SOCK_INVALID) {
        (void)closesocket(s);
    }
    return handle;
}

static SOCKET GetSocket(eip_sock_t handle)
{
    SOCKET s = INVALID_SOCKET;

    if (handle >= 0 && handle < EIP_WIN_MAX_SOCKETS) {
        AcquireSRWLockShared(&s_table_lock);
        if (s_used[handle]) {
            s = s_socks[handle];
        }
        ReleaseSRWLockShared(&s_table_lock);
    }
    return s;
}

static SOCKET TakeSocket(eip_sock_t handle)
{
    SOCKET s = INVALID_SOCKET;

    if (handle >= 0 && handle < EIP_WIN_MAX_SOCKETS) {
        AcquireSRWLockExclusive(&s_table_lock);
        if (s_used[handle]) {
            s = s_socks[handle];
            s_used[handle] = 0;
        }
        ReleaseSRWLockExclusive(&s_table_lock);
    }
    return s;
}

/* ---- socket helpers ----------------------------------------------------- */

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

static void SetNonBlocking(SOCKET s)
{
    u_long on = 1;
    (void)ioctlsocket(s, FIONBIO, &on);
}

static void SetOption(SOCKET s, int level, int name, int value)
{
    (void)setsockopt(s, level, name, (const char *)&value, (int)sizeof value);
}

static int WouldBlock(void)
{
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
}

/* Wait for one socket. write 0: readable, 1: writable. The except set is what
 * reports a failed connect(). >0 ready, 0 timeout, <0 error. */
static int WaitOne(SOCKET s, int write, uint32_t timeout_ms)
{
    fd_set set;
    fd_set except;
    struct timeval tv;

    FD_ZERO(&set);
    FD_ZERO(&except);
    FD_SET(s, &set);
    FD_SET(s, &except);
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    return select(0, write ? 0 : &set, write ? &set : 0, &except, &tv);
}

/* ---- TCP ---------------------------------------------------------------- */

eip_sock_t eip_hal_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    struct sockaddr_in sa;
    SOCKET s;
    int rc;

    Startup();
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return EIP_SOCK_INVALID;
    }
    SetNonBlocking(s);
    SetOption(s, IPPROTO_TCP, TCP_NODELAY, 1);
    ToSockaddr(ip, port, &sa);
    rc = connect(s, (const struct sockaddr *)&sa, (int)sizeof sa);
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        (void)closesocket(s);
        return EIP_SOCK_INVALID;
    }
    if (rc != 0) {
        int err = 0;
        int len = (int)sizeof err;
        if (WaitOne(s, 1, timeout_ms) <= 0 ||
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) != 0 || err != 0) {
            (void)closesocket(s);
            return EIP_SOCK_INVALID;
        }
    }
    return AddSocket(s);
}

eip_sock_t eip_hal_tcp_listen(uint16_t port, uint8_t backlog)
{
    static const uint8_t any[4] = {0, 0, 0, 0};
    struct sockaddr_in sa;
    SOCKET s;

    Startup();
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return EIP_SOCK_INVALID;
    }
    SetOption(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
    ToSockaddr(any, port, &sa);
    if (bind(s, (const struct sockaddr *)&sa, (int)sizeof sa) != 0 || listen(s, backlog ? backlog : 1) != 0) {
        (void)closesocket(s);
        return EIP_SOCK_INVALID;
    }
    SetNonBlocking(s);
    return AddSocket(s);
}

eip_sock_t eip_hal_tcp_accept(eip_sock_t listener, uint8_t peer_ip[4])
{
    struct sockaddr_in sa;
    struct tcp_keepalive keep;
    int len = (int)sizeof sa;
    DWORD bytes = 0;
    SOCKET ls = GetSocket(listener);
    SOCKET s;

    if (ls == INVALID_SOCKET) {
        return EIP_SOCK_INVALID;
    }
    s = accept(ls, (struct sockaddr *)&sa, &len);
    if (s == INVALID_SOCKET) {
        return EIP_SOCK_INVALID;
    }
    SetNonBlocking(s);
    SetOption(s, IPPROTO_TCP, TCP_NODELAY, 1);
    /* Keep-alive frees the slot of a peer that vanished: 30 s idle, then
     * probes 5 s apart (3 where the system supports TCP_KEEPCNT, else 10). */
    keep.onoff = 1;
    keep.keepalivetime = 30000;
    keep.keepaliveinterval = 5000;
    (void)WSAIoctl(s, SIO_KEEPALIVE_VALS, &keep, (DWORD)sizeof keep, 0, 0, &bytes, 0, 0);
#ifdef TCP_KEEPCNT
    SetOption(s, IPPROTO_TCP, TCP_KEEPCNT, 3);
#endif
    FromSockaddr(&sa, peer_ip, 0);
    return AddSocket(s);
}

int eip_hal_tcp_send(eip_sock_t sock, const uint8_t *data, uint16_t len)
{
    uint32_t start = eip_hal_millis();
    uint16_t sent = 0;
    SOCKET s = GetSocket(sock);

    if (s == INVALID_SOCKET) {
        return EIP_ERR_IO;
    }
    while (sent < len) {
        int n = send(s, (const char *)data + sent, (int)(len - sent), 0);
        if (n > 0) {
            sent = (uint16_t)(sent + (uint16_t)n);
            continue;
        }
        if (n < 0 && WouldBlock()) {
            uint32_t elapsed = eip_hal_millis() - start;
            if (elapsed >= kSendBudgetMs || WaitOne(s, 1, kSendBudgetMs - elapsed) < 0) {
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
    SOCKET s = GetSocket(sock);
    int n;

    if (s == INVALID_SOCKET) {
        return EIP_ERR_IO;
    }
    if (cap == 0) {
        return 0;
    }
    n = recv(s, (char *)buf, cap, 0);
    if (n < 0 && WouldBlock() && timeout_ms != 0) {
        int ready = WaitOne(s, 0, timeout_ms);
        if (ready < 0) {
            return EIP_ERR_IO;
        }
        if (ready == 0) {
            return 0;
        }
        n = recv(s, (char *)buf, cap, 0);
    }
    if (n > 0) {
        return n;
    }
    if (n < 0 && WouldBlock()) {
        return 0;
    }
    return EIP_ERR_IO; /* 0 = orderly close, or a hard error */
}

int eip_hal_tcp_local_ip(eip_sock_t sock, uint8_t ip[4])
{
    struct sockaddr_in sa;
    int len = (int)sizeof sa;
    SOCKET s = GetSocket(sock);

    if (s == INVALID_SOCKET || getsockname(s, (struct sockaddr *)&sa, &len) != 0) {
        return EIP_ERR_IO;
    }
    FromSockaddr(&sa, ip, 0);
    return EIP_OK;
}

void eip_hal_tcp_close(eip_sock_t sock)
{
    SOCKET s = TakeSocket(sock);

    if (s != INVALID_SOCKET) {
        (void)shutdown(s, SD_BOTH);
        (void)closesocket(s);
    }
}

/* ---- UDP ---------------------------------------------------------------- */

static eip_sock_t s_mcast_sock = EIP_SOCK_INVALID;
static uint8_t s_mcast_ip[4];

eip_sock_t eip_hal_udp_open(uint16_t local_port, uint8_t flags)
{
    static const uint8_t any[4] = {0, 0, 0, 0};
    struct sockaddr_in sa;
    BOOL report_reset = FALSE;
    DWORD bytes = 0;
    SOCKET s;

    Startup();
    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return EIP_SOCK_INVALID;
    }
    /* A second bind of a busy port must fail, so the originator never shares
     * UDP 2222 with an adapter on this host. */
    SetOption(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
    if (flags & EIP_UDP_BROADCAST) {
        SetOption(s, SOL_SOCKET, SO_BROADCAST, 1);
    }
    if (flags & EIP_UDP_MULTICAST_TX) {
        SetOption(s, IPPROTO_IP, IP_MULTICAST_TTL, 1); /* CIP multicast default TTL */
    }
    /* An ICMP port-unreachable must not fail the next recvfrom(). */
    (void)WSAIoctl(s, SIO_UDP_CONNRESET, &report_reset, (DWORD)sizeof report_reset, 0, 0, &bytes, 0, 0);
    ToSockaddr(any, local_port, &sa);
    if (bind(s, (const struct sockaddr *)&sa, (int)sizeof sa) != 0) {
        (void)closesocket(s);
        return EIP_SOCK_INVALID;
    }
    SetNonBlocking(s);
    return AddSocket(s);
}

static void PickMulticastInterface(eip_sock_t sock, SOCKET s)
{
    EipNetifInfo info;

    memset(&info, 0, sizeof info);
    if (eip_hal_netif_get(0, &info) != EIP_OK) {
        return;
    }
    if (sock == s_mcast_sock && memcmp(info.ip, s_mcast_ip, 4) == 0) {
        return;
    }
    {
        struct in_addr ifa;
        memcpy(&ifa.s_addr, info.ip, 4);
        (void)setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&ifa, (int)sizeof ifa);
    }
    s_mcast_sock = sock;
    memcpy(s_mcast_ip, info.ip, 4);
}

int eip_hal_udp_sendto(eip_sock_t sock, const uint8_t ip[4], uint16_t port, const uint8_t *data,
                       uint16_t len)
{
    struct sockaddr_in sa;
    SOCKET s = GetSocket(sock);
    int n;

    if (s == INVALID_SOCKET) {
        return EIP_ERR_IO;
    }
    if ((ip[0] & 0xF0u) == 0xE0u) {
        PickMulticastInterface(sock, s);
    }
    ToSockaddr(ip, port, &sa);
    n = sendto(s, (const char *)data, len, 0, (const struct sockaddr *)&sa, (int)sizeof sa);
    return (n == (int)len) ? EIP_OK : EIP_ERR_IO;
}

int eip_hal_udp_recvfrom(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint8_t ip[4],
                         uint16_t *port, uint32_t timeout_ms)
{
    struct sockaddr_in sa;
    int len = (int)sizeof sa;
    SOCKET s = GetSocket(sock);
    int n;

    if (s == INVALID_SOCKET) {
        return EIP_ERR_IO;
    }
    n = recvfrom(s, (char *)buf, cap, 0, (struct sockaddr *)&sa, &len);
    if (n < 0 && WouldBlock() && timeout_ms != 0) {
        int ready = WaitOne(s, 0, timeout_ms);
        if (ready <= 0) {
            return (ready < 0) ? EIP_ERR_IO : 0;
        }
        len = (int)sizeof sa;
        n = recvfrom(s, (char *)buf, cap, 0, (struct sockaddr *)&sa, &len);
    }
    if (n < 0) {
        int err = WSAGetLastError();
        if (err == WSAEMSGSIZE) {
            n = cap; /* the contract truncates; Winsock kept the first cap bytes */
        } else if (err == WSAEWOULDBLOCK || err == WSAEINTR || err == WSAECONNRESET || err == WSAENETRESET) {
            return 0;
        } else {
            return EIP_ERR_IO;
        }
    }
    if (n == 0) {
        return 0;
    }
    FromSockaddr(&sa, ip, port);
    return n;
}

void eip_hal_udp_close(eip_sock_t sock)
{
    SOCKET s = TakeSocket(sock);

    if (s != INVALID_SOCKET) {
        if (sock == s_mcast_sock) {
            s_mcast_sock = EIP_SOCK_INVALID;
        }
        (void)closesocket(s);
    }
}

/* ---- waiting ------------------------------------------------------------ */

int eip_hal_wait(const eip_sock_t *socks, uint8_t count, uint32_t timeout_ms)
{
    fd_set set;
    struct timeval tv;
    uint8_t i;
    int any = 0;
    int rc;

    Startup();
    FD_ZERO(&set);
    for (i = 0; i < count; i++) {
        SOCKET s = GetSocket(socks[i]);
        if (s != INVALID_SOCKET) {
            FD_SET(s, &set);
            any = 1;
        }
    }
    if (!any) {
        /* select() with empty sets is an error on Windows. */
        Sleep(timeout_ms);
        return 0;
    }
    tv.tv_sec = (long)(timeout_ms / 1000u);
    tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
    rc = select(0, &set, 0, 0, &tv);
    if (rc < 0) {
        Sleep(1); /* a socket closed under us: don't spin in the caller's loop */
        return 0;
    }
    return rc;
}

/* ---- network interfaces ------------------------------------------------- */

static SRWLOCK s_netif_lock = SRWLOCK_INIT;
static EipNetifInfo s_netifs[EIP_WIN_MAX_INTERFACES];
static uint8_t s_netif_count;
static uint8_t s_netif_valid;
static uint32_t s_netif_ms;
static volatile LONG s_netif_refreshing;

static void CopyIpv4(const SOCKET_ADDRESS *from, uint8_t ip[4])
{
    memcpy(ip, &((const struct sockaddr_in *)from->lpSockaddr)->sin_addr.s_addr, 4);
}

static const IP_ADAPTER_UNICAST_ADDRESS *FirstIpv4(const IP_ADAPTER_ADDRESSES *a)
{
    const IP_ADAPTER_UNICAST_ADDRESS *u;

    for (u = a->FirstUnicastAddress; u != 0; u = u->Next) {
        if (u->Address.lpSockaddr != 0 && u->Address.lpSockaddr->sa_family == AF_INET) {
            return u;
        }
    }
    return 0;
}

/* EIP_NETIF names this adapter, by friendly name or by IPv4 address. */
static int IsPreferred(const IP_ADAPTER_ADDRESSES *a, const IP_ADAPTER_UNICAST_ADDRESS *u, const char *prefer)
{
    char name[256];
    char text[16];
    uint8_t ip[4];

    if (prefer == 0 || prefer[0] == '\0') {
        return 0;
    }
    if (a->FriendlyName != 0 &&
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, (int)sizeof name, 0, 0) > 0 &&
        _stricmp(name, prefer) == 0) {
        return 1;
    }
    CopyIpv4(&u->Address, ip);
    (void)snprintf(text, sizeof text, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return strcmp(text, prefer) == 0;
}

static void FillInfo(const IP_ADAPTER_ADDRESSES *a, const IP_ADAPTER_UNICAST_ADDRESS *u, const char *host,
                     EipNetifInfo *info)
{
    const IP_ADAPTER_GATEWAY_ADDRESS_LH *gw;
    const IP_ADAPTER_DNS_SERVER_ADDRESS *dns;
    uint32_t mask = 0;
    int dns_seen = 0;

    memset(info, 0, sizeof *info);
    CopyIpv4(&u->Address, info->ip);
    if (u->OnLinkPrefixLength != 0 && u->OnLinkPrefixLength <= 32) {
        mask = 0xFFFFFFFFu << (32u - u->OnLinkPrefixLength);
    }
    info->netmask[0] = (uint8_t)(mask >> 24);
    info->netmask[1] = (uint8_t)(mask >> 16);
    info->netmask[2] = (uint8_t)(mask >> 8);
    info->netmask[3] = (uint8_t)mask;
    for (gw = a->FirstGatewayAddress; gw != 0; gw = gw->Next) {
        if (gw->Address.lpSockaddr != 0 && gw->Address.lpSockaddr->sa_family == AF_INET) {
            CopyIpv4(&gw->Address, info->gateway);
            break;
        }
    }
    for (dns = a->FirstDnsServerAddress; dns != 0 && dns_seen < 2; dns = dns->Next) {
        if (dns->Address.lpSockaddr != 0 && dns->Address.lpSockaddr->sa_family == AF_INET) {
            CopyIpv4(&dns->Address, dns_seen == 0 ? info->dns1 : info->dns2);
            dns_seen++;
        }
    }
    if (a->PhysicalAddressLength == 6) {
        memcpy(info->mac, a->PhysicalAddress, 6);
    }
    info->dhcp = (a->Flags & IP_ADAPTER_DHCP_ENABLED) ? 1u : 0u;
    info->link_known = 1;
    info->link_up = (a->OperStatus == IfOperStatusUp) ? 1u : 0u;
    if (info->link_up) {
        /* Windows reports the speed; duplex and negotiation aren't exposed. */
        ULONG64 bps = a->TransmitLinkSpeed;
        info->speed_mbps = (bps == (ULONG64)-1) ? 100u : (uint32_t)(bps / 1000000u);
        info->full_duplex = 1;
        info->autonegotiated = 1;
    }
    strncpy(info->host_name, host, sizeof info->host_name - 1u);
}

/* Read the system's adapters into s_netifs. Order: the EIP_NETIF match, then
 * adapters with a default gateway, then the rest, each in system order.
 * Loopback, tunnels and adapters that are down are left out (an EIP_NETIF
 * match is kept even when down, so its link state can be reported). */
static void ReadInterfaces(void)
{
    static const ULONG kFlags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
    EipNetifInfo found[EIP_WIN_MAX_INTERFACES];
    const char *prefer = getenv("EIP_NETIF");
    char host[65] = "";
    ULONG size = 32768;
    IP_ADAPTER_ADDRESSES *all = 0;
    const IP_ADAPTER_ADDRESSES *a;
    uint8_t count = 0;
    int pass;
    ULONG rc;
    int attempt;

    (void)gethostname(host, (int)sizeof host - 1);
    for (attempt = 0; attempt < 3; attempt++) {
        all = (IP_ADAPTER_ADDRESSES *)malloc(size);
        if (all == 0) {
            return;
        }
        rc = GetAdaptersAddresses(AF_INET, kFlags, 0, all, &size);
        if (rc == ERROR_SUCCESS) {
            break;
        }
        free(all);
        all = 0;
        if (rc != ERROR_BUFFER_OVERFLOW) {
            break;
        }
    }
    if (all != 0) {
        for (pass = 0; pass < 3; pass++) {
            for (a = all; a != 0 && count < EIP_WIN_MAX_INTERFACES; a = a->Next) {
                const IP_ADAPTER_UNICAST_ADDRESS *u = FirstIpv4(a);
                int preferred;
                int usable;
                int has_gateway;

                if (u == 0 || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) {
                    continue;
                }
                preferred = IsPreferred(a, u, prefer);
                usable = (a->OperStatus == IfOperStatusUp);
                has_gateway = (a->FirstGatewayAddress != 0);
                if ((pass == 0 && preferred) || (pass == 1 && !preferred && usable && has_gateway) ||
                    (pass == 2 && !preferred && usable && !has_gateway)) {
                    FillInfo(a, u, host, &found[count]);
                    count++;
                }
            }
        }
        free(all);
    }
    AcquireSRWLockExclusive(&s_netif_lock);
    memcpy(s_netifs, found, (size_t)count * sizeof found[0]);
    s_netif_count = count;
    s_netif_valid = 1;
    s_netif_ms = eip_hal_millis();
    ReleaseSRWLockExclusive(&s_netif_lock);
}

static DWORD WINAPI RefreshWork(LPVOID arg)
{
    (void)arg;
    ReadInterfaces();
    (void)InterlockedExchange(&s_netif_refreshing, 0);
    return 0;
}

int eip_hal_netif_get(uint8_t index, EipNetifInfo *info)
{
    int rc = EIP_ERR_ARG;
    int valid;
    int stale;

    Startup();
    memset(info, 0, sizeof *info);
    AcquireSRWLockShared(&s_netif_lock);
    valid = s_netif_valid;
    stale = valid && (eip_hal_millis() - s_netif_ms) >= EIP_WIN_NETIF_REFRESH_MS;
    ReleaseSRWLockShared(&s_netif_lock);
    if (!valid) {
        ReadInterfaces(); /* the first call has nothing to serve yet */
    } else if (stale && InterlockedCompareExchange(&s_netif_refreshing, 1, 0) == 0) {
        if (!QueueUserWorkItem(RefreshWork, 0, WT_EXECUTEDEFAULT)) {
            (void)InterlockedExchange(&s_netif_refreshing, 0);
        }
    }
    AcquireSRWLockShared(&s_netif_lock);
    if (index < s_netif_count) {
        *info = s_netifs[index];
        rc = EIP_OK;
    }
    ReleaseSRWLockShared(&s_netif_lock);
    return rc;
}

/* ---- adapter runner ----------------------------------------------------- */

#ifndef EIP_WIN_POLL_MODE
static unsigned __stdcall AdapterThread(void *arg)
{
    EipServer *server = (EipServer *)arg;
    for (;;) {
        eip_server_service(server, EIP_ADAPTER_IDLE_WAIT_MS);
    }
}
#endif

int eip_hal_adapter_start_task(struct EipServer *server)
{
#ifdef EIP_WIN_POLL_MODE
    (void)server; /* the application calls eip_adapter_poll() from its own loop */
    Startup();
    return EIP_OK;
#else
    uintptr_t thread;

    Startup();
    thread = _beginthreadex(0, EIP_ADAPTER_TASK_STACK, AdapterThread, server, 0, 0);
    if (thread == 0) {
        return EIP_ERR_IO;
    }
    /* EIP_ADAPTER_TASK_PRIORITY is a THREAD_PRIORITY_* value; 0 picks HIGHEST
     * so desktop load doesn't turn into RPI jitter. */
    (void)SetThreadPriority((HANDLE)thread,
                            EIP_ADAPTER_TASK_PRIORITY != 0 ? EIP_ADAPTER_TASK_PRIORITY : THREAD_PRIORITY_HIGHEST);
    (void)CloseHandle((HANDLE)thread);
    return EIP_OK;
#endif
}
