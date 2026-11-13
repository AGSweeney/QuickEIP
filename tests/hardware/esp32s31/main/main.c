/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP hardware test firmware: ESP32-S31, internal EMAC, YT8531 RGMII PHY.
 * Logs every adapter event, indicator change and stored-settings write on the console, keeps the
 * stored settings in NVS, and restarts on an Identity Reset. */
#include <string.h>

#include "eip.h"
#include "eip_esp_idf.h"

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "hwtest";
static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static volatile int s_link;
static volatile int s_got_ip;

static const EipAssemblyConfig kAssemblies[] = {
    {150, EIP_ASM_CONSUMED, 16, "Outputs", NULL, 0},
    {100, EIP_ASM_PRODUCED, 16, "Inputs", NULL, 0},
    {151, EIP_ASM_CONFIG, 0, "Config", NULL, 0},
};

static esp_err_t Yt8531ExtRmw(uint32_t ext_reg, uint32_t clear, uint32_t set)
{
    uint32_t v = ext_reg;
    esp_eth_phy_reg_rw_data_t r = {.reg_value_p = &v};
    r.reg_addr = 0x1E;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &r), TAG, "ext addr");
    r.reg_addr = 0x1F;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_READ_PHY_REG, &r), TAG, "ext read");
    v = (v & ~clear) | set;
    return esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &r);
}

static void OnEth(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_link = 1;
        ESP_LOGI(TAG, "link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_link = 0;
        ESP_LOGW(TAG, "link down");
    }
}

static void OnIp(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    s_got_ip = 1;
    ESP_LOGI(TAG, "IP " IPSTR " mask " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask),
             IP2STR(&ev->ip_info.gw));
}

static esp_err_t EthStart(void)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    bool an = true;

    phy_cfg.phy_addr = -1;
    phy_cfg.reset_gpio_num = 7;
    phy_cfg.hw_reset_assert_time_us = 10000;
    phy_cfg.post_hw_reset_delay_ms = 10;
    emac.smi_gpio.mdc_num = 5;
    emac.smi_gpio.mdio_num = 6;
    emac.interface = EMAC_DATA_INTERFACE_RGMII;
    emac.clock_config.rgmii.clock_rx_gpio = 14;
    emac.clock_config.rgmii.clock_tx_gpio = 13;
    emac.clock_config.rgmii.clock_phy_ref_gpio = -1;
    emac.emac_dataif_gpio.rgmii.tx_ctl_num = 12;
    emac.emac_dataif_gpio.rgmii.txd0_num = 8;
    emac.emac_dataif_gpio.rgmii.txd1_num = 9;
    emac.emac_dataif_gpio.rgmii.txd2_num = 10;
    emac.emac_dataif_gpio.rgmii.txd3_num = 11;
    emac.emac_dataif_gpio.rgmii.rx_ctl_num = 15;
    emac.emac_dataif_gpio.rgmii.rxd0_num = 19;
    emac.emac_dataif_gpio.rgmii.rxd1_num = 18;
    emac.emac_dataif_gpio.rgmii.rxd2_num = 17;
    emac.emac_dataif_gpio.rgmii.rxd3_num = 16;

    mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
    phy = esp_eth_phy_new_generic(&phy_cfg);
    if (mac == NULL || phy == NULL) {
        return ESP_FAIL;
    }
    {
        esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
        ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_cfg, &s_eth), TAG, "driver install");
    }
    /* YT8531: autonegotiation back on after the hardware reset, and about 2 ns RGMII clock delays. */
    (void)esp_eth_ioctl(s_eth, ETH_CMD_S_AUTONEGO, &an);
    (void)Yt8531ExtRmw(0xA001, 0, 1U << 8);
    (void)Yt8531ExtRmw(0xA003, 0xFFU, (13U << 4) | 13U);

    s_netif = esp_netif_new(&ncfg);
    esp_netif_set_hostname(s_netif, "quickeip-hwtest");
    esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, OnEth, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, OnIp, NULL));
    return esp_eth_start(s_eth);
}

/* ---- adapter callbacks -------------------------------------------------- */

static int LinkInfo(void *ctx, uint32_t *speed_mbps, int *full_duplex, int *autonegotiated)
{
    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;

    esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed);
    esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
    *speed_mbps = (speed == ETH_SPEED_10M) ? 10u : (speed == ETH_SPEED_100M) ? 100u : 1000u;
    *full_duplex = (duplex == ETH_DUPLEX_FULL);
    *autonegotiated = 1;
    return s_link;
}

static void OnEvent(void *ctx, uint8_t event)
{
    static const char *const names[] = {"?", "IO_OPENED", "IO_CLOSED", "IO_TIMEOUT", "RUN", "IDLE"};
    ESP_LOGI(TAG, "EVENT %s", (event <= 5) ? names[event] : "?");
}

static void OnLeds(void *ctx, uint8_t module, uint8_t network)
{
    static const char *const names[] = {"OFF", "GREEN", "GREEN_FLASH", "RED", "RED_FLASH"};
    ESP_LOGI(TAG, "LEDS module=%s network=%s", names[module % 5], names[network % 5]);
}

static void StoreNv(void *ctx, const EipNvSettings *nv)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("eip", NVS_READWRITE, &h);

    if (err == ESP_OK) {
        err = nvs_set_blob(h, "nv", nv, sizeof *nv);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "STORE_NV timeout=%u dscp=%u,%u,%u,%u,%u -> %s", nv->encap_timeout_s, nv->dscp_urgent,
             nv->dscp_scheduled, nv->dscp_high, nv->dscp_low, nv->dscp_explicit, esp_err_to_name(err));
}

static void Reset(void *ctx, uint8_t type)
{
    ESP_LOGW(TAG, "RESET type %u", type);
    if (type == 1) {
        nvs_handle_t h;
        if (nvs_open("eip", NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void app_main(void)
{
    static EipNvSettings nv;
    EipAdapterConfig config;
    nvs_handle_t h;
    size_t size = sizeof nv;
    int have_nv = 0;
    uint32_t counter = 0;
    int waited;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(EthStart());

    /* DHCP for 10 s, then a fixed address on the same subnet as the PC's 172.16.82.x adapter. */
    for (waited = 0; waited < 100 && !s_got_ip; waited++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_got_ip) {
        esp_netif_ip_info_t info = {0};
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_str_to_ip4("172.16.82.240", &info.ip);
        esp_netif_str_to_ip4("255.255.255.0", &info.netmask);
        esp_netif_set_ip_info(s_netif, &info);
        ESP_LOGW(TAG, "no DHCP lease: using 172.16.82.240");
    }

    if (nvs_open("eip", NVS_READONLY, &h) == ESP_OK) {
        have_nv = (nvs_get_blob(h, "nv", &nv, &size) == ESP_OK && size == sizeof nv);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "stored settings %s", have_nv ? "read from NVS" : "not found: defaults");

    eip_esp_set_netif(&s_netif, 1);
    memset(&config, 0, sizeof config);
    config.vendor_id = 65000;
    config.device_type = 43;
    config.product_code = 31;
    config.revision_major = 1;
    config.revision_minor = 1;
    config.product_name = "QuickEIP S31 Test";
    config.assemblies = kAssemblies;
    config.assembly_count = sizeof kAssemblies / sizeof kAssemblies[0];
    config.link_info = LinkInfo;
    config.reset = Reset;
    config.on_event = OnEvent;
    config.on_leds = OnLeds;
    config.nv = have_nv ? &nv : NULL;
    config.store_nv = StoreNv;
    if (eip_adapter_start(&config) != EIP_OK) {
        ESP_LOGE(TAG, "adapter did not start");
        return;
    }
    ESP_LOGI(TAG, "adapter started");

    for (;;) {
        uint8_t outputs[16];
        uint8_t inputs[16];
        uint16_t len = sizeof outputs;
        EipAdapterView view;

        eip_adapter_read(150, outputs, &len);
        memset(inputs, 0, sizeof inputs);
        eip_store_le32(inputs, counter++);
        inputs[4] = outputs[0];
        eip_adapter_write(100, inputs, sizeof inputs);
        if ((counter % 50u) == 0 && eip_adapter_snapshot(&view) == EIP_OK) {
            ESP_LOGI(TAG, "STATUS listening %u qos %u io %u run %u rx %lu tx %lu timeouts %lu clients %u",
                     view.listening, view.qos, view.io_open, view.run, (unsigned long)view.rx_packets,
                     (unsigned long)view.tx_packets, (unsigned long)view.timeouts, view.tcp_clients);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
