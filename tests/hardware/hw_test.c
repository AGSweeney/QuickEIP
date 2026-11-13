/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Hardware test of a QuickEIP adapter, from a PC, with the QuickEIP scanner as the originator.
 * The device runs one of the test firmwares in this folder. See README.md.
 *
 *   hw_test <ip> <stage> <qos> [rpi_ms]
 *
 *   stage w   warm-up: one Class 1 connection in idle for 5 s, no checks
 *   stage a   a device with default settings: identity, objects, Class 1, the watchdog, new stored
 *             settings, the encapsulation inactivity timeout, then Identity Reset type 0
 *   stage b   after that restart: the settings are still there, Class 1 again, then Identity Reset
 *             type 1 (factory defaults)
 *   stage c   after that restart: the defaults are back
 *   qos       1 when the port marks packets and so has the QoS object, else 0
 *   rpi_ms    10 by default; 50 for a NetBurner with the 20 Hz tick
 *
 * The exit code is 0 when every check of the stage passed. */
#define _DEFAULT_SOURCE 1
#include "eip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#define Sleep(ms) usleep((ms) * 1000u)
#endif

static int g_failed;
static int g_checks;

#define EXPECT(cond, name)                                               \
    do {                                                                 \
        g_checks++;                                                      \
        if (cond) {                                                      \
            printf("  ok    %s\n", name);                                \
        } else {                                                         \
            printf("  FAIL  %s (line %d)\n", name, __LINE__);            \
            g_failed++;                                                  \
        }                                                                \
    } while (0)

static const char *g_ip;
static EipSession g_session;
static unsigned g_rpi_ms = 10;

/* Firmware that has no console publishes its callback counts in produced assembly 102:
 * opened, closed, timeout, run, idle, module led, network led, led calls, store calls. */
static int Counters(uint8_t c[16])
{
    uint16_t len = 16;
    uint8_t status = 0;
    memset(c, 0, 16);
    return eip_get_attribute(&g_session, 0x04, 102, 3, c, &len, &status);
}

static int Open(void)
{
    EipOpenConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.ip = g_ip;
    cfg.slot = -1;
    cfg.vendor_id = 4321;
    cfg.serial_number = 0x2222;
    cfg.timeout_ms = 2000;
    return eip_session_open(&g_session, &cfg);
}

static int GetU8(uint16_t cls, uint16_t attr, uint8_t *value, uint8_t *status)
{
    uint8_t buf[8];
    uint16_t len = sizeof buf;
    int rc = eip_get_attribute(&g_session, cls, 1, attr, buf, &len, status);
    if (rc == EIP_OK && len == 1) {
        *value = buf[0];
        return EIP_OK;
    }
    return rc == EIP_OK ? EIP_ERR_PROTOCOL : rc;
}

static int GetTimeout(uint16_t *seconds)
{
    uint8_t buf[8];
    uint16_t len = sizeof buf;
    uint8_t status = 0;
    int rc = eip_get_attribute(&g_session, 0xF5, 1, 13, buf, &len, &status);
    if (rc == EIP_OK && len == 2) {
        *seconds = eip_load_le16(buf);
        return EIP_OK;
    }
    return rc == EIP_OK ? EIP_ERR_PROTOCOL : rc;
}

static void CheckDscp(int qos, const uint8_t want[5], const char *name)
{
    uint8_t status = 0;
    uint8_t v = 0;
    int i;
    int same = 1;

    if (!qos) {
        int rc = GetU8(0x48, 4, &v, &status);
        EXPECT(rc == EIP_ERR_CIP && status == 0x16, "no QoS object on this port");
        return;
    }
    for (i = 0; i < 5; i++) {
        if (GetU8(0x48, (uint16_t)(4 + i), &v, &status) != EIP_OK || v != want[i]) {
            printf("        QoS attr %d = %u, want %u\n", 4 + i, v, want[i]);
            same = 0;
        }
    }
    EXPECT(same, name);
}

/* Open, run, check data both ways, idle, close. Then open again and go silent. */
static void ClassOne(void)
{
    EipIo io;
    EipIoConfig cfg;
    uint8_t out[16];
    uint8_t in[16];
    uint16_t in_len;
    uint32_t first = 0;
    uint32_t last = 0;
    int got = 0;
    int echoed = 0;
    int i;
    int rc;
    int steps = (int)(3000u / g_rpi_ms);
    uint8_t before[16];
    uint8_t after[16];
    int have_counters = (Counters(before) == EIP_OK);

    memset(&cfg, 0, sizeof cfg);
    cfg.config_instance = 151;
    cfg.output_instance = 150;
    cfg.input_instance = 100;
    cfg.output_bytes = 16;
    cfg.input_bytes = 16;
    cfg.o2t_rpi_us = g_rpi_ms * 1000u;
    cfg.t2o_rpi_us = g_rpi_ms * 1000u;
    cfg.run_idle = 1;
    cfg.timeout_multiplier = 2; /* RPI x 16 */
    rc = eip_io_open(&g_session, &io, &cfg);
    EXPECT(rc == EIP_OK, "Class 1 open");
    if (rc != EIP_OK) {
        printf("        rc %d cip 0x%02X ext 0x%04X\n", rc, eip_last_cip_status(&g_session), g_session.cip_extended);
        return;
    }
    memset(out, 0, sizeof out);
    out[0] = 0xC3;
    /* 3 s of run. The firmware copies outputs[0] to inputs[4] and counts in inputs[0..3]
     * at 10 Hz. */
    for (i = 0; i < steps; i++) {
        (void)eip_io_produce(&io, out, sizeof out, 1);
        in_len = sizeof in;
        if (eip_io_consume(&io, in, &in_len, g_rpi_ms) == EIP_OK && in_len == 16) {
            last = eip_load_le32(in);
            if (got == 0) {
                first = last;
            }
            if (in[4] == 0xC3) {
                echoed = 1;
            }
            got++;
        }
    }
    printf("        T->O packets %d, counter %lu -> %lu\n", got, (unsigned long)first, (unsigned long)last);
    printf("        expected about %d\n", steps);
    EXPECT(got >= steps / 2 && got <= steps + 5, "T->O packets arrive at about the RPI");
    EXPECT(last > first && (last - first) >= 15 && (last - first) <= 40, "produced counter advances at 10 Hz");
    EXPECT(echoed, "consumed byte comes back in the produced assembly");
    /* 1 s of idle: the firmware reads zeros, so the echo goes to 0. */
    echoed = 1;
    for (i = 0; i < steps / 3; i++) {
        (void)eip_io_produce(&io, out, sizeof out, 0);
        in_len = sizeof in;
        if (eip_io_consume(&io, in, &in_len, g_rpi_ms) == EIP_OK && in_len == 16 && i > steps / 4) {
            echoed = (in[4] != 0);
        }
    }
    EXPECT(!echoed, "idle: the consumed assembly reads as zero");
    EXPECT(eip_io_close(&g_session, &io) == EIP_OK, "Class 1 close");
    Sleep(300);

    rc = eip_io_open(&g_session, &io, &cfg);
    EXPECT(rc == EIP_OK, "Class 1 open again");
    if (rc == EIP_OK) {
        for (i = 0; i < 20; i++) {
            (void)eip_io_produce(&io, out, sizeof out, 1);
            Sleep(g_rpi_ms);
        }
        Sleep(g_rpi_ms * 32u + 700u); /* silent for twice the watchdog time and more */
        rc = eip_io_close(&g_session, &io);
        EXPECT(rc != EIP_OK, "after silence the connection is gone (Forward_Close refused)");
    }
    if (have_counters && Counters(after) == EIP_OK) {
        printf("        callbacks: opened +%d closed +%d timeout +%d run +%d idle +%d, leds module %u network %u\n",
               (uint8_t)(after[0] - before[0]), (uint8_t)(after[1] - before[1]), (uint8_t)(after[2] - before[2]),
               (uint8_t)(after[3] - before[3]), (uint8_t)(after[4] - before[4]), after[5], after[6]);
        EXPECT((uint8_t)(after[0] - before[0]) == 2 && (uint8_t)(after[1] - before[1]) == 1 &&
                   (uint8_t)(after[2] - before[2]) == 1,
               "on_event: two opens, one close, one timeout");
        EXPECT((uint8_t)(after[3] - before[3]) == 2 && (uint8_t)(after[4] - before[4]) == 1,
               "on_event: two run, one idle");
        EXPECT(after[5] == EIP_LED_GREEN && after[6] == EIP_LED_RED_FLASH,
               "on_leds: module green, network flashing red after the timeout");
    }
}

static int Reset(uint8_t type)
{
    static const uint8_t path[] = {0x20, 0x01, 0x24, 0x01};
    uint8_t rsp[8];
    uint16_t rsp_len = sizeof rsp;
    uint8_t status = 0;
    uint16_t ext = 0;
    return eip_cip(&g_session, 0x05, path, sizeof path, &type, 1, rsp, &rsp_len, &status, &ext);
}

int main(int argc, char **argv)
{
    static const uint8_t defaults[5] = {55, 47, 43, 31, 27};
    static const uint8_t changed[5] = {46, 40, 34, 26, 10};
    EipIdentity identity;
    uint8_t buf[64];
    uint16_t len;
    uint8_t status = 0;
    uint16_t seconds = 0;
    char stage;
    int qos;
    int rc;
    int i;

    if (argc >= 5) {
        g_rpi_ms = (unsigned)atoi(argv[4]);
    }
    if (argc < 4) {
        printf("usage: hw_test <ip> w|a|b|c <qos 0|1> [rpi_ms]\n");
        return 2;
    }
    g_ip = argv[1];
    stage = argv[2][0];
    qos = atoi(argv[3]);
    printf("stage %c against %s\n", stage, g_ip);

    rc = eip_list_identity(g_ip, &identity, 2000);
    EXPECT(rc == EIP_OK, "ListIdentity over UDP");
    if (rc == EIP_OK) {
        printf("        \"%s\" vendor %u type %u code %u rev %u.%u serial %08lX status 0x%04X\n", identity.name,
               identity.vendor_id, identity.device_type, identity.product_code, identity.revision_major,
               identity.revision_minor, (unsigned long)identity.serial_number, identity.status);
    }
    rc = Open();
    EXPECT(rc == EIP_OK, "session open");
    if (rc != EIP_OK) {
        return 1;
    }
    len = sizeof buf;
    rc = eip_get_attribute(&g_session, 0xF5, 0, 1, buf, &len, &status);
    EXPECT(rc == EIP_OK && len == 2 && eip_load_le16(buf) == 4, "TCP/IP Interface class revision 4");
    len = sizeof buf;
    rc = eip_get_attribute(&g_session, 0xF6, 1, 1, buf, &len, &status);
    if (rc == EIP_OK && len == 4) {
        printf("        Ethernet Link speed %lu Mbit/s\n", (unsigned long)eip_load_le32(buf));
    }

    if (stage == 'w') {
        /* Warm-up, no checks: security software on the PC can drop the first seconds of UDP of
         * a program it has not seen before, in both directions. */
        EipIo io;
        EipIoConfig cfg;
        uint8_t out[16] = {0};
        memset(&cfg, 0, sizeof cfg);
        cfg.config_instance = 151;
        cfg.output_instance = 150;
        cfg.input_instance = 100;
        cfg.output_bytes = 16;
        cfg.input_bytes = 16;
        cfg.o2t_rpi_us = g_rpi_ms * 1000u;
        cfg.t2o_rpi_us = g_rpi_ms * 1000u;
        cfg.run_idle = 1;
        cfg.timeout_multiplier = 7;
        if (eip_io_open(&g_session, &io, &cfg) == EIP_OK) {
            for (i = 0; i < (int)(5000u / g_rpi_ms); i++) {
                (void)eip_io_produce(&io, out, sizeof out, 0);
                Sleep(g_rpi_ms);
            }
            (void)eip_io_close(&g_session, &io);
        }
        eip_session_close(&g_session);
        return 0;
    }
    if (stage == 'a' || stage == 'c') {
        EXPECT(GetTimeout(&seconds) == EIP_OK && seconds == 120, "encapsulation inactivity timeout is 120 s");
        CheckDscp(qos, defaults, "QoS object reports the default DSCP values");
    }
    if (stage == 'a') {
        ClassOne();
        if (qos) {
            int ok = 1;
            for (i = 0; i < 5; i++) {
                if (eip_set_attribute(&g_session, 0x48, 1, (uint16_t)(4 + i), &changed[i], 1, &status) != EIP_OK) {
                    ok = 0;
                }
            }
            EXPECT(ok, "set the five DSCP values");
            CheckDscp(qos, changed, "QoS object reports the new values");
            buf[0] = 64;
            rc = eip_set_attribute(&g_session, 0x48, 1, 4, buf, 1, &status);
            EXPECT(rc == EIP_ERR_CIP && status == 0x09, "DSCP 64 refused");
        }
        eip_store_le16(buf, 3);
        rc = eip_set_attribute(&g_session, 0xF5, 1, 13, buf, 2, &status);
        EXPECT(rc == EIP_OK, "set the encapsulation inactivity timeout to 3 s");
        if (rc != EIP_OK) {
            printf("        rc %d status 0x%02X\n", rc, status);
        }
        Sleep(1500);
        EXPECT(GetTimeout(&seconds) == EIP_OK && seconds == 3, "session alive after 1.5 s, timeout reads 3");
        {
            uint8_t c[16];
            if (Counters(c) == EIP_OK) {
                printf("        store_nv calls %u, last stored timeout %u\n", c[8], eip_load_le16(c + 9));
                EXPECT(c[8] >= 1 && eip_load_le16(c + 9) == 3, "store_nv was called with the new timeout");
            }
        }
        Sleep(4500);
        EXPECT(GetTimeout(&seconds) != EIP_OK, "session closed by the device after 4.5 s of silence");
        eip_session_close(&g_session);
        EXPECT(Open() == EIP_OK, "new session");
        rc = Reset(0);
        EXPECT(rc == EIP_OK, "Identity Reset type 0 accepted");
    }
    if (stage == 'b') {
        EXPECT(GetTimeout(&seconds) == EIP_OK && seconds == 3, "timeout kept through the restart");
        CheckDscp(qos, changed, "DSCP values kept through the restart");
        /* Stay inside the 3 s timeout: Class 1 keeps the session alive as its originator. */
        ClassOne();
        rc = Reset(1);
        EXPECT(rc == EIP_OK, "Identity Reset type 1 (factory) accepted");
    }
    eip_session_close(&g_session);
    printf("stage %c: %d checks, %d failed\n", stage, g_checks, g_failed);
    return g_failed ? 1 : 0;
}
