/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* eiptool: QuickEIP from a Linux/macOS/WSL shell.
 *
 *   eiptool scan
 *   eiptool identity <ip>
 *   eiptool tags     <ip> [slot]
 *   eiptool read     <ip> <tag> [slot]
 *   eiptool write    <ip> <tag> <dint|int|sint|real|bool> <value> [slot]
 *   eiptool get      <ip> <class> <instance> <attribute> [slot]
 *   eiptool adapter                 run the test adapter until Ctrl-C
 *
 * slot defaults to -1 (direct: CompactLogix, Micro800, any adapter).
 * class, instance, attribute and integer values accept decimal or 0x hex.
 * read and write handle one element. Exit status: 0 done, 1 failed, 2 usage.
 *
 * EIP_NETIF=<name> in the environment puts that interface first: the one the adapter reports and
 * the first one scan uses.
 *
 * Build: cmake -S . -B build && cmake --build build, which gives build/eiptool.
 */
#define _DEFAULT_SOURCE 1
#include "eip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define sleep(seconds) Sleep((seconds) * 1000u)
#else
#include <unistd.h>
#endif

static EipSession g_session;

static int Usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  eiptool scan\n"
            "  eiptool identity <ip>\n"
            "  eiptool tags <ip> [slot]\n"
            "  eiptool read <ip> <tag> [slot]\n"
            "  eiptool write <ip> <tag> <dint|int|sint|real|bool> <value> [slot]\n"
            "  eiptool get <ip> <class> <instance> <attribute> [slot]\n"
            "  eiptool adapter\n");
    return 2;
}

/* Print the error and return exit status 1. The CIP status survives eip_session_close(), so
 * callers close the session first and report after. */
static int Fail(const char *what, int rc)
{
    fprintf(stderr, "%s: %s", what, eip_status_text(rc));
    if (rc == EIP_ERR_CIP) {
        fprintf(stderr, " (general 0x%02X, extended 0x%04X)", eip_last_cip_status(&g_session),
                eip_last_cip_extended(&g_session));
    }
    fprintf(stderr, "\n");
    return 1;
}

/* Returns 0 or the exit status. vendor_id stays 0: only a Class 1 open uses it. */
static int Open(const char *ip, int slot)
{
    EipOpenConfig config;
    int rc;

    memset(&config, 0, sizeof config);
    config.ip = ip;
    config.slot = (int16_t)slot;
    config.timeout_ms = 3000;
    rc = eip_session_open(&g_session, &config);
    return (rc == EIP_OK) ? 0 : Fail("open", rc);
}

/* Print tag data element by element, by CIP type. LINT, ULINT, LREAL and any other type come out
 * as hex bytes in wire order. Unknown types print one byte per element. */
static void PrintValue(uint16_t type, const uint8_t *data, uint16_t len)
{
    uint16_t size = 1;
    uint16_t i;

    switch (type) {
    case EIP_INT: case EIP_UINT: size = 2; break;
    case EIP_DINT: case EIP_UDINT: case EIP_REAL: size = 4; break;
    case EIP_LINT: case EIP_ULINT: case EIP_LREAL: size = 8; break;
    default: size = 1; break;
    }
    for (i = 0; i + size <= len; i = (uint16_t)(i + size)) {
        const uint8_t *p = data + i;
        if (i != 0) {
            printf(" ");
        }
        switch (type) {
        case EIP_BOOL: printf("%u", p[0] ? 1u : 0u); break;
        case EIP_SINT: printf("%d", (int8_t)p[0]); break;
        case EIP_USINT: printf("%u", p[0]); break;
        case EIP_INT: printf("%d", (int16_t)eip_load_le16(p)); break;
        case EIP_UINT: printf("%u", eip_load_le16(p)); break;
        case EIP_DINT: printf("%ld", (long)(int32_t)eip_load_le32(p)); break;
        case EIP_UDINT: printf("%lu", (unsigned long)eip_load_le32(p)); break;
        case EIP_REAL: {
            uint32_t bits = eip_load_le32(p);
            float f;
            memcpy(&f, &bits, 4);
            printf("%g", (double)f);
            break;
        }
        default: {
            uint16_t k;
            for (k = 0; k < size; k++) {
                printf("%02X", p[k]);
            }
            break;
        }
        }
    }
    printf("  (type 0x%04X, %u bytes)\n", type, len);
}

/* The optional slot argument at argv[index], -1 when absent. A mistyped slot must stop the tool:
 * as 0 it would quietly route the request to the wrong module. */
static int SlotArg(int argc, char **argv, int index)
{
    char *end = 0;
    long slot;

    if (argc <= index) {
        return -1;
    }
    slot = strtol(argv[index], &end, 0);
    if (end == argv[index] || *end != '\0' || slot < -1 || slot > 255) {
        fprintf(stderr, "bad slot: %s\n", argv[index]);
        exit(2);
    }
    return (int)slot;
}

int main(int argc, char **argv)
{
    int rc;

    if (argc < 2) {
        return Usage();
    }
    if (strcmp(argv[1], "scan") == 0) {
        EipDevice devices[64];
        uint16_t count = 64;
        uint16_t i;
        rc = eip_scan_devices(devices, &count, 0);
        if (rc != EIP_OK) {
            return Fail("scan", rc);
        }
        for (i = 0; i < count; i++) {
            printf("%u.%u.%u.%u  vendor %u  type %u  product %u  rev %u.%u  serial %08lX  %s\n",
                   devices[i].ip[0], devices[i].ip[1], devices[i].ip[2], devices[i].ip[3], devices[i].vendor_id,
                   devices[i].device_type, devices[i].product_code, devices[i].revision_major,
                   devices[i].revision_minor, (unsigned long)devices[i].serial_number, devices[i].name);
        }
        printf("%u device(s)\n", count);
        return 0;
    }
    if (strcmp(argv[1], "identity") == 0 && argc >= 3) {
        EipIdentity id;
        rc = eip_list_identity(argv[2], &id, 0);
        if (rc != EIP_OK) {
            return Fail("identity", rc);
        }
        printf("vendor %u  type %u  product %u  rev %u.%u  status 0x%04X  serial %08lX  state %u  %s\n",
               id.vendor_id, id.device_type, id.product_code, id.revision_major, id.revision_minor, id.status,
               (unsigned long)id.serial_number, id.state, id.name);
        return 0;
    }
    if (strcmp(argv[1], "tags") == 0 && argc >= 3) {
        static EipSymbol symbols[1024];
        uint16_t count = 1024;
        uint16_t i;
        if (Open(argv[2], SlotArg(argc, argv, 3)) != 0) {
            return 1;
        }
        /* After a failure on a later page, count still holds what was read: print, then report. */
        rc = eip_list_symbols(&g_session, symbols, &count);
        for (i = 0; i < count; i++) {
            printf("%-40s type 0x%04X  size %u%s\n", symbols[i].name, symbols[i].symbol_type,
                   symbols[i].element_size, symbols[i].is_array ? "  array" : "");
        }
        eip_session_close(&g_session);
        return (rc == EIP_OK) ? 0 : Fail("tags", rc);
    }
    if (strcmp(argv[1], "read") == 0 && argc >= 4) {
        uint8_t data[512];
        uint16_t len = sizeof data;
        uint16_t type = 0;
        if (Open(argv[2], SlotArg(argc, argv, 4)) != 0) {
            return 1;
        }
        rc = eip_tag_read(&g_session, argv[3], 1, data, &len, &type, 0);
        if (rc == EIP_OK) {
            PrintValue(type, data, len);
        }
        eip_session_close(&g_session);
        return (rc == EIP_OK) ? 0 : Fail("read", rc);
    }
    if (strcmp(argv[1], "write") == 0 && argc >= 6) {
        uint8_t data[8];
        uint16_t len;
        uint16_t type;
        const char *t = argv[4];
        /* Encode the value for the named type. bool sends 0xFF for true. */
        if (strcmp(t, "dint") == 0) {
            type = EIP_DINT; len = 4; eip_store_le32(data, (uint32_t)strtol(argv[5], 0, 0));
        } else if (strcmp(t, "int") == 0) {
            type = EIP_INT; len = 2; eip_store_le16(data, (uint16_t)strtol(argv[5], 0, 0));
        } else if (strcmp(t, "sint") == 0) {
            type = EIP_SINT; len = 1; data[0] = (uint8_t)strtol(argv[5], 0, 0);
        } else if (strcmp(t, "bool") == 0) {
            type = EIP_BOOL; len = 1; data[0] = (uint8_t)(strtol(argv[5], 0, 0) ? 0xFF : 0);
        } else if (strcmp(t, "real") == 0) {
            float f = strtof(argv[5], 0);
            uint32_t bits;
            memcpy(&bits, &f, 4);
            type = EIP_REAL; len = 4; eip_store_le32(data, bits);
        } else {
            return Usage();
        }
        if (Open(argv[2], SlotArg(argc, argv, 6)) != 0) {
            return 1;
        }
        rc = eip_tag_write(&g_session, argv[3], type, 1, data, len, 0);
        eip_session_close(&g_session);
        if (rc == EIP_OK) {
            printf("ok\n");
        }
        return (rc == EIP_OK) ? 0 : Fail("write", rc);
    }
    if (strcmp(argv[1], "get") == 0 && argc >= 6) {
        uint8_t data[512];
        uint16_t len = sizeof data;
        uint16_t i;
        if (Open(argv[2], SlotArg(argc, argv, 6)) != 0) {
            return 1;
        }
        rc = eip_get_attribute(&g_session, (uint16_t)strtol(argv[3], 0, 0), (uint16_t)strtol(argv[4], 0, 0),
                               (uint16_t)strtol(argv[5], 0, 0), data, &len, 0);
        if (rc == EIP_OK) {
            for (i = 0; i < len; i++) {
                printf("%02X%s", data[i], (i + 1u) % 16u == 0 ? "\n" : " ");
            }
            printf("\n(%u bytes)\n", len);
        }
        eip_session_close(&g_session);
        return (rc == EIP_OK) ? 0 : Fail("get", rc);
    }
    if (strcmp(argv[1], "adapter") == 0) {
        EipAdapterView view;
        /* Config 0: the built-in test identity and assemblies. The port runs the adapter in its own
         * thread, so this loop only prints status every 2 seconds until the process is killed. */
        rc = eip_adapter_start(0);
        if (rc != EIP_OK) {
            return Fail("adapter", rc);
        }
        printf("test adapter running: 100 consumed (8 bytes), 101 produced (8), 102 produced (4), 150 config (4). Ctrl-C to stop.\n");
        for (;;) {
            sleep(2);
            if (eip_adapter_snapshot(&view) == EIP_OK) {
                printf("listening %u  io %u run %u  rx %lu tx %lu  opens %lu timeouts %lu  clients %u\n",
                       view.listening, view.io_open, view.run, (unsigned long)view.rx_packets,
                       (unsigned long)view.tx_packets, (unsigned long)view.opens, (unsigned long)view.timeouts,
                       view.tcp_clients);
            }
        }
    }
    return Usage();
}
