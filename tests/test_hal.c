/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Port contract test: calls include/eip_hal.h directly over this host's
 * loopback, so it checks the rules a port must keep rather than EtherNet/IP.
 * Portable: the POSIX port is the reference and must pass the same checks. */
#include "eip.h"
#include "eip_hal.h"

#include <stdio.h>
#include <string.h>

static int g_failed = 0;
static const uint8_t kLocal[4] = {127, 0, 0, 1};

#define EXPECT(cond, name)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s (line %d)\n", name, __LINE__);         \
            g_failed++;                                            \
        }                                                          \
    } while (0)

static void Pause(uint32_t ms)
{
    uint32_t start = eip_hal_millis();
    while (eip_hal_millis() - start < ms) {
        eip_hal_yield();
    }
}

/* A free port: bind to any, note it through a second socket's sendto, close. */
static uint16_t FreeUdpPort(void)
{
    static const uint8_t ping[1] = {0};
    eip_sock_t a = eip_hal_udp_open(0, 0);
    eip_sock_t b = eip_hal_udp_open(47801, 0);
    uint8_t buf[4];
    uint16_t port = 0;

    if (a >= 0 && b >= 0) {
        (void)eip_hal_udp_sendto(a, kLocal, 47801, ping, 1);
        (void)eip_hal_udp_recvfrom(b, buf, sizeof buf, 0, &port, 500);
    }
    eip_hal_udp_close(a);
    eip_hal_udp_close(b);
    return port;
}

static void TestClock(void)
{
    uint32_t start = eip_hal_millis();
    uint32_t t = start;
    uint32_t worst = 0;
    int i;

    for (i = 0; i < 50; i++) {
        uint32_t now;
        eip_hal_yield();
        now = eip_hal_millis();
        EXPECT((int32_t)(now - t) >= 0, "clock never runs backwards");
        if (now - t > worst) {
            worst = now - t;
        }
        t = now;
    }
    /* Class 1 pacing needs a yield far below a 10 ms RPI on average. */
    EXPECT(t - start < 250u, "50 yields take a few ms each at most");
    printf("clock: 50 yields in %lu ms, worst %lu ms, resolution %lu ms\n", (unsigned long)(t - start),
           (unsigned long)worst, (unsigned long)eip_hal_clock_resolution_ms());
    EXPECT(eip_hal_clock_resolution_ms() >= 1u, "clock resolution reported");

    /* The clock must really step at its reported resolution. T->O pacing runs
     * on it: a 16 ms clock reporting 1 ms held the adapter's T->O at about
     * half rate and the connection timed out. Count the distinct values seen
     * in a 100 ms spin, which is about 100 at 1 ms and about 6 at 16 ms. */
    {
        uint32_t distinct = 0;
        uint32_t last = eip_hal_millis();
        uint32_t expected = 100u / eip_hal_clock_resolution_ms();

        start = last;
        while (eip_hal_millis() - start < 100u) {
            uint32_t now = eip_hal_millis();
            if (now != last) {
                distinct++;
                last = now;
            }
        }
        printf("clock: %lu distinct values in 100 ms\n", (unsigned long)distinct);
        EXPECT(distinct >= expected / 2u, "clock really steps at its reported resolution");
    }
}

static void TestNetif(void)
{
    EipNetifInfo info;
    int i;

    EXPECT(eip_hal_netif_get(0, &info) == EIP_OK, "primary interface");
    printf("netif 0: %u.%u.%u.%u/%u.%u.%u.%u gw %u.%u.%u.%u %02x:%02x:%02x:%02x:%02x:%02x link %u %lu Mbit/s dhcp %u host '%s'\n",
           info.ip[0], info.ip[1], info.ip[2], info.ip[3], info.netmask[0], info.netmask[1], info.netmask[2],
           info.netmask[3], info.gateway[0], info.gateway[1], info.gateway[2], info.gateway[3], info.mac[0],
           info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5], info.link_up,
           (unsigned long)info.speed_mbps, info.dhcp, info.host_name);
    EXPECT(info.ip[0] != 0 && info.ip[0] != 127, "primary interface is a real address");
    EXPECT(info.netmask[0] != 0, "netmask known");
    for (i = 0; i < 255; i++) {
        if (eip_hal_netif_get((uint8_t)i, &info) != EIP_OK) {
            break;
        }
    }
    EXPECT(i > 0 && i < 255, "EIP_ERR_ARG past the last interface");
    EXPECT(eip_hal_netif_get(250, &info) == EIP_ERR_ARG, "far index is EIP_ERR_ARG");
}

static void TestUdp(void)
{
    static const uint8_t any_ip[4] = {0, 0, 0, 0};
    uint8_t big[100];
    uint8_t buf[200];
    uint8_t ip[4];
    uint16_t port = 0;
    uint16_t free_port = FreeUdpPort();
    eip_sock_t a;
    eip_sock_t b;
    uint32_t start;
    int n;

    EXPECT(free_port != 0, "datagram sender port reported");

    /* A busy port must fail: the scanner relies on it to stay off UDP 2222. */
    a = eip_hal_udp_open(47802, 0);
    b = eip_hal_udp_open(47802, 0);
    EXPECT(a >= 0, "udp open");
    EXPECT(b == EIP_SOCK_INVALID, "second bind of a busy UDP port fails");
    eip_hal_udp_close(b);

    b = eip_hal_udp_open(0, EIP_UDP_BROADCAST);
    EXPECT(b >= 0 && b != a, "udp open on any port");

    /* timeout 0 returns at once, a timeout waits about that long. */
    start = eip_hal_millis();
    EXPECT(eip_hal_udp_recvfrom(a, buf, sizeof buf, 0, 0, 0) == 0, "recvfrom timeout 0, nothing waiting");
    EXPECT(eip_hal_millis() - start < 20u, "recvfrom timeout 0 does not wait");
    start = eip_hal_millis();
    EXPECT(eip_hal_udp_recvfrom(a, buf, sizeof buf, 0, 0, 50) == 0, "recvfrom times out with 0");
    n = (int)(eip_hal_millis() - start);
    EXPECT(n >= 40 && n < 150, "recvfrom waits about timeout_ms");

    /* Round trip, sender address, NULL ip/port. */
    memset(big, 0x5A, sizeof big);
    EXPECT(eip_hal_udp_sendto(b, kLocal, 47802, big, sizeof big) == EIP_OK, "udp send");
    n = eip_hal_udp_recvfrom(a, buf, sizeof buf, ip, &port, 500);
    EXPECT(n == (int)sizeof big && memcmp(buf, big, sizeof big) == 0, "udp datagram arrives whole");
    EXPECT(memcmp(ip, kLocal, 4) == 0 && port != 0, "udp sender reported");
    EXPECT(eip_hal_udp_sendto(b, kLocal, 47802, big, 10) == EIP_OK, "udp send 2");
    EXPECT(eip_hal_udp_recvfrom(a, buf, sizeof buf, 0, 0, 500) == 10, "udp recv with NULL ip and port");

    /* A datagram longer than cap is truncated to cap, not an error. */
    EXPECT(eip_hal_udp_sendto(b, kLocal, 47802, big, sizeof big) == EIP_OK, "udp send 3");
    n = eip_hal_udp_recvfrom(a, buf, 10, 0, 0, 500);
    EXPECT(n == 10 && buf[9] == 0x5A, "long datagram truncated to cap");

    /* Sending to a closed port draws an ICMP port-unreachable: not fatal. */
    EXPECT(eip_hal_udp_sendto(a, kLocal, 47803, big, 4) == EIP_OK, "udp send to closed port");
    Pause(30);
    EXPECT(eip_hal_udp_recvfrom(a, buf, sizeof buf, 0, 0, 50) == 0, "ICMP port-unreachable is not an error");
    EXPECT(eip_hal_udp_sendto(b, kLocal, 47802, big, 7) == EIP_OK, "udp send after ICMP");
    EXPECT(eip_hal_udp_recvfrom(a, buf, sizeof buf, 0, 0, 500) == 7, "socket still works after ICMP");

    eip_hal_udp_close(a);
    eip_hal_udp_close(b);
    a = eip_hal_udp_open(47802, 0);
    EXPECT(a >= 0, "port is free again after close");
    eip_hal_udp_close(a);
    (void)any_ip;
}

static void TestTcp(void)
{
    uint8_t ip[4];
    uint8_t buf[4096];
    static uint8_t block[60000];
    eip_sock_t l;
    eip_sock_t l2;
    eip_sock_t c;
    eip_sock_t s = EIP_SOCK_INVALID;
    uint32_t start;
    int i;
    int n;

    l = eip_hal_tcp_listen(47804, 4);
    EXPECT(l >= 0, "tcp listen");
    l2 = eip_hal_tcp_listen(47804, 4);
    EXPECT(l2 == EIP_SOCK_INVALID, "second listen on a busy port fails");
    eip_hal_tcp_close(l2);

    start = eip_hal_millis();
    EXPECT(eip_hal_tcp_accept(l, ip) == EIP_SOCK_INVALID, "accept with nobody waiting");
    EXPECT(eip_hal_millis() - start < 20u, "accept never blocks");

    c = eip_hal_tcp_connect(kLocal, 47804, 1000);
    EXPECT(c >= 0, "tcp connect");
    for (i = 0; i < 200 && s < 0; i++) {
        s = eip_hal_tcp_accept(l, ip);
        if (s < 0) {
            eip_hal_yield();
        }
    }
    EXPECT(s >= 0, "tcp accept");
    EXPECT(memcmp(ip, kLocal, 4) == 0, "accept reports the peer");
    EXPECT(eip_hal_tcp_local_ip(c, ip) == EIP_OK && memcmp(ip, kLocal, 4) == 0, "tcp local ip");

    /* eip_hal_wait: timeout, then readable. */
    start = eip_hal_millis();
    EXPECT(eip_hal_wait(&s, 1, 50) == 0, "wait times out with 0");
    n = (int)(eip_hal_millis() - start);
    EXPECT(n >= 40 && n < 150, "wait waits about timeout_ms");
    EXPECT(eip_hal_tcp_recv(s, buf, sizeof buf, 0) == 0, "recv timeout 0, nothing waiting");
    EXPECT(eip_hal_tcp_recv(s, buf, sizeof buf, 30) == 0, "recv times out with 0");

    EXPECT(eip_hal_tcp_send(c, (const uint8_t *)"hello", 5) == EIP_OK, "tcp send");
    EXPECT(eip_hal_wait(&s, 1, 500) > 0, "wait sees readable data");
    n = eip_hal_tcp_recv(s, buf, sizeof buf, 500);
    EXPECT(n == 5 && memcmp(buf, "hello", 5) == 0, "tcp recv");

    {
        eip_sock_t set[3] = {EIP_SOCK_INVALID, l, EIP_SOCK_INVALID};
        eip_sock_t c2;
        eip_sock_t s2 = EIP_SOCK_INVALID;
        EXPECT(eip_hal_wait(set, 3, 0) == 0, "wait skips invalid handles");
        c2 = eip_hal_tcp_connect(kLocal, 47804, 1000);
        EXPECT(eip_hal_wait(set, 3, 500) > 0, "wait sees a pending connection");
        s2 = eip_hal_tcp_accept(l, ip);
        EXPECT(s2 >= 0, "accept after wait");
        eip_hal_tcp_close(s2);
        eip_hal_tcp_close(c2);
    }
    {
        eip_sock_t none[2] = {EIP_SOCK_INVALID, EIP_SOCK_INVALID};
        start = eip_hal_millis();
        EXPECT(eip_hal_wait(none, 2, 30) == 0, "wait with no sockets is a sleep");
        n = (int)(eip_hal_millis() - start);
        EXPECT(n >= 20 && n < 120, "wait with no sockets still waits");
    }

    /* A peer that never reads: send gives up within its bounded time. */
    start = eip_hal_millis();
    for (i = 0; i < 2000; i++) {
        if (eip_hal_tcp_send(c, block, 60000) != EIP_OK) {
            break;
        }
    }
    EXPECT(i < 2000, "send gives up when the peer stops reading");
    printf("tcp: send blocked after %d x 60000 bytes\n", i);

    /* Peer close: EIP_ERR_IO once the data is drained. */
    eip_hal_tcp_close(c);
    start = eip_hal_millis();
    do {
        n = eip_hal_tcp_recv(s, buf, sizeof buf, 100);
    } while (n > 0 && eip_hal_millis() - start < 5000u);
    EXPECT(n == EIP_ERR_IO, "recv reports a closed peer");
    eip_hal_tcp_close(s);
    eip_hal_tcp_close(l);

    /* Nobody listening: fails, and within timeout_ms. */
    start = eip_hal_millis();
    c = eip_hal_tcp_connect(kLocal, 47804, 300);
    n = (int)(eip_hal_millis() - start);
    EXPECT(c == EIP_SOCK_INVALID, "connect to a closed port fails");
    EXPECT(n < 500, "connect respects timeout_ms");
    eip_hal_tcp_close(c);
}

static void TestHandles(void)
{
    uint8_t buf[4];
    uint8_t ip[4];
    eip_sock_t socks[6];
    int i;

    /* Invalid handles are refused, not crashes. */
    EXPECT(eip_hal_tcp_send(EIP_SOCK_INVALID, buf, 1) == EIP_ERR_IO, "send on invalid handle");
    EXPECT(eip_hal_tcp_recv(EIP_SOCK_INVALID, buf, 1, 0) == EIP_ERR_IO, "recv on invalid handle");
    EXPECT(eip_hal_udp_sendto(EIP_SOCK_INVALID, kLocal, 1, buf, 1) == EIP_ERR_IO, "sendto on invalid handle");
    EXPECT(eip_hal_udp_recvfrom(EIP_SOCK_INVALID, buf, 1, 0, 0, 0) == EIP_ERR_IO, "recvfrom on invalid handle");
    EXPECT(eip_hal_tcp_local_ip(EIP_SOCK_INVALID, ip) == EIP_ERR_IO, "local ip on invalid handle");
    EXPECT(eip_hal_tcp_accept(EIP_SOCK_INVALID, ip) == EIP_SOCK_INVALID, "accept on invalid handle");
    eip_hal_tcp_close(EIP_SOCK_INVALID);
    eip_hal_udp_close(EIP_SOCK_INVALID);

    /* Handles are distinct while open, and closing frees them. */
    for (i = 0; i < 6; i++) {
        int j;
        socks[i] = eip_hal_udp_open(0, 0);
        EXPECT(socks[i] >= 0, "handle is non-negative");
        for (j = 0; j < i; j++) {
            EXPECT(socks[i] != socks[j], "open handles are distinct");
        }
    }
    for (i = 0; i < 6; i++) {
        eip_hal_udp_close(socks[i]);
    }
}

int main(void)
{
    TestClock();
    TestNetif();
    TestHandles();
    TestUdp();
    TestTcp();
    printf(g_failed ? "FAILED (%d)\n" : "PASS\n", g_failed);
    return g_failed ? 1 : 0;
}
