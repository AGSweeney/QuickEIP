/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP port: ESP-IDF 5.x (FreeRTOS, lwIP sockets through the VFS).
 *
 * Build with ports/bsd_sockets/eip_hal_bsd_sockets.c (the root
 * CMakeLists.txt does this when QuickEIP is used as an IDF component).
 * Also works under Arduino-ESP32, which is ESP-IDF underneath.
 *
 * Interface 0 is the netif set with eip_esp_set_netif(), or else the
 * first of ETH_DEF, WIFI_STA_DEF, WIFI_AP_DEF that exists. Link speed is
 * not available from esp_netif: pass EipAdapterConfig.link_info to report
 * it (read it from your esp_eth handle).
 *
 * The adapter runs in its own FreeRTOS task. EIP_ADAPTER_TASK_STACK and
 * EIP_ADAPTER_TASK_PRIORITY take precedence when non-zero, otherwise the
 * EIP_ESP_TASK_* defaults below apply. EIP_ESP_TASK_CORE pins the task, and
 * EIP_ESP_TASK_WDT=1 adds it to the task watchdog.
 */
#include "eip_hal.h"
#include "eip_server.h"
#include "eip_esp_idf.h"

#include <string.h>

#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if defined(EIP_ESP_TASK_WDT) && EIP_ESP_TASK_WDT
#include "esp_task_wdt.h"
#endif

#ifndef EIP_ESP_TASK_STACK
#define EIP_ESP_TASK_STACK 6144
#endif
#ifndef EIP_ESP_TASK_PRIORITY
#define EIP_ESP_TASK_PRIORITY 12
#endif
#ifndef EIP_ESP_TASK_CORE
#define EIP_ESP_TASK_CORE tskNO_AFFINITY
#endif

static esp_netif_t *s_netifs[4];
static uint8_t s_netif_count;
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

/* No lock: Netif() reads this list unguarded, so it is set before the adapter
 * starts. Entries past the fourth are ignored. */
void eip_esp_set_netif(esp_netif_t *const *netifs, uint8_t count)
{
    uint8_t i;

    if (count > sizeof s_netifs / sizeof s_netifs[0]) {
        count = sizeof s_netifs / sizeof s_netifs[0];
    }
    for (i = 0; i < count; i++) {
        s_netifs[i] = netifs[i];
    }
    s_netif_count = count;
}

/* Default indexes count only the netifs that exist, and are looked up on every
 * call, so a netif created later shifts the ones after it. */
static esp_netif_t *Netif(uint8_t index)
{
    static const char *const keys[] = {"ETH_DEF", "WIFI_STA_DEF", "WIFI_AP_DEF"};
    uint8_t seen = 0;
    size_t i;

    if (s_netif_count != 0) {
        return (index < s_netif_count) ? s_netifs[index] : NULL;
    }
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(keys[i]);
        if (nif != NULL) {
            if (seen == index) {
                return nif;
            }
            seen++;
        }
    }
    return NULL;
}

/* The cast from the 64-bit microsecond timer gives the 2^32 ms wrap. */
uint32_t eip_hal_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t eip_hal_clock_resolution_ms(void)
{
    return 1u;
}

void eip_hal_yield(void)
{
    vTaskDelay(1);
}

/* The contract has no init call, so the mutex is created on first use. The
 * spinlock and second test stop two tasks creating it at once. Its storage is
 * static, so creation allocates nothing. Unlock only ever follows a lock, so
 * s_lock exists by then. */
void eip_hal_lock(void)
{
    if (s_lock == NULL) {
        taskENTER_CRITICAL(&s_init_mux);
        if (s_lock == NULL) {
            s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        }
        taskEXIT_CRITICAL(&s_init_mux);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void eip_hal_unlock(void)
{
    xSemaphoreGive(s_lock);
}

int eip_hal_netif_get(uint8_t index, EipNetifInfo *info)
{
    esp_netif_t *nif = Netif(index);
    esp_netif_ip_info_t ip;
    esp_netif_dns_info_t dns;
    esp_netif_dhcp_status_t dhcp = ESP_NETIF_DHCP_INIT;
    const char *host = NULL;

    memset(info, 0, sizeof *info);
    if (nif == NULL) {
        return EIP_ERR_ARG;
    }
    if (esp_netif_get_ip_info(nif, &ip) == ESP_OK) {
        memcpy(info->ip, &ip.ip.addr, 4);
        memcpy(info->netmask, &ip.netmask.addr, 4);
        memcpy(info->gateway, &ip.gw.addr, 4);
    }
    (void)esp_netif_get_mac(nif, info->mac);
    if (esp_netif_dhcpc_get_status(nif, &dhcp) == ESP_OK) {
        /* Tracks the DHCP client being started, not an address having come
         * from it. */
        info->dhcp = (dhcp == ESP_NETIF_DHCP_STARTED) ? 1u : 0u;
    }
    if (esp_netif_get_hostname(nif, &host) == ESP_OK && host != NULL) {
        strncpy(info->host_name, host, sizeof info->host_name - 1u);
    }
    memset(&dns, 0, sizeof dns);
    if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
        memcpy(info->dns1, &dns.ip.u_addr.ip4.addr, 4);
    }
    memset(&dns, 0, sizeof dns);
    if (esp_netif_get_dns_info(nif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
        memcpy(info->dns2, &dns.ip.u_addr.ip4.addr, 4);
    }
    /* link_known stays 0: the server reports "up" while there is an address,
     * unless EipAdapterConfig.link_info says otherwise. */
    return EIP_OK;
}

static void AdapterTask(void *arg)
{
    EipServer *server = (EipServer *)arg;

#if defined(EIP_ESP_TASK_WDT) && EIP_ESP_TASK_WDT
    esp_task_wdt_add(NULL); /* a hung adapter task trips the task watchdog */
#endif
    for (;;) {
#if defined(EIP_ESP_TASK_WDT) && EIP_ESP_TASK_WDT
        /* Fed once per pass, so a slow apply_interface or reset callback,
         * which run inside the pass, counts against the watchdog. */
        esp_task_wdt_reset();
#endif
        eip_server_service(server, EIP_ADAPTER_IDLE_WAIT_MS);
    }
}

/* The stack size is in bytes. The task handle is not kept: the task runs for
 * good. */
int eip_hal_adapter_start_task(struct EipServer *server)
{
    uint32_t stack = (EIP_ADAPTER_TASK_STACK != 0) ? (uint32_t)EIP_ADAPTER_TASK_STACK : EIP_ESP_TASK_STACK;
    UBaseType_t prio = (EIP_ADAPTER_TASK_PRIORITY != 0) ? (UBaseType_t)EIP_ADAPTER_TASK_PRIORITY
                                                         : (UBaseType_t)EIP_ESP_TASK_PRIORITY;

    if (xTaskCreatePinnedToCore(AdapterTask, "eip", stack, server, prio, NULL, EIP_ESP_TASK_CORE) != pdPASS) {
        return EIP_ERR_IO;
    }
    return EIP_OK;
}
