/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* ESP-IDF port extras: what the application calls on top of eip.h. The port
 * itself is ports/esp-idf/eip_hal_esp_idf.c. */
#ifndef EIP_ESP_IDF_H_
#define EIP_ESP_IDF_H_

#include <stdint.h>

#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Choose which esp_netif handles the library sees, in order (index 0 is
 * the one the adapter reports). Call before eip_adapter_start(). Without
 * it, the port uses ETH_DEF, WIFI_STA_DEF, WIFI_AP_DEF, whichever exist.
 * The handles are copied into a list of at most four, and later entries are
 * ignored. The netifs stay yours and must outlive the adapter. A count of 0
 * goes back to the default lookup. */
void eip_esp_set_netif(esp_netif_t *const *netifs, uint8_t count);

#ifdef __cplusplus
}
#endif

#endif /* EIP_ESP_IDF_H_ */
