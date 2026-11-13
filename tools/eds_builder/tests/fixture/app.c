/* Test input for the scanner. It is never compiled. Expected result:
 *   identity  vendor 1234, type 12, code 7, revision 2.5, "Fixture Drive"
 *   150 consumed 8, 101 produced 12, 768 produced "sizeof(struct Status)" (unresolved),
 *   238 consumed 0 (heartbeat), 151 config 4 */
#include "app_config.h"
#include "eip.h"

struct Status {
    unsigned int flags;
    unsigned short code;
};

// A commented-out entry must not be picked up: {999, EIP_ASM_PRODUCED, 4, "Ghost", 0, 0},
static const EipAssemblyConfig kAssemblies[] = {
    {ASM_OUTPUTS, EIP_ASM_CONSUMED, 8, "Outputs", 0, 0},
    {.instance = ASM_INPUTS, .direction = EIP_ASM_PRODUCED, .size = INPUT_BYTES, .name = "Inputs"},
    {kWideInstance, EIP_ASM_PRODUCED, sizeof(struct Status), "Status", 0, 1},
    {ASM_HEARTBEAT, EIP_ASM_CONSUMED, 0, "Heartbeat", 0, 0},
    {ASM_CONFIG, EIP_ASM_CONFIG, (uint16_t)(2 << 1), "Config", 0, 0}, /* 4 */
};

void start(void)
{
    EipAdapterConfig cfg = {
        .vendor_id = APP_VENDOR_ID,
        .device_type = 12,
        .product_code = 7,
        .revision_major = 2,
        .revision_minor = 5,
        .product_name = APP_PRODUCT_NAME,
        .assemblies = kAssemblies,
        .assembly_count = sizeof kAssemblies / sizeof kAssemblies[0],
    };
    if (cfg.vendor_id == 0) {
        return;
    }
    eip_adapter_start(&cfg);
}
