/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP on ESP-IDF: an adapter with one input and one output assembly.
 *
 * The network comes up through protocol_examples_common
 * (idf.py menuconfig -> Example Connection Configuration: Wi-Fi or Ethernet).
 * For production use wired Ethernet: Wi-Fi jitter can trip Class 1 timeouts.
 *
 * Build: idf.py set-target esp32 && idf.py build
 *
 * To adapt it: bring up your own netif in place of example_connect(), set the identity to your
 * own vendor id and product, make kAssemblies match your EDS, and replace the counter and echo in
 * the loop with your application's data. */
#include <string.h>

#include "eip.h"
#include "eip_esp_idf.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

static const char *TAG = "app";

/* Instances and sizes must match the EDS: the adapter refuses a Forward_Open that names others
 * (table in docs/INTEGRATION.md 3.3). 150 is O->T, 100 is T->O. The size-0 config point accepts a
 * connection path with a config instance and no data. */
static const EipAssemblyConfig kAssemblies[] = {
    /* instance, direction, size, name, initial, implicit_only */
    {150, EIP_ASM_CONSUMED, 16, "Outputs", NULL, 0},
    {100, EIP_ASM_PRODUCED, 16, "Inputs", NULL, 0},
    {151, EIP_ASM_CONFIG, 0, "Config", NULL, 0},
};

void app_main(void)
{
    EipAdapterConfig config;
    esp_netif_t *netif;
    uint32_t counter = 0;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    /* Must come before eip_adapter_start(). Index 0 is the interface the adapter reports. Without
     * this call the port uses ETH_DEF, WIFI_STA_DEF, WIFI_AP_DEF, whichever exist. */
    netif = get_example_netif();
    eip_esp_set_netif(&netif, 1);

    /* serial_number stays 0, so the adapter derives it from the MAC. esp_netif reports no link
     * speed or duplex: on Ethernet set config.link_info (docs/INTEGRATION.md, ESP-IDF section). */
    memset(&config, 0, sizeof config);
    config.vendor_id = 65000;            /* replace with your ODVA vendor id */
    config.device_type = 43;             /* generic device */
    config.product_code = 1;
    config.revision_major = 1;
    config.revision_minor = 1;
    config.product_name = "QuickEIP ESP32";
    config.assemblies = kAssemblies;
    config.assembly_count = sizeof kAssemblies / sizeof kAssemblies[0];
    /* Starts the adapter task: this file has no polling to do. */
    if (eip_adapter_start(&config) != EIP_OK) {
        ESP_LOGE(TAG, "EtherNet/IP adapter did not start");
        return;
    }

    /* 10 Hz application loop, with a status line every 50 passes (5 s). The adapter task sends
     * T->O at the RPI from the last data written, so this cadence only sets how fresh it is.
     * Assembly 150 reads all zero with no connection or an idle PLC: make that your safe state. */
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
            ESP_LOGI(TAG, "listening %u io %u run %u rx %lu tx %lu timeouts %lu", view.listening, view.io_open,
                     view.run, (unsigned long)view.rx_packets, (unsigned long)view.tx_packets,
                     (unsigned long)view.timeouts);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
