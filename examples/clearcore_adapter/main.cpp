/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP on a Teknic ClearCore: an EtherNet/IP adapter in poll mode.
 *
 * Add to a ClearCore (Microchip Studio) project, see docs/INTEGRATION.md:
 *   QuickEIP/src/ (every .c file)
 *   QuickEIP/ports/lwip_raw/eip_hal_lwip_raw.c
 *   QuickEIP/ports/clearcore/eip_hal_clearcore.cpp
 *   include paths: QuickEIP/include, QuickEIP/src
 *
 * Assembly 150 (from the PLC): byte 0 bit 0 drives IO-0.
 * Assembly 100 (to the PLC):   byte 0 bit 0 is DI-6, bytes 4..7 a loop counter.
 *
 * To adapt it: set the identity to your own vendor id and product, make kAssemblies match your
 * EDS, and replace the I/O block in the loop. The ClearCore lwIP build is small, so build with
 * EIP_ADAPTER_MAX_CLIENTS set to 2 or 3 (docs/INTEGRATION.md, ClearCore section).
 */
#include "ClearCore.h"

#include <string.h>

#include "eip.h"

/* Instances and sizes must match the EDS: the adapter refuses a Forward_Open that names others
 * (table in docs/INTEGRATION.md 3.3). The size-0 config point accepts a connection path with a
 * config instance and no data. */
static const EipAssemblyConfig kAssemblies[] = {
    /* instance, direction,        size, name,      initial, implicit_only */
    {150, EIP_ASM_CONSUMED, 8, "Outputs", 0, 0},
    {100, EIP_ASM_PRODUCED, 8, "Inputs", 0, 0},
    {151, EIP_ASM_CONFIG, 0, "Config", 0, 0},
};

int main(void)
{
    EipAdapterConfig config;
    uint32_t loops = 0;
    uint32_t last_io = 0;

    ConnectorIO0.Mode(Connector::OUTPUT_DIGITAL);

    while (!EthernetMgr.PhyLinkActive()) {   /* wait for the cable */
        Delay_ms(100);
    }
    EthernetMgr.Setup();
    if (!EthernetMgr.DhcpBegin()) {
        EthernetMgr.LocalIp(IpAddress(192, 168, 1, 50)); /* fallback static address */
    }

    /* serial_number stays 0, so the adapter derives it from the MAC. */
    memset(&config, 0, sizeof config);
    config.vendor_id = 65000;            /* replace with your ODVA vendor id */
    config.device_type = 43;             /* generic device */
    config.product_code = 1;
    config.revision_major = 1;
    config.revision_minor = 1;
    config.product_name = "QuickEIP ClearCore";
    config.assemblies = kAssemblies;
    config.assembly_count = sizeof kAssemblies / sizeof kAssemblies[0];
    /* Poll mode on ClearCore: no task. A failed start here is a configuration error, such as
     * assemblies over EIP_ADAPTER_DATA_POOL. Stop rather than run outputs without an adapter. */
    if (eip_adapter_start(&config) != EIP_OK) {
        while (true) {
            EthernetMgr.Refresh();
        }
    }

    /* T->O packets go out from inside eip_adapter_poll(), so loop time becomes RPI jitter. Keep
     * each pass well under your fastest RPI. */
    while (true) {
        eip_adapter_poll();              /* also runs EthernetMgr.Refresh() as needed */
        EthernetMgr.Refresh();

        /* Application I/O every 5 ms. */
        if (Milliseconds() - last_io >= 5) {
            uint8_t outputs[8] = {0};    /* the safe state, if the read below ever fails */
            uint8_t inputs[8];
            uint16_t len = sizeof outputs;

            last_io = Milliseconds();
            eip_adapter_read(150, outputs, &len);
            /* Outputs drop to zero when the connection closes or goes idle. */
            ConnectorIO0.State((outputs[0] & 0x01) != 0);

            memset(inputs, 0, sizeof inputs);
            inputs[0] = ConnectorDI6.State() ? 1 : 0;
            eip_store_le32(inputs + 4, loops);
            eip_adapter_write(100, inputs, sizeof inputs);
        }
        loops++;
    }
}
