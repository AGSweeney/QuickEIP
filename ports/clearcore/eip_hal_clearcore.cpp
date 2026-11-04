/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP port: Teknic ClearCore (SAME53, bare metal, lwIP raw API).
 *
 * Build with ports/lwip_raw/eip_hal_lwip_raw.c. This file supplies the
 * clock, the stack poll, and interface information from EthernetMgr, plus an
 * empty lock and an adapter runner that starts nothing.
 *
 * ClearCore has no RTOS, so the adapter runs in poll mode:
 *
 *     EthernetMgr.Setup();
 *     EthernetMgr.DhcpBegin();          // or LocalIp(...)
 *     eip_adapter_start(&config);       // starts no task
 *     while (true) {
 *         eip_adapter_poll();           // every pass of the main loop
 *         ... motion and I/O ...
 *     }
 *
 * eip_adapter_poll reaches EthernetMgr.Refresh() through the zero-timeout
 * receives in ports/lwip_raw. T->O packets go out only from poll, so the
 * main loop's timing is the RPI jitter.
 *
 * The scanner calls (eip_session_open, eip_tag_read, ...) block until they
 * finish or time out. While they wait they keep calling EthernetMgr.Refresh(),
 * and that also lets the adapter's sockets receive. ClearCore motion runs
 * from interrupts and keeps running during a blocking call; anything else in
 * the main loop waits, eip_adapter_poll included, so no T->O packets go out
 * during a blocking call. Keep scanner timeouts short (timeout_ms).
 *
 * The ClearCore lwIP build has a 4 KB heap (MEM_SIZE) and 5 TCP PCBs. That
 * fits the adapter with 2-3 TCP clients and one scanner session. The library
 * default for EIP_ADAPTER_MAX_CLIENTS is 4, so set it to 2 or 3 for this
 * port; see docs/INTEGRATION.md.
 */
#include "ClearCore.h"

#include "eip_clearcore.h"
#include "eip_hal.h"
#include "eip_server.h"

#include <string.h>

#include "lwip/dhcp.h"
#include "lwip/netif.h"

static char s_host_name[65];

extern "C" void eip_clearcore_set_host_name(const char *name)
{
    if (name == 0) {
        name = "";
    }
    strncpy(s_host_name, name, sizeof s_host_name - 1u);
    s_host_name[sizeof s_host_name - 1u] = '\0';
}

static void StoreIp(uint8_t out[4], uint32_t lwip_addr)
{
    memcpy(out, &lwip_addr, 4); /* lwIP keeps addresses in network order */
}

extern "C" uint32_t eip_hal_millis(void)
{
    return Milliseconds();
}

extern "C" uint32_t eip_hal_clock_resolution_ms(void)
{
    return 1u;
}

/* Never sleeps. Every wait loop in ports/lwip_raw spins on this, and all lwIP
 * callbacks run inside it. */
extern "C" void eip_hal_yield(void)
{
    EthernetMgr.Refresh();
}

/* Empty: there is one context. This holds only while nothing calls QuickEIP
 * from an interrupt handler. */
extern "C" void eip_hal_lock(void)
{
}

extern "C" void eip_hal_unlock(void)
{
}

/* One interface. dns2 stays zero. */
extern "C" int eip_hal_netif_get(uint8_t index, EipNetifInfo *info)
{
    const uint8_t *mac;

    memset(info, 0, sizeof *info);
    if (index != 0) {
        return EIP_ERR_ARG;
    }
    StoreIp(info->ip, (uint32_t)EthernetMgr.LocalIp());
    StoreIp(info->netmask, (uint32_t)EthernetMgr.NetmaskIp());
    StoreIp(info->gateway, (uint32_t)EthernetMgr.GatewayIp());
    StoreIp(info->dns1, (uint32_t)EthernetMgr.DnsIp());
    mac = EthernetMgr.MacAddress();
    if (mac != 0) {
        memcpy(info->mac, mac, 6);
    }
#if LWIP_DHCP
    /* Reads 1 only once DHCP has supplied an address, so a device still
     * waiting for its lease reports static. */
    {
        struct netif *nif = EthernetMgr.MacInterface();
        info->dhcp = (nif != 0 && netif_dhcp_data(nif) != 0 && dhcp_supplied_address(nif)) ? 1u : 0u;
    }
#endif
    memcpy(info->host_name, s_host_name, sizeof info->host_name);
    info->link_known = 1;
    info->link_up = EthernetMgr.PhyLinkActive() ? 1u : 0u;
    /* EthernetMgr does not expose the negotiated speed; report 100 Mbit/s full duplex. */
    info->speed_mbps = info->link_up ? 100u : 0u;
    info->full_duplex = info->link_up;
    info->autonegotiated = info->link_up;
    return EIP_OK;
}

extern "C" int eip_hal_adapter_start_task(struct EipServer *server)
{
    (void)server; /* poll mode: the application calls eip_adapter_poll() */
    return EIP_OK;
}
