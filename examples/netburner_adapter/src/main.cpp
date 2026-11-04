/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP on NetBurner: an adapter with two assemblies, plus a scanner
 * that reads a Logix tag once a second when PLC_IP is set.
 *
 * Build: see ../makefile and docs/INTEGRATION.md (NetBurner section).
 *
 * To adapt it: set the identity in UserMain to your own vendor id and product, make kAssemblies
 * match your EDS, then replace the counter and echo in the loop with your application's data.
 * The default 20 Hz tick suits RPIs of about 50 ms and up (docs/INTEGRATION.md). */
#include <predef.h>

#include "eip.h"

#include <init.h>
#include <nbrtos.h>
#include <stdio.h>
#include <string.h>
#include <system.h>

const char *AppName = "QuickEIP Adapter";

/* Leave empty to skip the scanner demo. */
static const char *PLC_IP = "";

/* Instances and sizes must match the EDS: the adapter refuses a Forward_Open that names others
 * (table in docs/INTEGRATION.md 3.3). 150 is O->T, 100 is T->O. The size-0 config point accepts a
 * connection path with a config instance and no data. */
static const EipAssemblyConfig kAssemblies[] = {
    /* instance, direction,        size, name,      initial, implicit_only */
    {150, EIP_ASM_CONSUMED, 32, "Outputs", 0, 0},
    {100, EIP_ASM_PRODUCED, 32, "Inputs", 0, 0},
    {151, EIP_ASM_CONFIG, 0, "Config", 0, 0},
};

static EipSession g_session; /* about 5 KB: keep it static */

void UserMain(void *pd)
{
    EipAdapterConfig config;
    uint32_t counter = 0;

    (void)pd;
    init();
    /* Gives up after 10 s. That is safe: the adapter retries its sockets once a second. */
    WaitForActiveNetwork(TICKS_PER_SECOND * 10);

    /* serial_number stays 0, so the adapter derives it from the MAC. */
    memset(&config, 0, sizeof config);
    config.vendor_id = 65000;            /* replace with your ODVA vendor id */
    config.device_type = 43;             /* generic device */
    config.product_code = 1;
    config.revision_major = 1;
    config.revision_minor = 1;
    config.product_name = "QuickEIP NetBurner";
    config.assemblies = kAssemblies;
    config.assembly_count = sizeof kAssemblies / sizeof kAssemblies[0];
    config.timeout_floor_ms = 0;         /* never below 2 ticks anyway */

    /* Starts the adapter task: this file has no polling to do. */
    if (eip_adapter_start(&config) != EIP_OK) {
        iprintf("EIP adapter did not start\r\n");
    }

    /* Once a second for the demo. The adapter task sends T->O at the RPI from the last data
     * written, so this cadence only sets how fresh that data is. Run it faster for real I/O. */
    while (1) {
        uint8_t inputs[32];
        uint8_t outputs[32] = {0};       /* stays zero if the read below fails */
        uint16_t len = sizeof outputs;

        /* Application I/O: read what the scanner sent, publish our inputs. */
        eip_adapter_read(150, outputs, &len);
        memset(inputs, 0, sizeof inputs);
        eip_store_le32(inputs, counter++);
        inputs[4] = outputs[0];          /* echo the first output byte */
        eip_adapter_write(100, inputs, sizeof inputs);

        /* Scanner calls block: with timeout_ms 0 (5 s) an unreachable PLC stalls the I/O above on
         * every pass. In a real application give the scanner a task of its own. */
        if (PLC_IP[0] != '\0') {
            EipOpenConfig open;
            memset(&open, 0, sizeof open);
            open.ip = PLC_IP;
            open.slot = 0;               /* ControlLogix slot 0; -1 for CompactLogix/Micro800 */
            if (!eip_session_is_open(&g_session)) {
                eip_session_open(&g_session, &open);
            }
            if (eip_session_is_open(&g_session)) {
                uint8_t data[4];
                uint16_t data_len = sizeof data;
                uint16_t type = 0;
                int rc = eip_tag_read(&g_session, "MyDint", 1, data, &data_len, &type, 0);
                if (rc == EIP_OK) {
                    iprintf("MyDint = %ld\r\n", (long)(int32_t)eip_load_le32(data));
                } else {
                    iprintf("tag read: %s\r\n", eip_status_text(rc));
                    eip_session_close(&g_session); /* the next pass reconnects */
                }
            }
        }
        OSTimeDly(TICKS_PER_SECOND);
    }
}
