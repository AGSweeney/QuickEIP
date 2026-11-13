/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP hardware test firmware for NetBurner (MODRT1171).
 * No console is needed: the callback counts go out in produced assembly 102, which the PC test
 * reads with Get_Attribute_Single. Stored settings live in the user parameter area. */
#include <predef.h>

#include "eip.h"

#include <hal.h>
#include <init.h>
#include <nbrtos.h>
#include <stdio.h>
#include <string.h>
#include <system.h>

const char *AppName = "QuickEIP HW Test";

static const EipAssemblyConfig kAssemblies[] = {
    {150, EIP_ASM_CONSUMED, 16, "Outputs", 0, 0},
    {100, EIP_ASM_PRODUCED, 16, "Inputs", 0, 0},
    {102, EIP_ASM_PRODUCED, 16, "Status", 0, 0},
    {151, EIP_ASM_CONFIG, 0, "Config", 0, 0},
};

struct StoredBlock {
    uint32_t magic;
    EipNvSettings nv;
};
static const uint32_t kMagic = 0x51454950u; /* "QEIP" */

/* opened, closed, timeout, run, idle, module led, network led, led calls, store calls,
 * last stored timeout (2 bytes) */
static volatile uint8_t g_status[16];

static void OnEvent(void *ctx, uint8_t event)
{
    (void)ctx;
    if (event >= 1 && event <= 5) {
        g_status[event - 1]++;
    }
    iprintf("EVENT %d\r\n", event);
}

static void OnLeds(void *ctx, uint8_t module, uint8_t network)
{
    (void)ctx;
    g_status[5] = module;
    g_status[6] = network;
    g_status[7]++;
    iprintf("LEDS module %d network %d\r\n", module, network);
}

static void StoreNv(void *ctx, const EipNvSettings *nv)
{
    StoredBlock block;

    (void)ctx;
    block.magic = kMagic;
    block.nv = *nv;
    SaveUserParameters(&block, sizeof block);
    g_status[8]++;
    g_status[9] = (uint8_t)(nv->encap_timeout_s & 0xFFu);
    g_status[10] = (uint8_t)(nv->encap_timeout_s >> 8);
    iprintf("STORE_NV timeout %d\r\n", nv->encap_timeout_s);
}

static void Reset(void *ctx, uint8_t type)
{
    (void)ctx;
    iprintf("RESET type %d\r\n", type);
    if (type == 1) {
        StoredBlock block;
        memset(&block, 0, sizeof block);
        SaveUserParameters(&block, sizeof block);
    }
    OSTimeDly(2);
    ForceReboot();
}

void UserMain(void *pd)
{
    static EipNvSettings nv;
    EipAdapterConfig config;
    const StoredBlock *stored;
    uint32_t counter = 0;
    int have_nv = 0;

    (void)pd;
    init();
    WaitForActiveNetwork(TICKS_PER_SECOND * 10);

    stored = (const StoredBlock *)GetUserParameters();
    if (stored != 0 && stored->magic == kMagic) {
        nv = stored->nv;
        have_nv = 1;
    }
    iprintf("stored settings %s, tick %d Hz\r\n", have_nv ? "found" : "not found", TICKS_PER_SECOND);

    memset(&config, 0, sizeof config);
    config.vendor_id = 65000;
    config.device_type = 43;
    config.product_code = 1171;
    config.revision_major = 1;
    config.revision_minor = 1;
    config.product_name = "QuickEIP RT1171 Test";
    config.assemblies = kAssemblies;
    config.assembly_count = sizeof kAssemblies / sizeof kAssemblies[0];
    config.reset = Reset;
    config.on_event = OnEvent;
    config.on_leds = OnLeds;
    config.nv = have_nv ? &nv : 0;
    config.store_nv = StoreNv;
    if (eip_adapter_start(&config) != EIP_OK) {
        iprintf("EIP adapter did not start\r\n");
    }

    /* 10 Hz, the same as the ESP32 test firmware, so the PC test checks the same counter rate. */
    while (1) {
        uint8_t inputs[16];
        uint8_t outputs[16] = {0};
        uint8_t status[16];
        uint16_t len = sizeof outputs;
        int i;

        eip_adapter_read(150, outputs, &len);
        memset(inputs, 0, sizeof inputs);
        eip_store_le32(inputs, counter++);
        inputs[4] = outputs[0];
        eip_adapter_write(100, inputs, sizeof inputs);
        for (i = 0; i < 16; i++) {
            status[i] = g_status[i];
        }
        eip_adapter_write(102, status, sizeof status);
        OSTimeDly(TICKS_PER_SECOND / 10);
    }
}
