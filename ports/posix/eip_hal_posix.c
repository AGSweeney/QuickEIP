/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP port: POSIX (Linux, macOS, WSL).
 *
 * Build with ports/bsd_sockets/eip_hal_bsd_sockets.c. Link with -lpthread.
 * Used for host tests, desktop tools, and as the reference port.
 *
 * The adapter runs in a detached thread that never ends;
 * EIP_ADAPTER_TASK_STACK and EIP_ADAPTER_TASK_PRIORITY are not used. Interface
 * information is partial: no gateway, DNS or DHCP state, and the link speed
 * is a fixed value.
 */
/* Ask for the BSD and Darwin declarations a strict C99 build would hide. */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1

#include "eip_hal.h"
#include "eip_server.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <netpacket/packet.h>
#elif defined(__APPLE__)
#include <net/if_dl.h>
#endif

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* CLOCK_MONOTONIC, so setting the wall clock cannot disturb timeouts. The cast
 * gives the 2^32 ms wrap. */
uint32_t eip_hal_millis(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

uint32_t eip_hal_clock_resolution_ms(void)
{
    return 1u;
}

void eip_hal_yield(void)
{
    usleep(1000);
}

void eip_hal_lock(void)
{
    pthread_mutex_lock(&s_lock);
}

void eip_hal_unlock(void)
{
    pthread_mutex_unlock(&s_lock);
}

/* Interfaces in getifaddrs order: non-loopback IPv4 interfaces that are up.
 * Set EIP_NETIF=<name> in the environment to put one interface first. That
 * one needs only an IPv4 address, so it may be loopback or down. An interface
 * appears once per IPv4 address; one with no address is not listed at all, so
 * it gets EIP_ERR_ARG, not the contract's 0.0.0.0. */
static int Usable(const struct ifaddrs *ifa)
{
    return ifa->ifa_addr != 0 && ifa->ifa_addr->sa_family == AF_INET && (ifa->ifa_flags & IFF_UP) &&
           !(ifa->ifa_flags & IFF_LOOPBACK);
}

/* getifaddrs lists the link-layer address as a separate entry under the same
 * name. On systems other than Linux and macOS the MAC stays zero. */
static void FillMac(const struct ifaddrs *all, const char *name, uint8_t mac[6])
{
    const struct ifaddrs *ifa;

    for (ifa = all; ifa != 0; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == 0 || strcmp(ifa->ifa_name, name) != 0) {
            continue;
        }
#if defined(__linux__)
        if (ifa->ifa_addr->sa_family == AF_PACKET) {
            const struct sockaddr_ll *ll = (const struct sockaddr_ll *)ifa->ifa_addr;
            if (ll->sll_halen == 6) {
                memcpy(mac, ll->sll_addr, 6);
            }
        }
#elif defined(__APPLE__)
        if (ifa->ifa_addr->sa_family == AF_LINK) {
            const struct sockaddr_dl *dl = (const struct sockaddr_dl *)ifa->ifa_addr;
            if (dl->sdl_alen == 6) {
                memcpy(mac, LLADDR(dl), 6);
            }
        }
#endif
    }
}

int eip_hal_netif_get(uint8_t index, EipNetifInfo *info)
{
    struct ifaddrs *all = 0;
    const struct ifaddrs *ifa;
    const struct ifaddrs *pick = 0;
    const char *prefer = getenv("EIP_NETIF");
    uint8_t seen = 0;

    memset(info, 0, sizeof *info);
    if (getifaddrs(&all) != 0) {
        return EIP_ERR_ARG;
    }
    if (prefer != 0) {
        for (ifa = all; ifa != 0; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr != 0 && ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, prefer) == 0) {
                if (index == 0) {
                    pick = ifa;
                }
                seen = 1; /* it holds index 0, so the others count from 1 */
                break;
            }
        }
    }
    for (ifa = all; ifa != 0 && pick == 0; ifa = ifa->ifa_next) {
        if (!Usable(ifa) || (prefer != 0 && strcmp(ifa->ifa_name, prefer) == 0)) {
            continue;
        }
        if (seen == index) {
            pick = ifa;
        }
        seen++;
    }
    if (pick == 0) {
        freeifaddrs(all);
        return EIP_ERR_ARG;
    }
    memcpy(info->ip, &((const struct sockaddr_in *)pick->ifa_addr)->sin_addr.s_addr, 4);
    if (pick->ifa_netmask != 0) {
        memcpy(info->netmask, &((const struct sockaddr_in *)pick->ifa_netmask)->sin_addr.s_addr, 4);
    }
    FillMac(all, pick->ifa_name, info->mac);
    /* link_up is real (IFF_RUNNING). Speed, duplex and negotiation are fixed
     * values that follow it, not read from the system. */
    info->link_known = 1;
    info->link_up = (pick->ifa_flags & IFF_RUNNING) ? 1u : 0u;
    info->speed_mbps = info->link_up ? 100u : 0u;
    info->full_duplex = info->link_up;
    info->autonegotiated = info->link_up;
    (void)gethostname(info->host_name, sizeof info->host_name - 1u);
    freeifaddrs(all);
    return EIP_OK;
}

static void *AdapterThread(void *arg)
{
    EipServer *server = (EipServer *)arg;
    for (;;) {
        eip_server_service(server, EIP_ADAPTER_IDLE_WAIT_MS);
    }
    return 0;
}

int eip_hal_adapter_start_task(struct EipServer *server)
{
    pthread_t thread;
    pthread_attr_t attr;
    int rc;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    rc = pthread_create(&thread, &attr, AdapterThread, server);
    pthread_attr_destroy(&attr);
    return (rc == 0) ? EIP_OK : EIP_ERR_IO;
}
