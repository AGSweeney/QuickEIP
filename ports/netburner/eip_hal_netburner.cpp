/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP port: NetBurner NNDK 3.x (NBRTOS, NetBurner TCP/IP stack).
 *
 * Self-contained: implements the whole contract in eip_hal.h over the NNDK
 * socket API. Socket handles are NNDK file descriptors (always > 0).
 *
 * Notes carried over from the original NetBurner-only stack:
 *   - CreateRxUdpSocket leaves the local port at 0 and cannot send, so every
 *     UDP socket here is CreateRxTxUdpSocket4 with an explicit local port.
 *   - RegisterUDPFifo lets a second socket take a port's packets, so the
 *     scanner side never auto-binds UDP 2222 (handled in eip_io.c).
 *   - This stack does not loop a broadcast back to the sender; the scan also
 *     unicasts to the device's own address (handled in eip_scan.c).
 *   - eip_hal_millis follows TimeTick, so its resolution is one tick
 *     (50 ms at the default 20 Hz). The adapter never sets a Class 1 timeout
 *     under two ticks; raise TICKS_PER_SECOND for fast RPIs.
 *   - TCP sockets keep the stack's defaults: this port sets neither
 *     TCP_NODELAY nor keep-alive, so the slot of a vanished peer comes back
 *     through the server's client eviction.
 *
 * The adapter runs in its own task (bottom of this file). Handle 0 is never
 * valid here, so the checks below test > 0, stricter than the contract's >= 0.
 */
#include <predef.h>

#include "eip_hal.h"
#include "eip_server.h"

#include <string.h>

#include <iosys.h>
#include <nbrtos.h>
#include <netinterface.h>
#include <tcp.h>
#include <udp.h>

#ifndef EIP_NB_EPHEMERAL_FIRST
#define EIP_NB_EPHEMERAL_FIRST 50000u /* local UDP ports tried when the caller asks for "any" */
#endif
#define EIP_NB_EPHEMERAL_COUNT 64u

static void StoreIp(uint8_t out[4], IPADDR4 address)
{
    uint32_t value = (uint32_t)address;
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static IPADDR4 Ip4(const uint8_t ip[4])
{
    return IPADDR4(ip[0], ip[1], ip[2], ip[3]);
}

static IPADDR HostIp(IPADDR4 address)
{
#ifdef IPV6
    return IPADDR(address);
#else
    return address;
#endif
}

/* ms -> ticks, rounded up, at least 1 (a 0 tick timeout means "forever"). */
static unsigned long Ticks(uint32_t ms)
{
    uint64_t ticks = ((uint64_t)ms * TICKS_PER_SECOND + 999u) / 1000u;
    if (ticks == 0) {
        ticks = 1;
    }
    if (ticks > 0x7FFFFFFFu) {
        ticks = 0x7FFFFFFFu;
    }
    return (unsigned long)ticks;
}

/* NNDK's select timeouts are in ticks: ZeroWaitSelect is the "do not wait"
 * form, since 0 passed to select() would wait forever. */
static int Readable(int fd)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    return ZeroWaitSelect(FD_SETSIZE, &set, 0, 0) > 0 && FD_ISSET(fd, &set);
}

static int WaitReadable(int fd, uint32_t timeout_ms)
{
    fd_set set;
    if (timeout_ms == 0) {
        return Readable(fd);
    }
    FD_ZERO(&set);
    FD_SET(fd, &set);
    return select(FD_SETSIZE, &set, 0, 0, Ticks(timeout_ms)) > 0 && FD_ISSET(fd, &set);
}

/* ---- time and scheduling ------------------------------------------------ */

/* The product is 64-bit so it cannot overflow; the cast gives the 2^32 ms wrap
 * the contract asks for. */
extern "C" uint32_t eip_hal_millis(void)
{
    return (uint32_t)(((uint64_t)TimeTick * 1000u) / TICKS_PER_SECOND);
}

extern "C" uint32_t eip_hal_clock_resolution_ms(void)
{
    uint32_t ms = 1000u / TICKS_PER_SECOND;
    return (ms == 0) ? 1u : ms;
}

extern "C" void eip_hal_yield(void)
{
    OSTimeDly(1);
}

/* OSLock pauses task switching, so while it is held every other task waits,
 * including during allow_change, which runs under this lock. */
extern "C" void eip_hal_lock(void)
{
    OSLock();
}

extern "C" void eip_hal_unlock(void)
{
    OSUnlock();
}

/* ---- network interfaces ------------------------------------------------- */

/* dhcp and host_name are left at zero, so the adapter reports a static
 * configuration and an empty host name whatever the interface really uses.
 * autonegotiated simply follows link_up: it is not read from the PHY. */
extern "C" int eip_hal_netif_get(uint8_t index, EipNetifInfo *info)
{
    int32_t ifn = GetFirstInterface();
    uint8_t seen = 0;
    int i;

    memset(info, 0, sizeof *info);
    while (ifn > 0 && seen < index) {
        ifn = GetNextInterface((int)ifn);
        seen++;
    }
    if (ifn <= 0) {
        return EIP_ERR_ARG;
    }
    StoreIp(info->ip, InterfaceIP((int)ifn));
    StoreIp(info->netmask, InterfaceMASK((int)ifn));
    StoreIp(info->gateway, InterfaceGate((int)ifn));
    StoreIp(info->dns1, InterfaceDNS((int)ifn));
    StoreIp(info->dns2, InterfaceDNS2((int)ifn));
    {
        MACADR mac = InterfaceMAC((int)ifn);
        for (i = 0; i < 6; i++) {
            info->mac[i] = mac.GetByte(i);
        }
    }
    info->link_known = 1;
    info->link_up = InterfaceLinkActive((int)ifn) ? 1u : 0u;
    info->speed_mbps = info->link_up ? (uint32_t)InterfaceLinkSpeed((int)ifn) : 0u;
    info->full_duplex = (info->link_up && InterfaceLinkDuplex((int)ifn)) ? 1u : 0u;
    info->autonegotiated = info->link_up;
    return EIP_OK;
}

/* ---- TCP ---------------------------------------------------------------- */

extern "C" eip_sock_t eip_hal_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    int fd = connect(HostIp(Ip4(ip)), port, Ticks(timeout_ms));
    return (fd > 0) ? fd : EIP_SOCK_INVALID;
}

extern "C" eip_sock_t eip_hal_tcp_listen(uint16_t port, uint8_t backlog)
{
    int fd = listen(INADDR_ANY, port, backlog ? backlog : 1);
    return (fd > 0) ? fd : EIP_SOCK_INVALID;
}

extern "C" eip_sock_t eip_hal_tcp_accept(eip_sock_t listener, uint8_t peer_ip[4])
{
    uint16_t port = 0;
    int fd;

    if (listener <= 0 || !Readable(listener)) {
        return EIP_SOCK_INVALID; /* accept() with timeout 0 would wait forever */
    }
    /* Both forms below get a one-tick timeout, the shortest there is. */
#ifdef IPV6
    {
        IPADDR address;
        fd = accept(listener, &address, &port, 1);
        if (fd > 0) {
            StoreIp(peer_ip, address.Extract4());
        }
    }
#else
    {
        IPADDR4 address;
        fd = accept4(listener, &address, &port, 1);
        if (fd > 0) {
            StoreIp(peer_ip, address);
        }
    }
#endif
    (void)port;
    return (fd > 0) ? fd : EIP_SOCK_INVALID;
}

/* write() on a full TX buffer blocks for NNDK's TCP_WRITE_TIMEOUT, 10 seconds
 * by default, which would stall the adapter task and time out Class 1. So
 * write only when writeavail says it can't block, and give up after 500 ms.
 * The clock is coarse: at 20 Hz the limit lands on a tick. */
extern "C" int eip_hal_tcp_send(eip_sock_t sock, const uint8_t *data, uint16_t len)
{
    uint32_t start = eip_hal_millis();
    uint16_t sent = 0;

    if (sock <= 0) {
        return EIP_ERR_IO;
    }
    while (sent < len) {
        if (haserror(sock)) {
            return EIP_ERR_IO;
        }
        if (writeavail(sock)) {
            int n = write(sock, (const char *)data + sent, (int)(len - sent));
            if (n < 0) {
                return EIP_ERR_IO;
            }
            sent = (uint16_t)(sent + (uint16_t)n);
            if (sent == len) {
                break;
            }
        }
        if ((eip_hal_millis() - start) >= 500u) {
            return EIP_ERR_IO;
        }
        OSTimeDly(1);
    }
    return EIP_OK;
}

extern "C" int eip_hal_tcp_recv(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint32_t timeout_ms)
{
    int n;

    if (sock <= 0) {
        return EIP_ERR_IO;
    }
    if (cap == 0) {
        return 0;
    }
    if (timeout_ms == 0) {
        /* read() would block on an empty socket, so ask first and read only
         * what is there. Data comes before the error test, so a peer's last
         * reply is delivered even when the socket has since failed. */
        int waiting = dataavail(sock);
        if (waiting <= 0) {
            return haserror(sock) ? EIP_ERR_IO : 0;
        }
        n = read(sock, (char *)buf, (waiting < (int)cap) ? waiting : (int)cap);
        return (n > 0) ? n : EIP_ERR_IO;
    }
    n = ReadWithTimeout(sock, (char *)buf, (int)cap, Ticks(timeout_ms));
    if (n < 0) {
        return EIP_ERR_IO;
    }
    return n; /* 0 = timeout */
}

extern "C" int eip_hal_tcp_local_ip(eip_sock_t sock, uint8_t ip[4])
{
    IPADDR4 local;

    if (sock <= 0) {
        return EIP_ERR_IO;
    }
    local = GetSocketLocalAddr4(sock);
    if (local.IsNull()) {
        return EIP_ERR_IO;
    }
    StoreIp(ip, local);
    return EIP_OK;
}

extern "C" void eip_hal_tcp_close(eip_sock_t sock)
{
    if (sock > 0) {
        close(sock);
    }
}

/* ---- UDP ---------------------------------------------------------------- */

/* The destination stays unset: every send names its own with sendto4. */
static int OpenRxTx(uint16_t port)
{
    return CreateRxTxUdpSocket4(IPADDR4(0, 0, 0, 0), 0, port);
}

extern "C" eip_sock_t eip_hal_udp_open(uint16_t local_port, uint8_t flags)
{
    static uint16_t next = 0;
    uint16_t tries;
    int fd;

    (void)flags; /* broadcast and multicast sends need no option on this stack */
    if (local_port != 0) {
        fd = OpenRxTx(local_port);
        return (fd > 0) ? fd : EIP_SOCK_INVALID;
    }
    /* "Any port" is emulated by rotating through a fixed range. next carries
     * on from the last call, so a port is not reused until the range comes
     * round again. It is not locked: tasks opening sockets at the same moment
     * may pick the same port. */
    for (tries = 0; tries < EIP_NB_EPHEMERAL_COUNT; tries++) {
        uint16_t port = (uint16_t)(EIP_NB_EPHEMERAL_FIRST + next);
        next = (uint16_t)((next + 1u) % EIP_NB_EPHEMERAL_COUNT);
        fd = OpenRxTx(port);
        if (fd > 0) {
            return fd;
        }
    }
    return EIP_SOCK_INVALID;
}

extern "C" int eip_hal_udp_sendto(eip_sock_t sock, const uint8_t ip[4], uint16_t port, const uint8_t *data,
                                  uint16_t len)
{
    int sent;

    if (sock <= 0) {
        return EIP_ERR_IO;
    }
    sent = sendto4(sock, (puint8_t)data, (int)len, Ip4(ip), port);
    return (sent == (int)len) ? EIP_OK : EIP_ERR_IO;
}

extern "C" int eip_hal_udp_recvfrom(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint8_t ip[4], uint16_t *port,
                                    uint32_t timeout_ms)
{
    IPADDR4 from;
    uint16_t local_port = 0;
    uint16_t remote_port = 0;
    int n;

    if (sock <= 0) {
        return EIP_ERR_IO;
    }
    if (timeout_ms == 0 ? dataavail(sock) <= 0 : !WaitReadable(sock, timeout_ms)) {
        return 0;
    }
    n = recvfrom4(sock, (puint8_t)buf, (int)cap, &from, &local_port, &remote_port);
    if (n <= 0) {
        return (n == 0) ? 0 : EIP_ERR_IO;
    }
    if (ip != 0) {
        StoreIp(ip, from);
    }
    if (port != 0) {
        *port = remote_port;
    }
    return n;
}

extern "C" void eip_hal_udp_close(eip_sock_t sock)
{
    if (sock > 0) {
        close(sock);
    }
}

/* ---- waiting ------------------------------------------------------------ */

/* Ticks() rounds up, so the shortest real wait is one tick: with a 20 Hz tick,
 * the adapter's 5 ms idle wait can last 50 ms unless a socket wakes it. Only
 * the read set is passed. Errors surface on the next pass, in the receive
 * calls. */
extern "C" int eip_hal_wait(const eip_sock_t *socks, uint8_t count, uint32_t timeout_ms)
{
    fd_set set;
    uint8_t i;
    int any = 0;

    FD_ZERO(&set);
    for (i = 0; i < count; i++) {
        if (socks[i] > 0) {
            FD_SET(socks[i], &set);
            any = 1;
        }
    }
    if (!any) {
        OSTimeDly(Ticks(timeout_ms));
        return 0;
    }
    if (timeout_ms == 0) {
        return ZeroWaitSelect(FD_SETSIZE, &set, 0, 0);
    }
    return select(FD_SETSIZE, &set, 0, 0, Ticks(timeout_ms));
}

/* ---- adapter task ------------------------------------------------------- */

/* The task gets its server through s_server, as the create call below passes
 * it no argument. The priority is MAIN_PRIO - 1 unless
 * EIP_ADAPTER_TASK_PRIORITY is set. The stack is NNDK's default task stack:
 * EIP_ADAPTER_TASK_STACK has no effect on this port. */
static EipServer *s_server;

static void AdapterTask(void *pd)
{
    (void)pd;
    for (;;) {
        eip_server_service(s_server, EIP_ADAPTER_IDLE_WAIT_MS);
    }
}

extern "C" int eip_hal_adapter_start_task(struct EipServer *server)
{
    s_server = server;
#if EIP_ADAPTER_TASK_PRIORITY != 0
    if (OSSimpleTaskCreatewName(AdapterTask, EIP_ADAPTER_TASK_PRIORITY, "EIP Adapter") != 0) {
#else
    if (OSSimpleTaskCreatewName(AdapterTask, MAIN_PRIO - 1, "EIP Adapter") != 0) {
#endif
        return EIP_ERR_IO;
    }
    return EIP_OK;
}
