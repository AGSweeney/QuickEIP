/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Host unit tests for the scanner session (src/eip_session.c) against a scripted device.
 *
 * The fake HAL below plays the device's side of the TCP connection: each request reaches
 * OnRequest, which queues the reply bytes the test wants. That covers what the loopback test
 * cannot, because QuickEIP's own adapter never produces these replies: routed requests through a
 * ControlLogix backplane, and replies that arrive late, malformed or cut short. The clock advances
 * on every read, so timeouts take no real time.
 *
 * Links quickeip_core with no port. Run: ctest --test-dir build -R session, or
 * ./build/test_session. A clean run prints "session tests passed".
 */
#include "eip_codec.h"
#include "eip_hal.h"

#include <stdio.h>
#include <string.h>

static int g_failed = 0;

#define EXPECT(cond, name)                                         \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL %s (line %d)\n", name, __LINE__);         \
            g_failed++;                                            \
        }                                                          \
    } while (0)

enum { kHandle = 0x1234 };

/* ---- scripted device ---- */

typedef enum {
    kSilent,        /* no reply at all */
    kReply,         /* the next scripted CIP reply */
    kStaleThenReply, /* a reply to the previous request, then the next scripted CIP reply */
    kWrongContext,  /* a reply whose context this session never sent */
    kCutShort,      /* 10 bytes of a header, then nothing */
    kOversize       /* a header advertising a body larger than the session buffer */
} Behavior;

static Behavior g_behavior;
static const uint8_t *g_cip[4];   /* scripted CIP replies, in order */
static uint16_t g_cip_len[4];
static int g_next_cip;
static uint8_t g_wire[2048];      /* bytes waiting for eip_hal_tcp_recv */
static uint16_t g_wire_len;
static uint16_t g_wire_at;
static uint8_t g_last_cip_service; /* first byte of the last CIP request */
static uint32_t g_now;
static int g_closed;

static void Queue(const uint8_t *data, uint16_t len)
{
    memcpy(g_wire + g_wire_len, data, len);
    g_wire_len = (uint16_t)(g_wire_len + len);
}

/* Queue one encapsulation reply carrying a CIP reply, with the given sender context. */
static void QueueCip(const uint8_t context[8], const uint8_t *cip, uint16_t cip_len)
{
    uint8_t body[640];
    uint8_t packet[700];
    uint16_t body_len = 0;
    uint16_t packet_len = 0;

    if (eip_build_rr_body(body, sizeof body, 0, cip, cip_len, 0, 0, 0, &body_len) != EIP_OK ||
        eip_build_encap(packet, sizeof packet, EIP_CMD_SEND_RR, kHandle, context, body, body_len,
                        &packet_len) != EIP_OK) {
        printf("FAIL scripted reply did not build\n");
        g_failed++;
        return;
    }
    Queue(packet, packet_len);
}

/* A request past the end of the script gets no reply. */
static void QueueNextCip(const uint8_t context[8])
{
    if (g_next_cip >= 4 || g_cip[g_next_cip] == 0) {
        return;
    }
    QueueCip(context, g_cip[g_next_cip], g_cip_len[g_next_cip]);
    g_next_cip++;
}

static void OnRequest(const uint8_t *request, uint16_t len)
{
    uint16_t command = eip_load_le16(request);
    const uint8_t *context = request + 12;
    uint8_t other[8];
    uint8_t packet[64];
    uint16_t packet_len = 0;
    const uint8_t late[] = {0xCC, 0x00, 0x00, 0x00, 0xC4, 0x00, 0xEE, 0xEE, 0xEE, 0xEE};

    if (command == EIP_CMD_REGISTER) {
        const uint8_t body[4] = {1, 0, 0, 0};
        (void)eip_build_encap(packet, sizeof packet, EIP_CMD_REGISTER, kHandle, context, body, 4,
                              &packet_len);
        Queue(packet, packet_len);
        return;
    }
    if (command != EIP_CMD_SEND_RR) {
        return; /* UnregisterSession has no reply */
    }
    /* Header 24, interface handle and timeout 6, item count 2, null item 4, data item header 4. */
    if (len > 40u) {
        g_last_cip_service = request[40];
    }
    switch (g_behavior) {
    case kSilent:
        break;
    case kReply:
        QueueNextCip(context);
        break;
    case kStaleThenReply:
        memcpy(other, context, 8);
        eip_store_le32(other, eip_load_le32(context) - 1u);
        QueueCip(other, late, sizeof late);
        QueueNextCip(context);
        break;
    case kWrongContext:
        memset(other, 0xA5, sizeof other);
        QueueCip(other, late, sizeof late);
        break;
    case kCutShort:
        memset(packet, 0, 10);
        Queue(packet, 10);
        break;
    case kOversize:
        memset(packet, 0, 24);
        eip_store_le16(packet, EIP_CMD_SEND_RR);
        eip_store_le16(packet + 2, 0xFFFF);
        Queue(packet, 24);
        break;
    }
}

/* ---- fake HAL: only what eip_session.c calls ---- */

uint32_t eip_hal_millis(void)
{
    g_now += 10u;
    return g_now;
}

eip_sock_t eip_hal_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
    (void)ip;
    (void)port;
    (void)timeout_ms;
    g_closed = 0;
    return 3;
}

int eip_hal_tcp_send(eip_sock_t sock, const uint8_t *data, uint16_t len)
{
    (void)sock;
    OnRequest(data, len);
    return EIP_OK;
}

int eip_hal_tcp_recv(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint32_t timeout_ms)
{
    uint16_t left = (uint16_t)(g_wire_len - g_wire_at);
    uint16_t n = (left < cap) ? left : cap;

    (void)sock;
    (void)timeout_ms;
    memcpy(buf, g_wire + g_wire_at, n);
    g_wire_at = (uint16_t)(g_wire_at + n);
    return n;
}

void eip_hal_tcp_close(eip_sock_t sock)
{
    (void)sock;
    g_closed = 1;
}

/* ---- tests ---- */

static EipSession g_session;

static void Open(int16_t slot)
{
    EipOpenConfig config;

    memset(&config, 0, sizeof config);
    config.ip = "10.0.0.5";
    config.slot = slot;
    config.timeout_ms = 200;
    g_wire_len = 0;
    g_wire_at = 0;
    g_next_cip = 0;
    memset(g_cip, 0, sizeof g_cip);
    g_last_cip_service = 0;
    g_behavior = kReply;
    EXPECT(eip_session_open(&g_session, &config) == EIP_OK, "session opens");
}

static void Script(int index, const uint8_t *cip, uint16_t cip_len)
{
    g_cip[index] = cip;
    g_cip_len[index] = cip_len;
}

/* A successful routed reply is the target's own reply. The router removes the Unconnected Send
 * layer, so there is no outer reply to unwrap. */
static void TestRoutedRead(void)
{
    const uint8_t reply[] = {0xCC, 0x00, 0x00, 0x00, 0xC4, 0x00, 0x2A, 0x00, 0x00, 0x00};
    uint8_t data[4];
    uint16_t len = sizeof data;
    uint16_t type = 0;
    int rc;

    Open(1);
    Script(0, reply, sizeof reply);
    rc = eip_tag_read(&g_session, "MyDint", 1, data, &len, &type, 0);
    EXPECT(g_last_cip_service == 0x52, "routed read goes out as Unconnected Send");
    EXPECT(rc == EIP_OK, "routed read succeeds");
    EXPECT(type == EIP_DINT && len == 4 && eip_load_le32(data) == 42, "routed read data");
    eip_session_close(&g_session);
}

/* Read Tag Fragmented is service 0x52, the same code as Unconnected Send, so its success reply
 * is 0xD2. Telling the two apart by service code would break this read. */
static void TestRoutedFragmentedRead(void)
{
    const uint8_t first[] = {0xCC, 0x00, 0x06, 0x00, 0xC4, 0x00, 0x01, 0x02};
    const uint8_t second[] = {0xD2, 0x00, 0x00, 0x00, 0xC4, 0x00, 0x03, 0x04};
    const uint8_t expect[] = {1, 2, 3, 4};
    uint8_t data[4];
    uint16_t len = sizeof data;
    uint16_t type = 0;
    int rc;

    Open(1);
    Script(0, first, sizeof first);
    Script(1, second, sizeof second);
    rc = eip_tag_read(&g_session, "MyDint", 1, data, &len, &type, 0);
    EXPECT(rc == EIP_OK && len == 4 && memcmp(data, expect, 4) == 0, "routed fragmented read");
    eip_session_close(&g_session);
}

static void TestRoutedWrite(void)
{
    const uint8_t reply[] = {0xCD, 0x00, 0x00, 0x00};
    const uint8_t value[4] = {1, 0, 0, 0};

    Open(1);
    Script(0, reply, sizeof reply);
    EXPECT(eip_tag_write(&g_session, "MyDint", EIP_DINT, 1, value, 4, 0) == EIP_OK,
           "routed write succeeds");
    eip_session_close(&g_session);
}

/* A router that cannot deliver answers for itself: 0xD2, status 0x01, extended status 0x0204
 * (timed out), then the remaining path size and a reserved byte. */
static void TestRouterFailure(void)
{
    const uint8_t reply[] = {0xD2, 0x00, 0x01, 0x01, 0x04, 0x02, 0x01, 0x00};
    uint8_t data[1];
    uint16_t len = sizeof data;

    Open(3);
    Script(0, reply, sizeof reply);
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_ERR_CIP,
           "router failure is a CIP error");
    EXPECT(eip_last_cip_status(&g_session) == 0x01 && eip_last_cip_extended(&g_session) == 0x0204,
           "router failure status");
    eip_session_close(&g_session);
}

/* The status must survive a buffer too small for the failure's own data. */
static void TestRouterFailureSmallBuffer(void)
{
    const uint8_t reply[] = {0xD2, 0x00, 0x01, 0x01, 0x04, 0x02, 0x01, 0x00};
    const uint8_t path[] = {0x20, 0x01, 0x24, 0x01, 0x30, 0x01};
    uint8_t data[1];
    uint16_t len = sizeof data;

    Open(3);
    Script(0, reply, sizeof reply);
    EXPECT(eip_cip(&g_session, 0x0E, path, sizeof path, 0, 0, data, &len, 0, 0) == EIP_ERR_CIP,
           "failure beats a small buffer");
    eip_session_close(&g_session);
}

static void TestRoutedTargetError(void)
{
    const uint8_t reply[] = {0xCC, 0x00, 0x05, 0x00};
    uint8_t data[4];
    uint16_t len = sizeof data;

    Open(1);
    Script(0, reply, sizeof reply);
    EXPECT(eip_tag_read(&g_session, "Missing", 1, data, &len, 0, 0) == EIP_ERR_CIP,
           "target error is a CIP error");
    EXPECT(eip_last_cip_status(&g_session) == 0x05, "target error status");
    eip_session_close(&g_session);
}

static void TestDirectRead(void)
{
    const uint8_t reply[] = {0xCC, 0x00, 0x00, 0x00, 0xC4, 0x00, 0x07, 0x00, 0x00, 0x00};
    uint8_t data[4];
    uint16_t len = sizeof data;

    Open(-1);
    Script(0, reply, sizeof reply);
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_OK, "direct read");
    EXPECT(g_last_cip_service == 0x4C, "direct read is not wrapped");
    EXPECT(eip_load_le32(data) == 7, "direct read data");
    eip_session_close(&g_session);
}

/* "More data follows" with no data would never advance the read. */
static void TestEmptyPartialReplyRefused(void)
{
    const uint8_t reply[] = {0xCC, 0x00, 0x06, 0x00, 0xC4, 0x00};
    uint8_t data[4];
    uint16_t len = sizeof data;
    int i;

    Open(-1);
    for (i = 0; i < 4; i++) {
        Script(i, reply, sizeof reply);
    }
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_ERR_PROTOCOL,
           "empty partial reply refused");
    eip_session_close(&g_session);
}

/* A reply that misses its timeout turns up during the next request. It is discarded, and the
 * session stays in step. */
static void TestLateReplySkipped(void)
{
    const uint8_t reply[] = {0xCC, 0x00, 0x00, 0x00, 0xC4, 0x00, 0x09, 0x00, 0x00, 0x00};
    uint8_t data[4];
    uint16_t len = sizeof data;

    Open(-1);
    Script(0, reply, sizeof reply);
    g_behavior = kSilent;
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_ERR_TIMEOUT,
           "silent device times out");
    EXPECT(eip_session_is_open(&g_session), "a clean timeout keeps the session");

    g_behavior = kStaleThenReply;
    len = sizeof data;
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_OK,
           "late reply is skipped");
    EXPECT(eip_load_le32(data) == 9, "the current reply is the one returned");
    eip_session_close(&g_session);
}

/* Only recent contexts count as late. Anything else is still a protocol error. */
static void TestForeignContextRefused(void)
{
    uint8_t data[4];
    uint16_t len = sizeof data;

    Open(-1);
    g_behavior = kWrongContext;
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_ERR_PROTOCOL,
           "foreign context refused");
    eip_session_close(&g_session);
}

/* A packet cut short leaves no way to find the next one, so the session must not carry on. */
static void TestCutShortClosesSession(void)
{
    uint8_t data[4];
    uint16_t len = sizeof data;

    Open(-1);
    g_behavior = kCutShort;
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_ERR_TIMEOUT,
           "cut-short packet times out");
    EXPECT(!eip_session_is_open(&g_session) && g_closed, "cut-short packet closes the session");
    len = sizeof data;
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_ERR_STATE,
           "closed session refuses requests");
}

static void TestOversizeClosesSession(void)
{
    uint8_t data[4];
    uint16_t len = sizeof data;

    Open(-1);
    g_behavior = kOversize;
    EXPECT(eip_tag_read(&g_session, "MyDint", 1, data, &len, 0, 0) == EIP_ERR_TOO_BIG,
           "oversize packet refused");
    EXPECT(!eip_session_is_open(&g_session) && g_closed, "oversize packet closes the session");
}

int main(void)
{
    TestRoutedRead();
    TestRoutedFragmentedRead();
    TestRoutedWrite();
    TestRouterFailure();
    TestRouterFailureSmallBuffer();
    TestRoutedTargetError();
    TestDirectRead();
    TestEmptyPartialReplyRefused();
    TestLateReplySkipped();
    TestForeignContextRefused();
    TestCutShortClosesSession();
    TestOversizeClosesSession();
    if (g_failed != 0) {
        printf("%d failed\n", g_failed);
        return 1;
    }
    printf("session tests passed\n");
    return 0;
}
