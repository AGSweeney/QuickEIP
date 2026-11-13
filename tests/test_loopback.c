/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* End-to-end test on one host (POSIX or Windows port): the built-in adapter runs in
 * its own thread, and the scanner API talks to it over this host's primary
 * interface address (not 127.0.0.1: the adapter advertises its interface
 * address for O->T, and only accepts O->T from the Forward_Open's source).
 *
 * Exercises the HAL (TCP connect/listen/accept, UDP, select), the adapter
 * server, the session, explicit messaging, ListIdentity, the broadcast
 * scan, and a Class 1 connection in both directions.
 *
 * Needs a non-loopback IPv4 interface that is up (EIP_NETIF=<name> picks one) and free ports TCP
 * and UDP 44818 and UDP 2222, so no other EtherNet/IP program can run on the host. Waits are fixed
 * sleeps: a badly overloaded host can fail the timing checks.
 *
 * Run: ctest --test-dir build -R loopback, or ./build/test_loopback. Prints a FAIL line with the
 * source line per failed check. A clean run prints "loopback tests passed". */
#define _DEFAULT_SOURCE 1
#include "eip.h"
#include "eip_hal.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define usleep(us) Sleep((us) / 1000u)
#else
#include <fcntl.h>
#include <unistd.h>
#endif

static int g_failed = 0;
static char g_host[16];

#define EXPECT(cond, name)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s (line %d)\n", name, __LINE__);         \
            g_failed++;                                            \
        }                                                          \
    } while (0)

/* Initial data of produced assembly 100, read back over explicit messaging. */
static const uint8_t kInitial[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/* The same shape as an application: consumed, produced and a size-0 config point. */
static const EipAssemblyConfig kAssemblies[] = {
    {150, EIP_ASM_CONSUMED, 32, "Outputs", 0, 0},
    {100, EIP_ASM_PRODUCED, 16, "Inputs", kInitial, 0},
    {151, EIP_ASM_CONFIG, 0, "Config", 0, 0},
};

/* allow_change / apply_interface / reset callbacks (run in the adapter thread) */
static volatile uint8_t g_refuse;          /* status allow_change returns */
static volatile int g_allow_calls;
static volatile uint8_t g_last_what;
static volatile int g_applied;
static char g_applied_host[65];
static volatile int g_reset_type = -1;

static uint8_t AllowChange(void *ctx, uint8_t what)
{
    (void)ctx;
    g_allow_calls++;
    g_last_what = what;
    return g_refuse;
}

static void ApplyInterface(void *ctx, const EipInterfaceConfig *cfg)
{
    (void)ctx;
    strncpy(g_applied_host, cfg->host_name, sizeof g_applied_host - 1);
    g_applied++;
}

static void ResetDevice(void *ctx, uint8_t type)
{
    (void)ctx;
    g_reset_type = type;
}

/* on_event / on_leds / store_nv callbacks (adapter thread too) */
static volatile uint8_t g_events[16];
static volatile int g_event_count;
static volatile uint8_t g_module_led = 0xFF;
static volatile uint8_t g_network_led = 0xFF;
static volatile int g_led_calls;
static volatile int g_stored;
static EipNvSettings g_stored_nv;

static void OnEvent(void *ctx, uint8_t event)
{
    (void)ctx;
    if (g_event_count < (int)sizeof g_events) {
        g_events[g_event_count] = event;
    }
    g_event_count++;
}

static void OnLeds(void *ctx, uint8_t module, uint8_t network)
{
    (void)ctx;
    g_module_led = module;
    g_network_led = network;
    g_led_calls++;
}

static void StoreNv(void *ctx, const EipNvSettings *nv)
{
    (void)ctx;
    g_stored_nv = *nv;
    g_stored++;
}

/* The events from index first on are exactly the n in want. */
static int EventsAre(int first, const uint8_t *want, int n)
{
    int i;

    if (g_event_count != first + n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (g_events[first + i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    EipAdapterConfig acfg;
    EipNvSettings nv;
    EipAdapterView view;
    EipIdentity identity;
    EipDevice devices[8];
    uint16_t device_count = 8;
    static EipSession session;
    EipOpenConfig ocfg;
    EipIo io;
    EipIoConfig iocfg;
    uint8_t buf[64];
    uint16_t len;
    uint8_t status = 0;
    int rc;
    int i;
    int found_self = 0;

    /* Closing never-opened (zeroed) objects must not close handle 0. */
    {
        static EipSession zeroed_session;
        static EipIo zeroed_io;
#ifdef _WIN32
        static const uint8_t self[4] = {127, 0, 0, 1};
        eip_sock_t first = eip_hal_udp_open(0, 0);   /* handles are table slots: this is 0 */
        EXPECT(first == 0, "first handle is 0");
#endif
        eip_session_close(&zeroed_session);
        (void)eip_io_close(&zeroed_session, &zeroed_io);
#ifdef _WIN32
        EXPECT(eip_hal_udp_sendto(first, self, 9, (const uint8_t *)"x", 1) == EIP_OK,
               "zeroed close leaves handle 0 alone");
        eip_hal_udp_close(first);
#else
        EXPECT(fcntl(0, F_GETFD) != -1, "zeroed close leaves fd 0 alone");
#endif
        EXPECT(!eip_session_is_open(&zeroed_session), "zeroed session is not open");
    }

    memset(&acfg, 0, sizeof acfg);
    acfg.vendor_id = 1234;
    acfg.device_type = 43;
    acfg.product_code = 7;
    acfg.revision_major = 2;
    acfg.revision_minor = 3;
    acfg.product_name = "QuickEIP Loopback";
    acfg.serial_number = 0x00C0FFEE;
    acfg.assemblies = kAssemblies;
    acfg.assembly_count = 3;
    acfg.apply_interface = ApplyInterface;
    acfg.reset = ResetDevice;
    acfg.allow_change = AllowChange;
    acfg.on_event = OnEvent;
    acfg.on_leds = OnLeds;
    acfg.store_nv = StoreNv;
    eip_nv_defaults(&nv);
    nv.dscp_low = 20;
    nv.dscp_high = 200; /* out of range: the default, 43, takes its place */
    acfg.nv = &nv;
    rc = eip_adapter_start(&acfg);
    EXPECT(rc == EIP_OK, "adapter start");
    EXPECT(eip_adapter_start(&acfg) == EIP_ERR_STATE, "second start refused");
    usleep(200000); /* let the adapter thread get going */
    EXPECT(eip_adapter_snapshot(&view) == EIP_OK && view.listening, "adapter listening");
    EXPECT(view.assembly_count == 3 && view.vendor_id == 1234, "adapter view identity");
    EXPECT(view.nv.encap_timeout_s == 120 && view.nv.dscp_low == 20 && view.nv.dscp_high == 43,
           "adapter view stored settings");
    EXPECT(g_led_calls == 1 && g_module_led == EIP_LED_GREEN && g_network_led == EIP_LED_GREEN_FLASH,
           "leds reported once after start: no connection");
    EXPECT(view.module_led == EIP_LED_GREEN && view.network_led == EIP_LED_GREEN_FLASH, "adapter view leds");
    EXPECT(g_event_count == 0, "no events before a connection");

    /* ListIdentity, unicast */
    {
        EipNetifInfo netif;
        EXPECT(eip_hal_netif_get(0, &netif) == EIP_OK, "host has an interface");
        snprintf(g_host, sizeof g_host, "%u.%u.%u.%u", netif.ip[0], netif.ip[1], netif.ip[2], netif.ip[3]);
    }
    rc = eip_list_identity(g_host, &identity, 1000);
    EXPECT(rc == EIP_OK, "list identity");
    EXPECT(identity.vendor_id == 1234 && identity.product_code == 7 && identity.serial_number == 0x00C0FFEE,
           "list identity values");
    EXPECT(strcmp(identity.name, "QuickEIP Loopback") == 0, "list identity name");

    /* Broadcast scan: finds this host through the unicast-to-self target */
    rc = eip_scan_devices(devices, &device_count, 1500);
    EXPECT(rc == EIP_OK, "scan");
    for (i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == 1234 && devices[i].serial_number == 0x00C0FFEE) {
            found_self = 1;
        }
    }
    EXPECT(found_self, "scan finds local adapter");

    /* Session + explicit messaging */
    memset(&ocfg, 0, sizeof ocfg);
    ocfg.ip = g_host;
    ocfg.slot = -1;
    ocfg.vendor_id = 4321;
    ocfg.serial_number = 0x1111;
    ocfg.timeout_ms = 2000;
    rc = eip_session_open(&session, &ocfg);
    EXPECT(rc == EIP_OK, "session open");
    len = sizeof buf;
    rc = eip_get_attribute(&session, 0x01, 1, 1, buf, &len, &status);
    EXPECT(rc == EIP_OK && len == 2 && eip_load_le16(buf) == 1234, "get identity vendor");
    len = sizeof buf;
    rc = eip_get_attribute(&session, 0x04, 100, 3, buf, &len, &status);
    EXPECT(rc == EIP_OK && len == 16 && memcmp(buf, kInitial, 16) == 0, "get produced assembly data");
    memset(buf, 0x5A, 32);
    rc = eip_set_attribute(&session, 0x04, 150, 3, buf, 32, &status);
    EXPECT(rc == EIP_OK, "set consumed assembly data");
    len = sizeof buf;
    rc = eip_adapter_read(150, buf, &len);
    EXPECT(rc == EIP_OK && len == 32 && buf[0] == 0x5A && buf[31] == 0x5A, "adapter sees explicit write");
    len = sizeof buf;
    rc = eip_get_attribute(&session, 0x04, 999, 3, buf, &len, &status);
    EXPECT(rc == EIP_ERR_CIP && status == 0x16, "unknown assembly is object does not exist");

    /* The QoS object exists only on a port that marks packets (not on Windows). */
    len = sizeof buf;
    rc = eip_get_attribute(&session, 0x48, 1, 7, buf, &len, &status);
    if (view.qos) {
        EXPECT(rc == EIP_OK && len == 1 && buf[0] == 20, "QoS object reports the stored low DSCP");
    } else {
        EXPECT(rc == EIP_ERR_CIP && status == 0x16, "no QoS object on a port that cannot mark");
    }

    /* allow_change gates TCP/IP Interface sets and Identity Reset. apply_interface and reset run
     * about 150 ms and 300 ms after the reply, so each half sleeps 500 ms before checking them.
     * Service 0x05 with type 0 is Identity Reset (restart). */
    {
        static const uint8_t host[] = {4, 0, 'c', 'c', '0', '1'};   /* attr 6: UINT length + chars */
        static const uint8_t reset_path[] = {0x20, 0x01, 0x24, 0x01};
        uint8_t reset_type = 0;
        uint8_t rsp[8];
        uint16_t rsp_len;
        uint16_t ext = 0;

        len = sizeof buf;
        rc = eip_get_attribute(&session, 0xF5, 1, 2, buf, &len, &status);
        EXPECT(rc == EIP_OK && len == 4 && (eip_load_le32(buf) & 0x10u) != 0, "tcp/ip reports settable");

        g_refuse = 0x10;
        rc = eip_set_attribute(&session, 0xF5, 1, 6, host, sizeof host, &status);
        EXPECT(rc == EIP_ERR_CIP && status == 0x10, "refused set answers the app's status");
        EXPECT(g_allow_calls == 1 && g_last_what == EIP_CHANGE_INTERFACE, "allow_change asked for interface");
        rsp_len = sizeof rsp;
        rc = eip_cip(&session, 0x05, reset_path, sizeof reset_path, &reset_type, 1, rsp, &rsp_len, &status, &ext);
        EXPECT(rc == EIP_ERR_CIP && status == 0x10, "refused reset answers the app's status");
        EXPECT(g_last_what == EIP_CHANGE_RESET, "allow_change asked for reset");
        usleep(500000);
        EXPECT(g_applied == 0 && g_reset_type == -1, "refused changes never reach the callbacks");
        len = sizeof buf;
        rc = eip_get_attribute(&session, 0xF5, 1, 6, buf, &len, &status);
        EXPECT(rc == EIP_OK && !(len >= 6 && memcmp(buf + 2, "cc01", 4) == 0), "refused set leaves host name");

        g_refuse = 0;
        rc = eip_set_attribute(&session, 0xF5, 1, 6, host, sizeof host, &status);
        EXPECT(rc == EIP_OK, "allowed set accepted");
        rsp_len = sizeof rsp;
        rc = eip_cip(&session, 0x05, reset_path, sizeof reset_path, &reset_type, 1, rsp, &rsp_len, &status, &ext);
        EXPECT(rc == EIP_OK, "allowed reset accepted");
        usleep(500000);
        EXPECT(g_applied == 1 && strcmp(g_applied_host, "cc01") == 0, "allowed set reaches apply_interface");
        EXPECT(g_reset_type == 0, "allowed reset reaches reset");
    }

    /* Class 1 */
    memset(&iocfg, 0, sizeof iocfg);
    iocfg.config_instance = 151;
    iocfg.output_instance = 150;
    iocfg.input_instance = 100;
    iocfg.output_bytes = 32;
    iocfg.input_bytes = 16;
    iocfg.o2t_rpi_us = 10000;
    iocfg.t2o_rpi_us = 10000;
    iocfg.run_idle = 1;
    iocfg.timeout_multiplier = 2;
    rc = eip_io_open(&session, &io, &iocfg);
    EXPECT(rc == EIP_OK, "io open");
    if (rc == EIP_OK) {
        uint8_t out[32];
        uint8_t in[16];
        uint16_t in_len = sizeof in;
        uint8_t updated[16];

        for (i = 0; i < 32; i++) {
            out[i] = (uint8_t)(0xA0 + i);
        }
        /* One packet per call: pacing at the RPI is up to the caller. */
        for (i = 0; i < 20; i++) {
            EXPECT(eip_io_produce(&io, out, sizeof out, 1) == EIP_OK, "io produce");
            usleep(10000);
        }
        len = sizeof buf;
        rc = eip_adapter_read(150, buf, &len);
        EXPECT(rc == EIP_OK && memcmp(buf, out, 32) == 0, "adapter consumed O->T data");

        memset(updated, 0x77, sizeof updated);
        EXPECT(eip_adapter_write(100, updated, sizeof updated) == EIP_OK, "adapter write produced");
        usleep(50000);
        rc = EIP_ERR_TIMEOUT;
        for (i = 0; i < 200; i++) { /* drain T->O packets queued before the write */
            in_len = sizeof in;
            rc = eip_io_consume(&io, in, &in_len, 500);
            if (rc == EIP_OK && in[0] == 0x77) {
                break;
            }
        }
        EXPECT(rc == EIP_OK && in_len == 16 && in[0] == 0x77 && in[15] == 0x77, "scanner consumed T->O data");

        EXPECT(eip_adapter_snapshot(&view) == EIP_OK && view.io_open && view.run, "adapter view io open");
        EXPECT(view.o2t_instance == 150 && view.t2o_instance == 100 && view.rpi_us == 10000, "adapter view io");
        EXPECT(view.rx_packets >= 15 && view.tx_packets > 0, "adapter counters");
        {
            static const uint8_t want[] = {EIP_EVENT_IO_OPENED, EIP_EVENT_RUN};
            EXPECT(EventsAre(0, want, 2), "events: opened, then run");
        }
        EXPECT(g_network_led == EIP_LED_GREEN && view.network_led == EIP_LED_GREEN, "network led green while open");

        /* idle: data goes to zero */
        EXPECT(eip_io_produce(&io, out, sizeof out, 0) == EIP_OK, "io produce idle");
        usleep(30000);
        len = sizeof buf;
        EXPECT(eip_adapter_read(150, buf, &len) == EIP_OK && buf[0] == 0 && buf[31] == 0, "idle clears data");

        EXPECT(eip_io_close(&session, &io) == EIP_OK, "io close");
        usleep(30000);
        EXPECT(eip_adapter_snapshot(&view) == EIP_OK && !view.io_open, "adapter io closed");
        EXPECT(view.opens == 1 && view.timeouts == 0, "one open, no timeout");
        {
            static const uint8_t want[] = {EIP_EVENT_IDLE, EIP_EVENT_IO_CLOSED};
            EXPECT(EventsAre(2, want, 2), "events: idle, then closed");
        }
        EXPECT(g_network_led == EIP_LED_GREEN_FLASH, "network led flashes green after a close");
    }

    /* Inactivity timeout: open, then go silent */
    rc = eip_io_open(&session, &io, &iocfg);
    EXPECT(rc == EIP_OK, "io reopen");
    if (rc == EIP_OK) {
        uint8_t out[32] = {0};
        EXPECT(eip_io_produce(&io, out, sizeof out, 1) == EIP_OK, "io produce once");
        usleep(400000); /* 10 ms x 4 << 2 = 160 ms timeout */
        EXPECT(eip_adapter_snapshot(&view) == EIP_OK && !view.io_open && view.timeouts == 1,
               "watchdog closes silent connection");
        {
            static const uint8_t want[] = {EIP_EVENT_IO_OPENED, EIP_EVENT_RUN, EIP_EVENT_IO_TIMEOUT};
            EXPECT(EventsAre(4, want, 3), "events: opened, run, timeout");
        }
        EXPECT(g_network_led == EIP_LED_RED_FLASH && view.network_led == EIP_LED_RED_FLASH,
               "network led flashes red after a timeout");
        (void)eip_io_close(&session, &io);
    }

    /* TCP/IP Interface attr 13: a scanner sets the encapsulation inactivity timeout, the application
     * is asked to store it, and a session silent that long is closed. The session has been silent
     * for well under the default 120 s until now. */
    {
        uint8_t seconds[2] = {1, 0};

        rc = eip_set_attribute(&session, 0xF5, 1, 13, seconds, 2, &status);
        EXPECT(rc == EIP_OK, "set encapsulation inactivity timeout");
        usleep(100000);
        EXPECT(g_stored == 1 && g_stored_nv.encap_timeout_s == 1 && g_stored_nv.dscp_low == 20,
               "store_nv gets the whole settings block");
        usleep(500000);
        len = sizeof buf;
        EXPECT(eip_get_attribute(&session, 0xF5, 1, 13, buf, &len, &status) == EIP_OK && len == 2 && buf[0] == 1,
               "session alive inside the timeout");
        usleep(1600000);
        EXPECT(eip_adapter_snapshot(&view) == EIP_OK && view.tcp_clients == 0, "silent session closed");
        len = sizeof buf;
        EXPECT(eip_get_attribute(&session, 0x01, 1, 1, buf, &len, &status) != EIP_OK,
               "request on the closed session fails");
        EXPECT(g_stored == 1, "nothing stored without a change");
    }

    /* A closed session object can be opened again. */
    eip_session_close(&session);
    EXPECT(eip_session_open(&session, &ocfg) == EIP_OK, "session reopen");
    eip_session_close(&session);

    if (g_failed == 0) {
        printf("loopback tests passed\n");
    }
    return g_failed ? 1 : 0;
}
