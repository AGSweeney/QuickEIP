/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Host unit tests for the codec (src/eip_codec.c) and the sans-I/O adapter core.
 *
 * No sockets and no clock: the tests call the codec and eip_adapter_on_encap() directly with
 * hand-made packets and tick values, so the binary links quickeip_core without a port. Wire
 * formats are pinned byte for byte, and several adapter tests replay what real devices send.
 *
 * Run: ctest --test-dir build -R core, or ./build/test_core. Each failed check prints a FAIL
 * line and the exit status is 1. A clean run prints "codec tests passed".
 */
#include "eip_codec.h"
#include "eip_adapter.h"

/* Identity Reset service. eip_codec.h has no name for it. */
#define EIP_CIP_RESET 0x05

#include <stdio.h>
#include <string.h>

static int g_failed = 0;

/* Records a failure and carries on, so one run reports every broken check. */
static void Expect(int cond, const char *name)
{
    if (!cond) {
        printf("FAIL %s\n", name);
        g_failed++;
    }
}

/* ---- codec: encode and decode, no adapter state ------------------------- */

/* 5000 ms needs a tick unit of 2^5 = 32 ms, and 5000 / 32 rounds up to 157 ticks. This pair is the
 * tick and timeout-ticks bytes of Forward_Open and Unconnected Send. */
static void TestTick(void)
{
    uint8_t tick = 0;
    uint8_t ticks = 0;
    eip_tick_pair(5000, &tick, &ticks);
    Expect(tick == 5 && ticks == 157, "tick pair 5000 ms");
}

/* Symbolic path encoding. 0x91 is a symbol segment: length, name, and a 0x00 pad when the length
 * is odd. [n] adds an element segment: 0x28 with one byte, or 0x29 0x00 with a little-endian
 * 16-bit index. "Program:Main.Start" splits at the dot into two symbol segments. */
static void TestTagPath(void)
{
    uint8_t path[64];
    uint16_t len = 0;
    const uint8_t simple[] = {0x91, 0x08, 'D', 'I', 'N', 'T', '_', 'T', 'a', 'g'};
    const uint8_t indexed[] = {0x91, 0x03, 'T', 'a', 'g', 0x00, 0x28, 0x03};
    const uint8_t wide[] = {0x91, 0x03, 'T', 'a', 'g', 0x00, 0x29, 0x00, 0x2C, 0x01};
    int rc = eip_build_tag_path("DINT_Tag", path, sizeof path, &len);
    Expect(rc == EIP_OK && len == sizeof simple && memcmp(path, simple, len) == 0, "simple tag");
    rc = eip_build_tag_path("Tag[3]", path, sizeof path, &len);
    Expect(rc == EIP_OK && len == sizeof indexed && memcmp(path, indexed, len) == 0, "tag element");
    rc = eip_build_tag_path("Tag[300]", path, sizeof path, &len);
    Expect(rc == EIP_OK && len == sizeof wide && memcmp(path, wide, len) == 0, "tag element 300");
    rc = eip_build_tag_path("Program:Main.Start", path, sizeof path, &len);
    Expect(rc == EIP_OK && path[0] == 0x91 && path[1] == 12 && path[14] == 0x91, "program tag");
    Expect(eip_build_tag_path("", path, sizeof path, &len) == EIP_ERR_ARG, "empty tag");
    Expect(eip_build_tag_path("Bad Tag", path, sizeof path, &len) == EIP_ERR_PATH, "space in tag");
}

/* Register Session: a 24-byte encapsulation header (command 0x65, length 4) and a body of
 * protocol version 1, options 0. */
static void TestEncap(void)
{
    uint8_t packet[64];
    uint8_t context[8];
    uint8_t body[4] = {0x01, 0x00, 0x00, 0x00};
    uint16_t len = 0;
    EipEncapView view;
    int rc;
    memset(context, 0, sizeof context);
    rc = eip_build_encap(packet, sizeof packet, EIP_CMD_REGISTER, 0, context, body, 4, &len);
    Expect(rc == EIP_OK && len == 28, "register length");
    Expect(packet[0] == 0x65 && packet[1] == 0x00 && packet[2] == 0x04 && packet[24] == 0x01,
           "register bytes");
    rc = eip_parse_encap(packet, len, &view);
    Expect(rc == EIP_OK && view.command == EIP_CMD_REGISTER && view.length == 4, "parse register");
}

static void TestWrapAndParams(void)
{
    uint8_t inner[2] = {0x4C, 0x00};
    uint8_t out[32];
    uint16_t len = 0;
    /* Unconnected Send (0x52) to the Connection Manager (20 06 24 01): tick 5, ticks 10, embedded
     * length 2, the inner request, then route path size 1, reserved, port 1, slot 0. */
    const uint8_t expect[] = {0x52, 0x02, 0x20, 0x06, 0x24, 0x01, 0x05, 0x0A, 0x02, 0x00,
                              0x4C, 0x00, 0x01, 0x00, 0x01, 0x00};
    int rc = eip_wrap_unconnected(out, sizeof out, inner, 2, 0, 5, 10, &len);
    Expect(rc == EIP_OK && len == sizeof expect && memcmp(out, expect, len) == 0, "unconnected wrap");
    /* Size 10 in the low bits, connection type 2 (point-to-point) at bit 29. */
    Expect(eip_large_conn_params(10, 0, 0, 2, 0) == 0x4000000Au, "large connection params");
    /* Every field at once: owner bit 31, type 30..29, priority 27..26, variable bit 25. The
     * adapter's DecodeParam reads the same layout. */
    Expect(eip_large_conn_params(10, 1, 2, 2, 1) == 0xCA00000Au, "large connection params, all bits");
}

/* Large_Forward_Open (0x5B) field offsets. Service and the 2-word Connection Manager path take
 * 6 bytes, which puts the 32-bit O->T parameters at 32 and the transport byte at 44. */
static void TestForwardOpen(void)
{
    EipForwardOpenInfo info;
    uint8_t path[8];
    uint16_t path_len = 0;
    uint8_t request[128];
    uint16_t request_len = 0;
    int rc = eip_build_assembly_path(path, sizeof path, &path_len, 1, 100, 101);
    memset(&info, 0, sizeof info);
    info.priority_tick = 5;
    info.timeout_ticks = 157;
    info.o2t_id = 0x11111111u;
    info.connection_serial = 7;
    info.vendor_id = 1;
    info.originator_serial = 0x10001;
    info.o2t_rpi_us = 20000;
    info.t2o_rpi_us = 20000;
    info.o2t_params = 0x4000000Au;
    info.t2o_params = 0x4000000Cu;
    info.transport = 0x01;
    rc |= eip_build_forward_open_request(request, sizeof request, &request_len, &info, path, path_len);
    Expect(rc == EIP_OK && request[0] == 0x5B && request[1] == 0x02, "forward open service");
    Expect(eip_load_le32(request + 32) == 0x4000000Au, "o2t params offset");
    Expect(request[44] == 0x01, "class 1 transport");
}

/* Class 1 packet with a run/idle header. The parser strips the 16-bit count only, so the 4-byte
 * header (run = 1) stays in front of the two data bytes in the payload. */
static void TestIoRoundTrip(void)
{
    uint8_t data[2] = {0x34, 0x12};
    uint8_t packet[64];
    uint16_t len = 0;
    uint32_t connection_id = 0;
    uint16_t sequence = 0;
    const uint8_t *payload = 0;
    uint16_t payload_len = 0;
    int rc = eip_build_io_packet(packet, sizeof packet, &len, 0xAABBCCDDu, 9, 3, 1, 1, data, 2);
    Expect(rc == EIP_OK, "build io");
    rc = eip_parse_io_packet(packet, len, &connection_id, &sequence, &payload, &payload_len);
    Expect(rc == EIP_OK && connection_id == 0xAABBCCDDu && sequence == 3, "parse io header");
    Expect(payload_len == 6 && eip_load_le32(payload) == 1u && payload[4] == 0x34 && payload[5] == 0x12,
           "parse io run and data");
}

/* ListIdentity reply built by hand: item count 1, item 0x000C, length 40. Item data offsets:
 * version 0, sockaddr 2, vendor 18, device type 20, product 22, revision 24, status 26, serial 28,
 * name length 32, name 33, and the state byte right after the name. */
static void TestIdentity(void)
{
    uint8_t body[80];
    EipIdentity identity;
    int rc;
    memset(body, 0, sizeof body);
    eip_store_le16(body, 1);
    eip_store_le16(body + 2, EIP_ITEM_IDENTITY);
    eip_store_le16(body + 4, 40);
    eip_store_le16(body + 6 + 18, 0x1234);
    eip_store_le16(body + 6 + 20, 0x000C);
    eip_store_le16(body + 6 + 22, 0x00AB);
    body[6 + 24] = 3;
    body[6 + 25] = 1;
    eip_store_le16(body + 6 + 26, 0x0060);
    eip_store_le32(body + 6 + 28, 0x89ABCDEFu);
    body[6 + 32] = 4;
    memcpy(body + 6 + 33, "Pump", 4);
    body[6 + 37] = 0x03;
    rc = eip_parse_identity(body, 6 + 40, &identity);
    Expect(rc == EIP_OK && identity.vendor_id == 0x1234 && identity.product_code == 0x00AB, "identity ids");
    Expect(strcmp(identity.name, "Pump") == 0 && identity.state == 0x03 && identity.revision_major == 3,
           "identity name");
}

/* One symbol-list record as a controller returns it: instance, type, element size, three
 * dimensions, name length (22 bytes so far), then the name. */
static void AppendSymbol(uint8_t *page, uint16_t *used, uint32_t instance, uint16_t type, uint16_t elem,
                         uint32_t dim0, uint32_t dim1, uint32_t dim2, const char *name)
{
    uint16_t name_len = (uint16_t)strlen(name);
    uint8_t *p = page + *used;
    eip_store_le32(p, instance);
    eip_store_le16(p + 4, type);
    eip_store_le16(p + 6, elem);
    eip_store_le32(p + 8, dim0);
    eip_store_le32(p + 12, dim1);
    eip_store_le32(p + 16, dim2);
    eip_store_le16(p + 20, name_len);
    memcpy(p + 22, name, name_len);
    *used = (uint16_t)(*used + 22u + name_len);
}

/* Symbol listing and the Rockwell catalog heuristics. The page holds a scalar, a 2 x 3 array whose
 * name has a space (stored with '_') and a repeat of the first name (counted in parsed, not
 * stored). The last two cases pin partial results: a full array stops with EIP_ERR_TOO_BIG, and a
 * record cut off at its name gives EIP_ERR_PROTOCOL. */
static void TestMicro800(void)
{
    uint8_t request[32];
    uint16_t len = 0;
    uint8_t page[128];
    uint16_t used = 0;
    EipSymbol symbols[4];
    uint16_t count = 0;
    uint16_t parsed = 0;
    uint32_t last = 0;
    char name[32];
    /* Service 0x55, 3-word path to class 0x6B, 16-bit instance 2, then 4 attributes: 2, 7, 8, 1. */
    const uint8_t expect[] = {0x55, 0x03, 0x20, 0x6B, 0x25, 0x00, 0x02, 0x00, 0x04, 0x00,
                              0x02, 0x00, 0x07, 0x00, 0x08, 0x00, 0x01, 0x00};
    int rc = eip_build_symbol_list(request, sizeof request, &len, 2);

    Expect(rc == EIP_OK && len == sizeof expect && memcmp(request, expect, len) == 0, "symbol list request");
    Expect(eip_name_is_micro800(1, "2080-L50E-24QWB") == 1, "catalog 2080");
    Expect(eip_name_is_micro800(1, "2085-L20E") == 1, "catalog 2085");
    Expect(eip_name_is_micro800(1, "Micro850") == 1, "family name");
    Expect(eip_name_is_micro800(1, "1769-L33ER") == 0, "compactlogix is not micro800");
    Expect(eip_name_is_micro800(2, "2080-LC50") == 0, "other vendor");
    Expect(eip_name_is_compactlogix(1, "1769-L33ER") == 1, "1769 compact");
    Expect(eip_name_is_compactlogix(1, "5069-L306ER") == 1, "5069 compact");
    Expect(eip_name_is_compactlogix(1, "CompactLogix 5380") == 1, "compact name");
    Expect(eip_name_is_compactlogix(1, "1756-L83E") == 0, "controllogix is not compact");
    Expect(eip_name_is_compactlogix(1, "2080-L50E-24QWB") == 0, "micro catalog is not compact");
    Expect(eip_device_kind(1, "2080-L50E-24QWB") == EIP_KIND_MICRO800, "kind micro");
    Expect(eip_device_kind(1, "1769-L16ER") == EIP_KIND_COMPACTLOGIX, "kind compact");
    Expect(eip_device_kind(1, "PowerFlex 525") == EIP_KIND_ASSEMBLY, "kind assembly");

    memcpy(name, "2080-L50E*24", 13);
    eip_sanitize_product_name(name);
    Expect(strcmp(name, "2080-L50E_24") == 0, "sanitize product name");

    AppendSymbol(page, &used, 1, EIP_DINT, 4, 0, 0, 0, "Start");
    AppendSymbol(page, &used, 2, EIP_DINT, 4, 2, 3, 0, "Va l");
    AppendSymbol(page, &used, 3, EIP_DINT, 4, 0, 0, 0, "Start");
    rc = eip_parse_symbol_page(page, used, symbols, 4, &count, &parsed, &last);
    Expect(rc == EIP_OK && parsed == 3 && count == 2 && last == 3, "symbol page counts");
    Expect(strcmp(symbols[0].name, "Start") == 0 && symbols[0].is_array == 0 &&
               symbols[0].element_count == 1 && symbols[0].symbol_type == EIP_DINT,
           "scalar symbol");
    Expect(strcmp(symbols[1].name, "Va_l") == 0 && symbols[1].is_array == 1 && symbols[1].element_count == 6,
           "array symbol");

    count = 0;
    rc = eip_parse_symbol_page(page, used, symbols, 1, &count, &parsed, &last);
    Expect(rc == EIP_ERR_TOO_BIG && count == 1 && strcmp(symbols[0].name, "Start") == 0, "symbol page full");

    count = 0;
    rc = eip_parse_symbol_page(page, 22, symbols, 4, &count, &parsed, &last);
    Expect(rc == EIP_ERR_PROTOCOL && count == 0, "truncated symbol record");
}

/* ---- adapter core: requests through eip_adapter_on_encap ---------------- */

/* One encapsulation request through the adapter, from a client at 172.16.82.199 at tick 5000.
 * *session goes on the request, and Register stores the new handle back in it. */
static int RoundTrip(EipAdapter *adapter, uint32_t *session, uint16_t command, const uint8_t *body,
                      uint16_t body_len, uint8_t *response, uint16_t *response_len)
{
    uint8_t request[700];
    uint8_t context[8];
    uint8_t peer[4] = {172, 16, 82, 199};
    uint16_t request_len = 0;
    int rc;

    memset(context, 0x5A, sizeof context);
    rc = eip_build_encap(request, sizeof request, command, *session, context, body, body_len, &request_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return eip_adapter_on_encap(adapter, session, peer, request, request_len, 5000, response, 700,
                                response_len);
}

/* The CIP reply inside a SendRRData packet. The first 6 body bytes are the interface handle and
 * timeout; the item list follows. */
static int CipFromReply(const uint8_t *packet, uint16_t packet_len, uint8_t *service, uint8_t *status,
                        uint16_t *extended, const uint8_t **data, uint16_t *data_len)
{
    EipEncapView view;
    const uint8_t *item = 0;
    uint16_t item_len = 0;
    int rc = eip_parse_encap(packet, packet_len, &view);

    if (rc != EIP_OK || view.length < 8u) {
        return EIP_ERR_PROTOCOL;
    }
    rc = eip_cpf_find(view.body + 6, (uint16_t)(view.length - 6u), EIP_ITEM_UNCONNECTED, &item, &item_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return eip_parse_cip_rsp(item, item_len, service, status, extended, data, data_len);
}

/* A client's whole life on the default test adapter with a 20 Hz clock: ListIdentity, Register,
 * explicit read of assembly 101, Large_Forward_Open, one packet each way, a refused second
 * owner, Forward_Close, then a regular 16-bit Forward_Open. */
static void TestAdapter(void)
{
    static EipAdapter adapter;
    uint8_t ip[4] = {172, 16, 82, 224};
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t body[4] = {0x01, 0x00, 0x00, 0x00};
    uint8_t path[16];
    uint16_t path_len = 0;
    uint8_t cip[160];
    uint16_t cip_len = 0;
    uint8_t rr[320];
    uint16_t rr_len = 0;
    uint8_t extra[40];
    uint16_t extra_len = 0;
    uint16_t one_len = 0;
    EipForwardOpenInfo info;
    EipEncapView view;
    EipIdentity identity;
    uint8_t service = 0;
    uint8_t status = 0;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    uint8_t copied[16];
    uint16_t copied_len = sizeof copied;
    uint8_t produced[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    uint8_t packet[128];
    uint16_t packet_len = 0;
    uint32_t o2t_id = 0;
    uint32_t t2o_id = 0;
    uint32_t api_a = 0;
    uint32_t api_b = 0;
    uint8_t dest[4];
    uint16_t dest_port = 0;
    /* Initial contents of test assembly 101. */
    const uint8_t expect_input[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    int rc;

    eip_adapter_init(&adapter, ip, 0x0C096E6E);
    eip_adapter_set_clock(&adapter, 20);
    rc = RoundTrip(&adapter, &session, EIP_CMD_LIST_IDENTITY, 0, 0, response, &response_len);
    Expect(rc == EIP_OK && eip_parse_encap(response, response_len, &view) == EIP_OK, "adapter list identity");
    Expect(eip_parse_identity(view.body, view.length, &identity) == EIP_OK, "adapter identity parse");
    Expect(identity.vendor_id == 65000 && identity.device_type == 12, "adapter vendor and type");
    Expect(strcmp(identity.name, "EIP Test Adapter") == 0, "adapter name");

    rc = RoundTrip(&adapter, &session, EIP_CMD_REGISTER, body, 4, response, &response_len);
    Expect(rc == EIP_OK && eip_parse_encap(response, response_len, &view) == EIP_OK && view.session != 0,
           "adapter register");
    Expect(session == view.session, "adapter session handle");

    path_len = 0;
    Expect(eip_put_logical(path, sizeof path, &path_len, 0x20, 4) == EIP_OK, "asm class");
    Expect(eip_put_logical(path, sizeof path, &path_len, 0x24, 101) == EIP_OK, "asm instance");
    Expect(eip_put_logical(path, sizeof path, &path_len, 0x30, 3) == EIP_OK, "asm attribute");
    rc = eip_build_cip_req(cip, sizeof cip, EIP_CIP_GET_ATTRIBUTE, path, path_len, 0, 0, &cip_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    Expect(rc == EIP_OK, "adapter get attribute send");
    rc = CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0 && data_len == 8 && memcmp(data, expect_input, 8) == 0,
           "adapter produced assembly");

    /* Large_Forward_Open: config 150, O->T 100, T->O 101. O->T size is the 2-byte count, the
     * 4-byte run/idle header and 8 data bytes; T->O is the count and 8 data bytes. The T->O
     * sockaddr item moves the adapter's packets from port 2222 to 2223. */
    memset(&info, 0, sizeof info);
    info.priority_tick = 5;
    info.timeout_ticks = 20;
    info.o2t_id = 0x11111111u;
    info.connection_serial = 7;
    info.vendor_id = 1;
    info.originator_serial = 99;
    info.timeout_multiplier = 2;
    info.o2t_rpi_us = 20000;
    info.t2o_rpi_us = 20000;
    info.o2t_params = eip_large_conn_params((uint16_t)(2u + 4u + 8u), 0, 0, 2, 0);
    info.t2o_params = eip_large_conn_params((uint16_t)(2u + 8u), 0, 0, 2, 0);
    info.transport = 0x01;
    path_len = 0;
    Expect(eip_build_assembly_path(path, sizeof path, &path_len, 150, 100, 101) == EIP_OK, "asm path");
    rc = eip_build_forward_open_request(cip, sizeof cip, &cip_len, &info, path, path_len);
    rc |= eip_build_sockaddr_item(extra, sizeof extra, &one_len, EIP_ITEM_SOCK_O2T, ip, EIP_UDP_PORT);
    extra_len = one_len;
    rc |= eip_build_sockaddr_item(extra + extra_len, (uint16_t)(sizeof extra - extra_len), &one_len,
                                  EIP_ITEM_SOCK_T2O, (const uint8_t *)"\xAC\x10\x52\xC7", 2223);
    extra_len = (uint16_t)(extra_len + one_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, extra, extra_len, 2, &rr_len);
    rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    Expect(rc == EIP_OK, "adapter large forward open send");
    rc = CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0, "adapter forward open status");
    Expect(eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b) == EIP_OK,
           "adapter forward open body");
    Expect(o2t_id == 0x11111111u && t2o_id != 0 && t2o_id != o2t_id, "adapter connection ids");

    rc = eip_build_io_packet(packet, sizeof packet, &packet_len, o2t_id, 1, 1, 1, 1, produced, 8);
    Expect(rc == EIP_OK && eip_adapter_take_input(&adapter, packet, packet_len, 5000) == EIP_OK,
           "adapter consume");
    copied_len = sizeof copied;
    Expect(eip_adapter_copy(&adapter, 100, copied, &copied_len) == EIP_OK && copied_len == 8 &&
               memcmp(copied, produced, 8) == 0,
           "adapter output assembly");

    Expect(eip_adapter_io_due(&adapter, 5000) == 1, "adapter produce due");
    rc = eip_adapter_build_output(&adapter, 5000, packet, sizeof packet, &packet_len, dest, &dest_port);
    Expect(rc == EIP_OK && dest_port == 2223, "adapter produce destination");
    {
        uint32_t cid = 0;
        uint16_t seq = 0;
        const uint8_t *payload = 0;
        uint16_t payload_len = 0;
        Expect(eip_parse_io_packet(packet, packet_len, &cid, &seq, &payload, &payload_len) == EIP_OK,
               "adapter produced packet");
        Expect(cid == t2o_id && payload_len == 8 && memcmp(payload, expect_input, 8) == 0,
               "adapter input bytes");
    }

    /* A different originator (vendor and serial) while the connection is open: 0x01 / 0x0106. */
    info.connection_serial = 8;
    info.vendor_id = 2;
    info.originator_serial = 50;
    info.o2t_id = 0x22222222u;
    rc = eip_build_forward_open_request(cip, sizeof cip, &cip_len, &info, path, path_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0x01 && extended == 0x0106, "adapter ownership conflict");

    /* Forward_Close has to name the owner: serial 7, vendor 1, originator serial 99. */
    rc = eip_build_forward_close_request(cip, sizeof cip, &cip_len, 5, 20, 7, 1, 99, path, path_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0, "adapter forward close");
    Expect(eip_adapter_build_output(&adapter, 5000, packet, sizeof packet, &packet_len, dest, &dest_port) ==
               EIP_ERR_STATE,
           "adapter closed");

    /* Regular Forward_Open (0x54), built by hand because the codec only builds the large form.
     * Fields in order: tick, ticks, O->T id, T->O id, connection serial, vendor, originator
     * serial, timeout multiplier and 3 reserved bytes, O->T RPI, O->T parameters, T->O RPI, T->O
     * parameters, transport, path words, path. Parameters are 16 bits here: size in the low bits,
     * connection type at bit 13. */
    {
        uint8_t regular[80];
        uint16_t regular_len = 0;
        const uint8_t cm[4] = {0x20, 0x06, 0x24, 0x01};
        uint16_t o2t_param = (uint16_t)(14u | (2u << 13));
        uint16_t t2o_param = (uint16_t)(10u | (2u << 13));

        regular[regular_len++] = 5;
        regular[regular_len++] = 20;
        eip_store_le32(regular + regular_len, 0x33333333u);
        regular_len = (uint16_t)(regular_len + 4u);
        eip_store_le32(regular + regular_len, 0);
        regular_len = (uint16_t)(regular_len + 4u);
        eip_store_le16(regular + regular_len, 9);
        regular_len = (uint16_t)(regular_len + 2u);
        eip_store_le16(regular + regular_len, 1);
        regular_len = (uint16_t)(regular_len + 2u);
        eip_store_le32(regular + regular_len, 99);
        regular_len = (uint16_t)(regular_len + 4u);
        regular[regular_len++] = 2;
        regular[regular_len++] = 0;
        regular[regular_len++] = 0;
        regular[regular_len++] = 0;
        eip_store_le32(regular + regular_len, 20000);
        regular_len = (uint16_t)(regular_len + 4u);
        eip_store_le16(regular + regular_len, o2t_param);
        regular_len = (uint16_t)(regular_len + 2u);
        eip_store_le32(regular + regular_len, 20000);
        regular_len = (uint16_t)(regular_len + 4u);
        eip_store_le16(regular + regular_len, t2o_param);
        regular_len = (uint16_t)(regular_len + 2u);
        regular[regular_len++] = 0x01;
        regular[regular_len++] = (uint8_t)(path_len / 2u);
        memcpy(regular + regular_len, path, path_len);
        regular_len = (uint16_t)(regular_len + path_len);
        rc = eip_build_cip_req(cip, sizeof cip, 0x54, cm, 4, regular, regular_len, &cip_len);
        rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
        rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
        rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
        Expect(rc == EIP_OK && status == 0, "adapter 16-bit forward open");
        Expect(eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b) == EIP_OK &&
                   o2t_id == 0x33333333u,
               "adapter 16-bit connection id");
    }
}

/* Regular Forward_Open (0x54) the way a scanner such as the YRC1000 sends it.
 *
 * Field order as in TestAdapter's regular open; 0x0A / 0x0E are tick and ticks. conn_type is the
 * T->O connection type (1 multicast, 2 point-to-point). t2o_request is the T->O id the originator
 * proposes. o2t_header adds the 4-byte run/idle header to the O->T size. The path has no config
 * assembly (instance 0): O->T 110, T->O 101, each 4 data bytes. */
static int RobotOpen(EipAdapter *adapter, uint32_t *session, uint8_t conn_type, uint16_t serial,
                     uint32_t t2o_request, int o2t_header, uint8_t *response, uint16_t *response_len)
{
    uint8_t path[16];
    uint16_t path_len = 0;
    uint8_t req[96];
    uint16_t n = 0;
    uint8_t cip[160];
    uint16_t cip_len = 0;
    uint8_t rr[320];
    uint16_t rr_len = 0;
    const uint8_t cm[4] = {0x20, 0x06, 0x24, 0x01};
    uint16_t o2t_param = (uint16_t)((2u + (o2t_header ? 4u : 0u) + 4u) | (2u << 13));
    uint16_t t2o_param = (uint16_t)((2u + 4u) | ((uint16_t)conn_type << 13));
    int rc = eip_build_assembly_path(path, sizeof path, &path_len, 0, 110, 101);

    req[n++] = 0x0A;
    req[n++] = 0x0E;
    eip_store_le32(req + n, 0); /* O->T id: the target chooses */
    n = (uint16_t)(n + 4u);
    eip_store_le32(req + n, t2o_request);
    n = (uint16_t)(n + 4u);
    eip_store_le16(req + n, serial);
    n = (uint16_t)(n + 2u);
    eip_store_le16(req + n, 0x0044); /* Yaskawa */
    n = (uint16_t)(n + 2u);
    eip_store_le32(req + n, 0x00ABCDEFu);
    n = (uint16_t)(n + 4u);
    req[n++] = 1; /* x8 -> 200 ms at 25 ms */
    req[n++] = 0;
    req[n++] = 0;
    req[n++] = 0;
    eip_store_le32(req + n, 25000);
    n = (uint16_t)(n + 4u);
    eip_store_le16(req + n, o2t_param);
    n = (uint16_t)(n + 2u);
    eip_store_le32(req + n, 25000);
    n = (uint16_t)(n + 4u);
    eip_store_le16(req + n, t2o_param);
    n = (uint16_t)(n + 2u);
    req[n++] = 0x01;
    req[n++] = (uint8_t)(path_len / 2u);
    memcpy(req + n, path, path_len);
    n = (uint16_t)(n + path_len);
    rc |= eip_build_cip_req(cip, sizeof cip, 0x54, cm, 4, req, n, &cip_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return RoundTrip(adapter, session, EIP_CMD_SEND_RR, rr, rr_len, response, response_len);
}

/* An adapter shaped like a FEN20-4DIP-4DXP, driven the way a robot scanner drives it. Assembly 110
 * is implicit-only, T->O is multicast or point-to-point, and O->T may or may not carry a run/idle
 * header. A 1 kHz clock and no timeout floor make the timing exact: 25 ms RPI, 200 ms timeout. */
static void TestRobotShape(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {192, 168, 1, 91};
    const uint8_t mask[4] = {255, 255, 255, 0};
    const uint8_t group[4] = {239, 192, 12, 64};
    uint8_t mc[4];
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t body[4] = {0x01, 0x00, 0x00, 0x00};
    EipEncapView view;
    const uint8_t *item = 0;
    uint16_t item_len = 0;
    uint8_t service = 0;
    uint8_t status = 0;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    uint32_t o2t_id = 0;
    uint32_t t2o_id = 0;
    uint32_t api_a = 0;
    uint32_t api_b = 0;
    uint8_t packet[64];
    uint16_t packet_len = 0;
    uint8_t dest[4];
    uint16_t dest_port = 0;
    uint8_t sock_ip[4];
    uint16_t sock_port = 0;
    const uint8_t t2o_data[4] = {0x5A, 0, 0, 0};
    const uint8_t o2t_data[4] = {0x00, 0x00, 0x08, 0}; /* word 1 bit 3 = red */
    uint8_t copied[8];
    uint16_t copied_len = sizeof copied;
    int rc;

    /* 192.168.1.91/24: host 91 gives 239.192.1.0 + 90 x 32 = 239.192.12.64. */
    eip_adapter_multicast_address(ip, mask, mc);
    Expect(memcmp(mc, group, 4) == 0, "cip multicast address");

    eip_adapter_init(&adapter, ip, 0x21F02F);
    eip_adapter_set_clock(&adapter, 1000);
    eip_adapter_set_netmask(&adapter, mask);
    eip_adapter_set_timeout_floor_ms(&adapter, 0);
    eip_adapter_set_identity(&adapter, 48, 12, 1234, 3, 3, "FEN20-4DIP-4DXP");
    eip_adapter_clear_assemblies(&adapter);
    Expect(eip_adapter_add_assembly(&adapter, 101, EIP_ASM_PRODUCED, 4, "T2O", t2o_data) == EIP_OK &&
               eip_adapter_add_assembly(&adapter, 110, EIP_ASM_CONSUMED, 4, "O2T", 0) == EIP_OK,
           "robot assemblies");
    Expect(eip_adapter_add_assembly(&adapter, 110, EIP_ASM_CONSUMED, 4, "dup", 0) == EIP_ERR_ARG,
           "duplicate assembly rejected");

    rc = RoundTrip(&adapter, &session, EIP_CMD_REGISTER, body, 4, response, &response_len);
    Expect(rc == EIP_OK && session != 0, "robot register");
    Expect(eip_adapter_set_implicit_only(&adapter, 110, 1) == EIP_OK, "110 implicit only");
    {
        uint8_t path[16];
        uint16_t path_len = 0;
        uint8_t cip[32];
        uint16_t cip_len = 0;
        uint8_t rr[96];
        uint16_t rr_len = 0;
        uint8_t svc = 0;
        uint8_t st = 0;
        uint16_t ext = 0;
        const uint8_t *d = 0;
        uint16_t dl = 0;
        eip_put_logical(path, sizeof path, &path_len, 0x20, 4);
        eip_put_logical(path, sizeof path, &path_len, 0x24, 110);
        eip_put_logical(path, sizeof path, &path_len, 0x30, 3);
        rc = eip_build_cip_req(cip, sizeof cip, EIP_CIP_GET_ATTRIBUTE, path, path_len, 0, 0, &cip_len);
        rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
        rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
        rc |= CipFromReply(response, response_len, &svc, &st, &ext, &d, &dl);
        Expect(rc == EIP_OK && st == 0x16, "110 explicit read -> 0x16");
    }

    /* Exclusive owner, multicast T->O (scan-list default). */
    rc = RobotOpen(&adapter, &session, 1, 0x101, 0x77777777u, 1, response, &response_len);
    Expect(rc == EIP_OK, "robot multicast open send");
    rc = CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0, "robot multicast open status");
    Expect(eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b) == EIP_OK &&
               o2t_id != 0 && t2o_id != 0x77777777u && api_b == 25000,
           "robot multicast ids and api");
    Expect(eip_parse_encap(response, response_len, &view) == EIP_OK &&
               eip_cpf_find(view.body + 6, (uint16_t)(view.length - 6u), EIP_ITEM_SOCK_T2O, &item,
                            &item_len) == EIP_OK &&
               eip_parse_sockaddr(item, item_len, sock_ip, &sock_port) == EIP_OK &&
               memcmp(sock_ip, group, 4) == 0 && sock_port == 2222,
           "robot multicast t2o sockaddr item");
    rc = eip_adapter_build_output(&adapter, 10000, packet, sizeof packet, &packet_len, dest, &dest_port);
    Expect(rc == EIP_OK && memcmp(dest, group, 4) == 0 && dest_port == 2222 && adapter.t2o_multicast,
           "robot produces to group");
    /* The open was at tick 5000, so the send at 10000 was over an interval late and restarted the
     * schedule from now: next due 10025. The late send at 10027 then keeps to the grid (10050),
     * so wake-up jitter does not stretch the average RPI. */
    Expect(eip_adapter_io_due(&adapter, 10024) == 0 && eip_adapter_io_due(&adapter, 10025) == 1,
           "robot 25 ms rpi");
    rc = eip_adapter_build_output(&adapter, 10027, packet, sizeof packet, &packet_len, dest, &dest_port);
    Expect(rc == EIP_OK && adapter.next_tx_tick == 10050u, "rpi schedule anchored");

    rc = eip_build_io_packet(packet, sizeof packet, &packet_len, o2t_id, 1, 1, 1, 1, o2t_data, 4);
    Expect(rc == EIP_OK && eip_adapter_take_input(&adapter, packet, packet_len, 20000) == EIP_OK &&
               adapter.run == 1,
           "robot o2t consume with run header");
    Expect(eip_adapter_copy(&adapter, 110, copied, &copied_len) == EIP_OK && copied_len == 4 &&
               memcmp(copied, o2t_data, 4) == 0,
           "robot o2t data");
    eip_adapter_check_timeout(&adapter, 20199);
    Expect(adapter.io_open == 1, "robot open at 199 ms");
    eip_adapter_check_timeout(&adapter, 20200);
    Expect(adapter.io_open == 0 && adapter.timeouts == 1 && adapter.t2o_multicast == 0,
           "robot timeout at 200 ms");

    /* Point-to-point T->O keeps the originator's connection id and sends to the peer. */
    rc = RobotOpen(&adapter, &session, 2, 0x102, 0x77777777u, 1, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0, "robot p2p open status");
    Expect(eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b) == EIP_OK &&
               t2o_id == 0x77777777u,
           "robot p2p keeps t2o id");
    Expect(eip_parse_encap(response, response_len, &view) == EIP_OK &&
               eip_cpf_find(view.body + 6, (uint16_t)(view.length - 6u), EIP_ITEM_SOCK_T2O, &item,
                            &item_len) != EIP_OK,
           "robot p2p has no t2o sockaddr");
    rc = eip_adapter_build_output(&adapter, 30000, packet, sizeof packet, &packet_len, dest, &dest_port);
    Expect(rc == EIP_OK && dest[0] == 172 && dest[3] == 199 && dest_port == 2222, "robot p2p destination");

    /* Modeless O->T (no run/idle header) is also accepted. */
    rc = RobotOpen(&adapter, &session, 2, 0x102, 0x77777777u, 0, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0 && adapter.o2t_header == 0, "robot modeless o2t");
}

/* One request to instance 1 of class_id. attribute 0 leaves the attribute segment out, for
 * Get_Attributes_All (service 0x01). A Set sends four zero bytes. */
static int NetGet(EipAdapter *adapter, uint32_t *session, uint8_t service, uint16_t class_id,
                  uint16_t attribute, uint8_t *status, const uint8_t **data, uint16_t *data_len,
                  uint8_t *response)
{
    uint8_t path[16];
    uint16_t path_len = 0;
    uint8_t cip[64];
    uint16_t cip_len = 0;
    uint8_t rr[128];
    uint16_t rr_len = 0;
    uint16_t response_len = 0;
    uint8_t svc = 0;
    uint16_t extended = 0;
    const uint8_t one[4] = {0, 0, 0, 0};
    int rc = eip_put_logical(path, sizeof path, &path_len, 0x20, class_id);

    rc |= eip_put_logical(path, sizeof path, &path_len, 0x24, 1);
    if (attribute != 0) {
        rc |= eip_put_logical(path, sizeof path, &path_len, 0x30, attribute);
    }
    rc |= eip_build_cip_req(cip, sizeof cip, service, path, path_len, one,
                            (service == EIP_CIP_SET_ATTRIBUTE) ? 4u : 0u, &cip_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(adapter, session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return CipFromReply(response, response_len, &svc, status, &extended, data, data_len);
}

/* TCP/IP Interface (0xF5) and Ethernet Link (0xF6) reads. The layouts copy a real FEN20; see the
 * note above TcpIpAttr in src/eip_adapter.c. */
static void TestNetObjects(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {192, 168, 1, 91};
    const uint8_t gw[4] = {192, 168, 1, 1};
    const uint8_t mac[6] = {0x94, 0x51, 0xDC, 0x21, 0xF0, 0x2F};
    /* Attr 5: IP, mask, gateway as little-endian UDINTs, DNS 1 and 2 zero. The reply adds a
     * 2-byte domain length. */
    const uint8_t want_cfg[20] = {0x5B, 0x01, 0xA8, 0xC0, 0x00, 0xFF, 0xFF, 0xFF, 0x01, 0x01, 0xA8, 0xC0,
                                  0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t body[4] = {0x01, 0x00, 0x00, 0x00};
    uint8_t status = 0xFF;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    int rc;

    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_set_interface(&adapter, mac, gw, 0, "test-adapter");
    eip_adapter_set_link(&adapter, 1, 100, 1, 1);
    RoundTrip(&adapter, &session, EIP_CMD_REGISTER, body, 4, response, &response_len);

    /* Speed, flags, MAC. Flags 0x2F: link up, full duplex, negotiated (3 at bit 2), and bit 5. */
    rc = NetGet(&adapter, &session, 0x01, 0xF6, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 14 && eip_load_le32(data) == 100 &&
               eip_load_le32(data + 4) == 0x2Fu && memcmp(data + 8, mac, 6) == 0,
           "ethernet link get all");
    rc = NetGet(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF5, 5, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 22 && memcmp(data, want_cfg, 20) == 0 &&
               eip_load_le16(data + 20) == 0,
           "tcpip interface configuration");
    /* Attr 4: 2-word path to Ethernet Link instance 3, the "internal" port. */
    rc = NetGet(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF5, 4, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 6 && data[2] == 0x20 && data[3] == 0xF6 && data[5] == 3,
           "tcpip physical link path");
    rc = NetGet(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF5, 6, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 14 && eip_load_le16(data) == 12 &&
               memcmp(data + 2, "test-adapter", 12) == 0,
           "tcpip host name");
    rc = NetGet(&adapter, &session, 0x01, 0xF5, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 4 + 4 + 4 + 6 + 22 + 14, "tcpip get all");
    rc = NetGet(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF6, 10, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 5 && data[0] == 4 && memcmp(data + 1, "ETH1", 4) == 0,
           "ethernet link label");
    rc = NetGet(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF6, 7, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 1 && data[0] == 2, "ethernet link type");
    rc = NetGet(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 3, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x0E, "tcpip set rejected");
}

/* Regression from a real capture. The Micro850 names config instance 106 and sends no config data,
 * so the open is refused (0x01 / 0x0129) until a size-0 config assembly exists, and then accepted.
 * Multiplier 0 at a 50 ms RPI gives 200 ticks on a 1 kHz clock. */
static void TestMicro850Open(void)
{
    static EipAdapter adapter;
    /* Forward_Open captured from a Micro850 (172.16.82.203) on 2026-10-31: config 106, O->T 110, T->O 101.
     *
     * Bytes: service 0x54, Connection Manager path, tick 04, ticks BC, O->T id 0, T->O id
     * 0x4AC65D2F, serial 0x00CB, vendor 1, originator serial, multiplier 0 and 3 reserved, then
     * RPI 50 ms and parameters 0x480A (size 10: count, run/idle header, 4 data) for O->T, RPI
     * 50 ms and 0x4806 (size 6: count, 4 data, no header) for T->O, transport 01, path of 9 words:
     * electronic key 34 04 with 8 zero bytes, then 20 04 24 6A 2C 6E 2C 65. */
    static const uint8_t fo[] = {
        0x54, 0x02, 0x20, 0x06, 0x24, 0x01, 0x04, 0xbc, 0x00, 0x00, 0x00, 0x00, 0x2f, 0x5d, 0xc6, 0x4a,
        0xcb, 0x00, 0x01, 0x00, 0x81, 0xc4, 0xb7, 0x70, 0x00, 0x00, 0x00, 0x00, 0x50, 0xc3, 0x00, 0x00,
        0x0a, 0x48, 0x50, 0xc3, 0x00, 0x00, 0x06, 0x48, 0x01, 0x09, 0x34, 0x04, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x20, 0x04, 0x24, 0x6a, 0x2c, 0x6e, 0x2c, 0x65};
    const uint8_t ip[4] = {172, 16, 82, 148};
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t body[4] = {0x01, 0x00, 0x00, 0x00};
    uint8_t rr[160];
    uint16_t rr_len = 0;
    uint8_t service = 0, status = 0xFF;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    uint32_t o2t_id = 0, t2o_id = 0, api_a = 0, api_b = 0;
    int rc;

    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_set_clock(&adapter, 1000);
    eip_adapter_set_timeout_floor_ms(&adapter, 0);
    eip_adapter_clear_assemblies(&adapter);
    eip_adapter_add_assembly(&adapter, 101, EIP_ASM_PRODUCED, 4, "T2O", 0);
    eip_adapter_add_assembly(&adapter, 110, EIP_ASM_CONSUMED, 4, "O2T", 0);
    rc = RoundTrip(&adapter, &session, EIP_CMD_REGISTER, body, 4, response, &response_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 0, fo, sizeof fo, 0, 0, 0, &rr_len);
    rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0x01 && extended == 0x0129, "micro850 rejected without config 106");

    Expect(eip_adapter_add_assembly(&adapter, 106, EIP_ASM_CONFIG, 0, "Config", 0) == EIP_OK &&
               eip_adapter_set_implicit_only(&adapter, 106, 1) == EIP_OK,
           "config 106 added");
    rc = RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0, "micro850 forward open accepted");
    Expect(eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b) == EIP_OK &&
               t2o_id == 0x4ac65d2fu && api_a == 50000 && api_b == 50000 && adapter.timeout_ticks == 200 &&
               adapter.o2t_header == 1 && adapter.t2o_header == 0 && adapter.t2o_multicast == 0,
           "micro850 connection parameters");
}

/* NetGet for any instance and request body. attribute < 0 leaves the attribute segment out. */
static int CipRaw(EipAdapter *adapter, uint32_t *session, uint8_t service, uint16_t class_id, uint16_t instance,
                  int attribute, const uint8_t *body, uint16_t body_len, uint8_t *status, const uint8_t **data,
                  uint16_t *data_len, uint8_t *response)
{
    uint8_t path[16];
    uint16_t path_len = 0;
    uint8_t cip[160];
    uint16_t cip_len = 0;
    uint8_t rr[256];
    uint16_t rr_len = 0;
    uint16_t response_len = 0;
    uint8_t svc = 0;
    uint16_t extended = 0;
    int rc = eip_put_logical(path, sizeof path, &path_len, 0x20, class_id);

    rc |= eip_put_logical(path, sizeof path, &path_len, 0x24, instance);
    if (attribute >= 0) {
        rc |= eip_put_logical(path, sizeof path, &path_len, 0x30, (uint32_t)attribute);
    }
    rc |= eip_build_cip_req(cip, sizeof cip, service, path, path_len, body, body_len, &cip_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(adapter, session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return CipFromReply(response, response_len, &svc, status, &extended, data, data_len);
}

/* Fake application callbacks: record what reached them and accept it. TestNetConfigSet and
 * TestMultipleService both use them. */
static int g_set_calls;
static uint16_t g_set_attr;
static EipInterfaceConfig g_set_cfg;
static int g_reset_type = -1;

static uint8_t TestSetIf(void *ctx, uint16_t attribute, const EipInterfaceConfig *cfg)
{
    (void)ctx;
    g_set_calls++;
    g_set_attr = attribute;
    g_set_cfg = *cfg;
    return 0;
}

static uint8_t TestReset(void *ctx, uint8_t type)
{
    (void)ctx;
    g_reset_type = type;
    return 0;
}

/* 22-byte attr 5 body: IP, mask, gateway as little-endian UDINTs. DNS and domain length stay 0. */
static void PutCfg(uint8_t *b, const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4])
{
    uint8_t i;
    memset(b, 0, 22);
    for (i = 0; i < 4; i++) {
        b[i] = ip[3 - i];
        b[4 + i] = mask[3 - i];
        b[8 + i] = gw[3 - i];
    }
}

/* Set_Attribute_Single on TCP/IP Interface attrs 3, 5, 6, and Identity Reset. Pins the status of
 * each refusal and that only accepted values reach the callback: a bad address applied to a live
 * device can take it off the network. */
static void TestNetConfigSet(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {172, 16, 82, 148};
    const uint8_t mask[4] = {255, 255, 255, 0};
    const uint8_t gw[4] = {172, 16, 82, 1};
    const uint8_t new_ip[4] = {192, 168, 1, 91};
    const uint8_t new_gw[4] = {192, 168, 1, 1};
    const uint8_t bcast[4] = {192, 168, 1, 255};
    const uint8_t mcast[4] = {239, 1, 1, 1};
    const uint8_t badmask[4] = {255, 0, 255, 0};
    const uint8_t offnet_gw[4] = {10, 0, 0, 1};
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t body[64];
    uint8_t status = 0xFF;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    uint8_t reg[4] = {0x01, 0x00, 0x00, 0x00};
    int rc;

    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_set_netmask(&adapter, mask);
    eip_adapter_set_interface(&adapter, 0, gw, 2, "test-adapter");
    RoundTrip(&adapter, &session, EIP_CMD_REGISTER, reg, 4, response, &response_len);

    /* No callback: not settable, no 0x10 capability. */
    eip_store_le32(body, 0);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 3, body, 4, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x0E, "netcfg: no callback -> 0x0E");
    rc = CipRaw(&adapter, &session, EIP_CIP_RESET, 0x01, 1, -1, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x08, "netcfg: reset without callback -> 0x08");

    eip_adapter_set_callbacks(&adapter, TestSetIf, TestReset, 0);
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF5, 1, 2, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && (eip_load_le32(data) & 0x10u) != 0, "netcfg: capability settable bit");

    /* attr 5 while DHCP -> object state conflict */
    PutCfg(body, new_ip, mask, new_gw);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 5, body, 22, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x0C && g_set_calls == 0, "netcfg: attr 5 under DHCP -> 0x0C");

    /* BOOTP refused, static accepted */
    eip_store_le32(body, 1);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 3, body, 4, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x09, "netcfg: BOOTP -> 0x09");
    eip_store_le32(body, 0);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 3, body, 4, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && g_set_calls == 1 && g_set_attr == 3 && g_set_cfg.control == 0 &&
               adapter.ip_config_control == 0,
           "netcfg: set static");
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 3, body, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x13, "netcfg: short attr 3 -> 0x13");

    /* invalid addresses */
    PutCfg(body, bcast, mask, new_gw);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 5, body, 22, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x09, "netcfg: broadcast ip -> 0x09");
    PutCfg(body, mcast, mask, gw);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 5, body, 22, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x09, "netcfg: multicast ip -> 0x09");
    PutCfg(body, new_ip, badmask, new_gw);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 5, body, 22, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x09, "netcfg: non-contiguous mask -> 0x09");
    PutCfg(body, new_ip, mask, offnet_gw);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 5, body, 22, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x09 && g_set_calls == 1, "netcfg: off-subnet gateway -> 0x09");
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 5, body, 20, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x13, "netcfg: short attr 5 -> 0x13");

    /* valid address with a domain name "cell" */
    PutCfg(body, new_ip, mask, new_gw);
    body[20] = 4;
    memcpy(body + 22, "cell", 4);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 5, body, 26, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && g_set_calls == 2 && g_set_attr == 5 &&
               memcmp(g_set_cfg.ip, new_ip, 4) == 0 && memcmp(g_set_cfg.gateway, new_gw, 4) == 0 &&
               strcmp(g_set_cfg.domain, "cell") == 0,
           "netcfg: set address");
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF5, 1, 5, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data[0] == 91 && data[3] == 192 && data_len == 26 &&
               memcmp(data + 22, "cell", 4) == 0,
           "netcfg: attr 5 reads back");

    /* host name: valid, odd length with pad byte, invalid characters */
    body[0] = 5;
    body[1] = 0;
    memcpy(body + 2, "bcn-1", 5);
    body[7] = 0;
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 6, body, 8, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && strcmp(adapter.host_name, "bcn-1") == 0, "netcfg: set host name");
    memcpy(body + 2, "bc n!", 5);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 6, body, 7, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x09, "netcfg: bad host name -> 0x09");

    /* other attributes and the Ethernet Link object stay read-only */
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 1, body, 4, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x0E, "netcfg: attr 1 read-only");
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF6, 1, 6, body, 4, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x0E, "netcfg: F6 read-only");

    /* back to DHCP */
    eip_store_le32(body, 2);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 3, body, 4, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && adapter.ip_config_control == 2, "netcfg: set DHCP");

    /* Identity Reset */
    rc = CipRaw(&adapter, &session, EIP_CIP_RESET, 0x01, 1, -1, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && g_reset_type == 0, "reset type 0 (no data)");
    body[0] = 1;
    rc = CipRaw(&adapter, &session, EIP_CIP_RESET, 0x01, 1, -1, body, 1, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && g_reset_type == 1, "reset type 1");
    body[0] = 2;
    rc = CipRaw(&adapter, &session, EIP_CIP_RESET, 0x01, 1, -1, body, 1, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x20 && g_reset_type == 1, "reset type 2 -> 0x20");
}

/* ---- Class 1 input ------------------------------------------------------ */

/* One O->T packet from src. b2 lands in data byte 2 and marks which packet's data got through.
 * seq fills both the 32-bit and the 16-bit count. trim cuts bytes off the end for a short packet. */
static int Io(EipAdapter *a, uint32_t id, uint32_t seq, int run, uint8_t b2, const uint8_t src[4], uint32_t now,
              uint16_t trim)
{
    uint8_t pkt[64];
    uint16_t len = 0;
    const uint8_t data[4] = {0, 0, b2, 0};
    eip_build_io_packet(pkt, sizeof pkt, &len, id, seq, (uint16_t)seq, 1, run, data, 4);
    return eip_adapter_take_input_from(a, pkt, (uint16_t)(len - trim), now, src);
}

/* eip_adapter_take_input_from: a bad packet must change neither the consumed data nor the
 * watchdog, or a stray sender could drive outputs or hold a dead connection open. Also pins when
 * the consumed assembly clears: idle, timeout and close. RobotOpen gives a 200-tick timeout. */
static void TestInputHardening(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {172, 16, 82, 148};
    const uint8_t peer[4] = {172, 16, 82, 199};   /* RoundTrip()'s originator address */
    const uint8_t other[4] = {172, 16, 82, 66};
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t body[4] = {0x01, 0x00, 0x00, 0x00};
    uint8_t service = 0, status = 0xFF;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    uint32_t o2t_id = 0, t2o_id = 0, api_a = 0, api_b = 0;
    uint8_t d[8];
    uint16_t dl;
    int rc;

    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_set_clock(&adapter, 1000);
    eip_adapter_set_timeout_floor_ms(&adapter, 0);
    eip_adapter_clear_assemblies(&adapter);
    eip_adapter_add_assembly(&adapter, 101, EIP_ASM_PRODUCED, 4, "T2O", 0);
    eip_adapter_add_assembly(&adapter, 110, EIP_ASM_CONSUMED, 4, "O2T", 0);
    RoundTrip(&adapter, &session, EIP_CMD_REGISTER, body, 4, response, &response_len);
    rc = RobotOpen(&adapter, &session, 2, 0x201, 0x55555555u, 1, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    rc |= eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b);
    Expect(rc == EIP_OK && status == 0, "hardening: open");
    /* Byte 2 of the consumed assembly, read back through the normal API. */
#define O2T_BYTE2() (dl = sizeof d, eip_adapter_copy(&adapter, 110, d, &dl), d[2])

    Expect(Io(&adapter, o2t_id, 10, 1, 0x08, peer, 1000, 0) == EIP_OK && O2T_BYTE2() == 0x08 && adapter.run == 1,
           "hardening: valid packet accepted");
    Expect(Io(&adapter, o2t_id, 10, 1, 0x20, peer, 1010, 0) != EIP_OK && O2T_BYTE2() == 0x08 &&
               adapter.rx_dropped_seq == 1 && adapter.last_rx_tick == 1000,
           "hardening: duplicate sequence dropped, timer not refreshed");
    Expect(Io(&adapter, o2t_id, 9, 1, 0x20, peer, 1020, 0) != EIP_OK && O2T_BYTE2() == 0x08 && adapter.rx_dropped_seq == 2,
           "hardening: older sequence dropped");
    Expect(Io(&adapter, o2t_id, 11, 1, 0x20, other, 1030, 0) != EIP_OK && O2T_BYTE2() == 0x08 &&
               adapter.rx_dropped_source == 1 && adapter.last_rx_tick == 1000,
           "hardening: unexpected source dropped");
    Expect(Io(&adapter, o2t_id + 1, 11, 1, 0x20, peer, 1040, 0) != EIP_OK && adapter.rx_dropped_bad == 1,
           "hardening: wrong connection id dropped");
    Expect(Io(&adapter, o2t_id, 11, 1, 0x20, peer, 1050, 1) != EIP_OK && adapter.last_rx_tick == 1000 &&
               adapter.rx_dropped_bad == 2,
           "hardening: short packet does not refresh the timer");
    Expect(Io(&adapter, o2t_id, 11, 0, 0x20, peer, 1060, 0) == EIP_OK && O2T_BYTE2() == 0x00 && adapter.run == 0 &&
               adapter.rx_idle == 1 && adapter.last_rx_tick == 1060,
           "hardening: idle keeps the connection but zeroes the data");
    Expect(Io(&adapter, o2t_id, 12, 1, 0x40, peer, 1070, 0) == EIP_OK && O2T_BYTE2() == 0x40 && adapter.run == 1,
           "hardening: run again delivers data");
    Expect(Io(&adapter, o2t_id, 12 + 0x80000000u, 1, 0x02, peer, 1080, 0) != EIP_OK && O2T_BYTE2() == 0x40,
           "hardening: sequence half a range away is treated as old");
    Expect(Io(&adapter, o2t_id, 0, 1, 0x02, peer, 1090, 1) != EIP_OK, "hardening: (short) ignored");
    /* timeout -> connection closed and the consumed data cleared */
    eip_adapter_check_timeout(&adapter, 1070 + 200);
    Expect(adapter.io_open == 0 && O2T_BYTE2() == 0x00, "hardening: timeout clears the consumed assembly");

    /* new connection: sequence tracking restarts, wrap-around 0xFFFFFFFF -> 1 is "newer" */
    rc = RobotOpen(&adapter, &session, 2, 0x202, 0x56565656u, 1, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    rc |= eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b);
    Expect(rc == EIP_OK && status == 0 && O2T_BYTE2() == 0x00, "hardening: reopen starts with cleared data");
    Expect(Io(&adapter, o2t_id, 0xFFFFFFFEu, 1, 0x04, peer, 2000, 0) == EIP_OK && O2T_BYTE2() == 0x04,
           "hardening: first packet of a new connection accepted (any sequence)");
    Expect(Io(&adapter, o2t_id, 0xFFFFFFFFu, 1, 0x08, peer, 2010, 0) == EIP_OK &&
               Io(&adapter, o2t_id, 1, 1, 0x10, peer, 2020, 0) == EIP_OK && O2T_BYTE2() == 0x10,
           "hardening: 32-bit sequence wrap-around accepted");
    /* Forward_Close clears the data too */
    {
        uint8_t path[16], cip[160], rr[320];
        uint16_t path_len = 0, cip_len = 0, rr_len = 0;
        eip_build_assembly_path(path, sizeof path, &path_len, 0, 110, 101);
        rc = eip_build_forward_close_request(cip, sizeof cip, &cip_len, 0x0A, 0x0E, 0x202, 0x0044, 0x00ABCDEFu, path, path_len);
        rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
        rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
        rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
        Expect(rc == EIP_OK && status == 0 && adapter.io_open == 0 && O2T_BYTE2() == 0x00,
               "hardening: Forward_Close clears the consumed assembly");
    }
    /* legacy entry point (no source check) still works */
    rc = RobotOpen(&adapter, &session, 2, 0x203, 0x57575757u, 1, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    rc |= eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b);
    {
        uint8_t pkt[64];
        uint16_t len = 0;
        const uint8_t dd[4] = {0, 0, 0x01, 0};
        eip_build_io_packet(pkt, sizeof pkt, &len, o2t_id, 1, 1, 1, 1, dd, 4);
        Expect(rc == EIP_OK && eip_adapter_take_input(&adapter, pkt, len, 3000) == EIP_OK && O2T_BYTE2() == 0x01,
               "hardening: eip_adapter_take_input without source still accepted");
    }
#undef O2T_BYTE2
}

/* ---- smaller units ------------------------------------------------------ */

/* Strict dotted quad: four parts of 1 to 3 digits, each up to 255, nothing after the last. */
static void TestParseIpv4(void)
{
    uint8_t ip[4] = {9, 9, 9, 9};
    Expect(eip_parse_ipv4("192.168.1.10", ip) == EIP_OK && ip[0] == 192 && ip[1] == 168 && ip[2] == 1 &&
               ip[3] == 10, "ipv4 parse");
    Expect(eip_parse_ipv4("0.0.0.0", ip) == EIP_OK && ip[0] == 0 && ip[3] == 0, "ipv4 zero");
    Expect(eip_parse_ipv4("255.255.255.255", ip) == EIP_OK && ip[0] == 255, "ipv4 broadcast");
    Expect(eip_parse_ipv4("256.1.1.1", ip) == EIP_ERR_ARG, "ipv4 octet range");
    Expect(eip_parse_ipv4("1.2.3", ip) == EIP_ERR_ARG, "ipv4 three parts");
    Expect(eip_parse_ipv4("1.2.3.4.5", ip) == EIP_ERR_ARG, "ipv4 five parts");
    Expect(eip_parse_ipv4("1.2.3.4 ", ip) == EIP_ERR_ARG, "ipv4 trailing space");
    Expect(eip_parse_ipv4("1..3.4", ip) == EIP_ERR_ARG, "ipv4 empty part");
    Expect(eip_parse_ipv4("0001.2.3.4", ip) == EIP_ERR_ARG, "ipv4 too many digits");
    Expect(eip_parse_ipv4("", ip) == EIP_ERR_ARG && eip_parse_ipv4(0, ip) == EIP_ERR_ARG, "ipv4 empty");
}

/* All assemblies share EIP_ADAPTER_DATA_POOL. Pins an exact fit, the overflow refusal, that a
 * size-0 config point needs no space, and that clearing returns all of it. */
static void TestAssemblyPool(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {10, 0, 0, 2};
    uint8_t big[300];
    uint8_t out[300];
    uint16_t out_len = sizeof out;
    int rc;
    int i;

    for (i = 0; i < (int)sizeof big; i++) {
        big[i] = (uint8_t)i;
    }
    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_clear_assemblies(&adapter);
    rc = eip_adapter_add_assembly(&adapter, 100, EIP_ASM_PRODUCED, 300, "Big", big);
    Expect(rc == EIP_OK, "pool: 300-byte assembly (old limit was 16)");
    rc = eip_adapter_copy(&adapter, 100, out, &out_len);
    Expect(rc == EIP_OK && out_len == 300 && memcmp(out, big, 300) == 0, "pool: initial data kept");
    rc = eip_adapter_add_assembly(&adapter, 150, EIP_ASM_CONSUMED, EIP_ADAPTER_DATA_POOL - 300, "Rest", 0);
    Expect(rc == EIP_OK, "pool: fill exactly");
    rc = eip_adapter_add_assembly(&adapter, 151, EIP_ASM_CONSUMED, 1, "Over", 0);
    Expect(rc == EIP_ERR_TOO_BIG, "pool: overflow refused");
    rc = eip_adapter_add_assembly(&adapter, 152, EIP_ASM_CONFIG, 0, "Cfg", 0);
    Expect(rc == EIP_OK, "pool: size-0 config point fits a full pool");
    out_len = sizeof out;
    rc = eip_adapter_copy(&adapter, 100, out, &out_len);
    Expect(rc == EIP_OK && memcmp(out, big, 300) == 0, "pool: neighbours do not overlap");
    eip_adapter_clear_assemblies(&adapter);
    Expect(eip_adapter_add_assembly(&adapter, 100, EIP_ASM_PRODUCED, EIP_ADAPTER_DATA_POOL, "All", 0) == EIP_OK,
           "pool: clear returns the space");
}

/* O->T with only a Connected Address item (no 32-bit sequence number). */
static int IoNoSeq32(EipAdapter *a, uint32_t id, uint16_t seq16, uint8_t b2, const uint8_t src[4], uint32_t now)
{
    uint8_t pkt[64];
    uint16_t n = 0;
    eip_store_le16(pkt + n, 2); n += 2;
    eip_store_le16(pkt + n, EIP_ITEM_CONN_ADDR); n += 2;
    eip_store_le16(pkt + n, 4); n += 2;
    eip_store_le32(pkt + n, id); n += 4;
    eip_store_le16(pkt + n, EIP_ITEM_CONNECTED); n += 2;
    eip_store_le16(pkt + n, 2 + 4 + 4); n += 2;
    eip_store_le16(pkt + n, seq16); n += 2;
    eip_store_le32(pkt + n, 1); n += 4;              /* run/idle header: run */
    pkt[n++] = 0; pkt[n++] = 0; pkt[n++] = b2; pkt[n++] = 0;
    return eip_adapter_take_input_from(a, pkt, n, now, src);
}

/* With no Sequenced Address item the adapter falls back on the 16-bit CIP count. An unchanged count
 * refreshes the watchdog and keeps the old data; a count half the range or more behind is stale. */
static void TestSeq16Fallback(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {172, 16, 82, 148};
    const uint8_t peer[4] = {172, 16, 82, 199};
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t body[4] = {0x01, 0x00, 0x00, 0x00};
    uint8_t service = 0, status = 0xFF;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    uint32_t o2t_id = 0, t2o_id = 0, api_a = 0, api_b = 0;
    uint8_t d[8];
    uint16_t dl;
    int rc;

    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_set_clock(&adapter, 1000);
    eip_adapter_set_timeout_floor_ms(&adapter, 0);
    eip_adapter_clear_assemblies(&adapter);
    eip_adapter_add_assembly(&adapter, 101, EIP_ASM_PRODUCED, 4, "T2O", 0);
    eip_adapter_add_assembly(&adapter, 110, EIP_ASM_CONSUMED, 4, "O2T", 0);
    RoundTrip(&adapter, &session, EIP_CMD_REGISTER, body, 4, response, &response_len);
    rc = RobotOpen(&adapter, &session, 2, 0x301, 0x55555555u, 1, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    rc |= eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b);
    Expect(rc == EIP_OK && status == 0, "seq16: open");
#define O2T_BYTE2() (dl = sizeof d, eip_adapter_copy(&adapter, 110, d, &dl), d[2])
    Expect(IoNoSeq32(&adapter, o2t_id, 5, 0x11, peer, 1000) == EIP_OK && O2T_BYTE2() == 0x11, "seq16: first accepted");
    Expect(IoNoSeq32(&adapter, o2t_id, 5, 0x22, peer, 1010) == EIP_OK && O2T_BYTE2() == 0x11 &&
               adapter.last_rx_tick == 1010,
           "seq16: same count refreshes the watchdog, keeps the data");
    Expect(IoNoSeq32(&adapter, o2t_id, 4, 0x33, peer, 1020) != EIP_OK && O2T_BYTE2() == 0x11 &&
               adapter.last_rx_tick == 1010,
           "seq16: older count dropped");
    Expect(IoNoSeq32(&adapter, o2t_id, 6, 0x44, peer, 1030) == EIP_OK && O2T_BYTE2() == 0x44, "seq16: newer count");
    Expect(IoNoSeq32(&adapter, o2t_id, 0xFFFF, 0x55, peer, 1040) != EIP_OK, "seq16: half a range back is old");
#undef O2T_BYTE2
}

/* ---- Multiple Service Packet (0x0A, Message Router) */

/* One embedded request: service, class / instance / attribute (attribute < 0: none), data. */
static uint16_t Embedded(uint8_t *out, uint16_t cap, uint8_t service, uint16_t class_id, uint16_t instance,
                         int attribute, const uint8_t *data, uint16_t data_len)
{
    uint8_t path[16];
    uint16_t path_len = 0;
    uint16_t len = 0;

    eip_put_logical(path, sizeof path, &path_len, 0x20, class_id);
    eip_put_logical(path, sizeof path, &path_len, 0x24, instance);
    if (attribute >= 0) {
        eip_put_logical(path, sizeof path, &path_len, 0x30, (uint32_t)attribute);
    }
    if (eip_build_cip_req(out, cap, service, path, path_len, data, data_len, &len) != EIP_OK) {
        return 0;
    }
    return len;
}

/* Sends `count` embedded requests (back to back in `reqs`, lengths in `lens`) as one packet to the Message
 * Router instance `instance`. Gives the general status and the reply data (count, offsets, replies). */
static int Multiple(EipAdapter *adapter, uint32_t *session, uint16_t instance, const uint8_t *reqs,
                    const uint16_t *lens, uint16_t count, uint8_t *status, const uint8_t **data, uint16_t *data_len,
                    uint8_t *response)
{
    uint8_t body[600];
    uint8_t path[8];
    uint16_t path_len = 0;
    uint8_t cip[640];
    uint16_t cip_len = 0;
    uint8_t rr[680];
    uint16_t rr_len = 0;
    uint16_t response_len = 0;
    uint16_t used = (uint16_t)(2u + 2u * count);
    uint16_t at = 0;
    uint16_t i;
    uint8_t svc = 0;
    uint16_t extended = 0;
    int rc;

    eip_store_le16(body, count);
    for (i = 0; i < count; i++) {
        eip_store_le16(body + 2u + 2u * i, used);
        memcpy(body + used, reqs + at, lens[i]);
        used = (uint16_t)(used + lens[i]);
        at = (uint16_t)(at + lens[i]);
    }
    rc = eip_put_logical(path, sizeof path, &path_len, 0x20, 2);
    rc |= eip_put_logical(path, sizeof path, &path_len, 0x24, instance);
    rc |= eip_build_cip_req(cip, sizeof cip, 0x0A, path, path_len, body, used, &cip_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(adapter, session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return CipFromReply(response, response_len, &svc, status, &extended, data, data_len);
}

/* Entry i of a Multiple Service Packet reply: its general status and data. */
static int Entry(const uint8_t *data, uint16_t data_len, uint16_t i, uint8_t *status, const uint8_t **edata,
                 uint16_t *elen)
{
    uint16_t count = eip_load_le16(data);
    uint16_t start = eip_load_le16(data + 2u + 2u * i);
    uint16_t end = (uint16_t)((i + 1u < count) ? eip_load_le16(data + 4u + 2u * i) : data_len);
    uint8_t svc = 0;
    uint16_t extended = 0;

    if (i >= count || start > end || end > data_len) {
        return EIP_ERR_PROTOCOL;
    }
    return eip_parse_cip_rsp(data + start, (uint16_t)(end - start), &svc, status, &extended, edata, elen);
}

/* Each embedded request gets the answer it would get alone, and the reply stays a whole, parseable
 * packet when entries fail or overflow. */
static void TestMultipleService(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {10, 168, 41, 5};
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t reg[4] = {0x01, 0x00, 0x00, 0x00};
    uint8_t reqs[560];
    uint16_t lens[48];
    uint16_t at;
    uint8_t status = 0xFF;
    uint8_t est = 0xFF;
    const uint8_t *data = 0;
    const uint8_t *ed = 0;
    uint16_t data_len = 0;
    uint16_t elen = 0;
    uint8_t host[8];
    uint16_t i;
    int ok;
    int rc;

    eip_adapter_init(&adapter, ip, 7);
    eip_adapter_set_identity(&adapter, 65000, 43, 3, 0, 2, "Sample Adapter");
    RoundTrip(&adapter, &session, EIP_CMD_REGISTER, reg, 4, response, &response_len);

    /* Two Gets: vendor and product name. */
    at = 0;
    lens[0] = Embedded(reqs + at, 64, EIP_CIP_GET_ATTRIBUTE, 0x01, 1, 1, 0, 0);
    at = (uint16_t)(at + lens[0]);
    lens[1] = Embedded(reqs + at, 64, EIP_CIP_GET_ATTRIBUTE, 0x01, 1, 7, 0, 0);
    rc = Multiple(&adapter, &session, 1, reqs, lens, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && eip_load_le16(data) == 2, "msp: two gets, general status 0");
    rc = Entry(data, data_len, 0, &est, &ed, &elen);
    Expect(rc == EIP_OK && est == 0 && elen == 2 && eip_load_le16(ed) == 65000, "msp: entry 0 is the vendor id");
    rc = Entry(data, data_len, 1, &est, &ed, &elen);
    Expect(rc == EIP_OK && est == 0 && elen == 15 && ed[0] == 14 && memcmp(ed + 1, "Sample Adapter", 14) == 0,
           "msp: entry 1 is the product name");

    /* A Get and a refused Set (TCP/IP config without a callback): 0x1E, each entry its own status. */
    at = 0;
    lens[0] = Embedded(reqs, 64, EIP_CIP_GET_ATTRIBUTE, 0x01, 1, 1, 0, 0);
    at = lens[0];
    eip_store_le32(host, 2);
    lens[1] = Embedded(reqs + at, 64, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 3, host, 4);
    rc = Multiple(&adapter, &session, 1, reqs, lens, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x1E, "msp: a failed entry gives 0x1E");
    rc = Entry(data, data_len, 0, &est, &ed, &elen);
    Expect(rc == EIP_OK && est == 0, "msp: the get still succeeds");
    rc = Entry(data, data_len, 1, &est, &ed, &elen);
    Expect(rc == EIP_OK && est == 0x0E, "msp: the set answers 0x0E as on its own");

    /* Sets inside a bundle go through the application's callback. */
    g_set_calls = 0;
    eip_adapter_set_callbacks(&adapter, TestSetIf, TestReset, 0);
    eip_store_le16(host, 4);
    memcpy(host + 2, "node", 4);
    lens[0] = Embedded(reqs, 64, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 6, host, 6);
    rc = Multiple(&adapter, &session, 1, reqs, lens, 1, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && g_set_calls == 1 && g_set_attr == 6 &&
               strcmp(g_set_cfg.host_name, "node") == 0,
           "msp: an embedded set reaches the callback");

    /* Connection services and nested packets are refused per entry. */
    {
        uint8_t fo[4] = {0, 0, 0, 0};
        at = 0;
        lens[0] = Embedded(reqs, 64, 0x54, 0x06, 1, -1, fo, sizeof fo);
        at = lens[0];
        lens[1] = Embedded(reqs + at, 64, 0x0A, 0x02, 1, -1, fo, sizeof fo);
        at = (uint16_t)(at + lens[1]);
        lens[2] = Embedded(reqs + at, 64, 0x4E, 0x06, 1, -1, fo, sizeof fo);
        rc = Multiple(&adapter, &session, 1, reqs, lens, 3, &status, &data, &data_len, response);
        ok = rc == EIP_OK && status == 0x1E;
        for (i = 0; i < 3; i++) {
            ok = ok && Entry(data, data_len, i, &est, &ed, &elen) == EIP_OK && est == 0x08;
        }
        Expect(ok, "msp: forward open, nested packet and forward close refused (0x08)");
    }

    /* Another Message Router instance does not exist. */
    lens[0] = Embedded(reqs, 64, EIP_CIP_GET_ATTRIBUTE, 0x01, 1, 1, 0, 0);
    rc = Multiple(&adapter, &session, 2, reqs, lens, 1, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x16, "msp: message router instance 2 -> 0x16");

    /* Malformed: count 0, and an offset past the end. */
    {
        uint8_t cip[32];
        uint8_t path[8];
        uint16_t path_len = 0;
        uint16_t cip_len = 0;
        uint8_t rr[64];
        uint16_t rr_len = 0;
        uint8_t svc = 0;
        uint16_t extended = 0;
        uint8_t bad[2][6] = {{0, 0}, {1, 0, 40, 0, 0x0E, 0}};
        uint16_t bad_len[2] = {2, 6};
        eip_put_logical(path, sizeof path, &path_len, 0x20, 2);
        eip_put_logical(path, sizeof path, &path_len, 0x24, 1);
        ok = 1;
        for (i = 0; i < 2; i++) {
            cip_len = 0;
            rr_len = 0;
            rc = eip_build_cip_req(cip, sizeof cip, 0x0A, path, path_len, bad[i], bad_len[i], &cip_len);
            rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
            rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
            rc |= CipFromReply(response, response_len, &svc, &status, &extended, &data, &data_len);
            ok = ok && rc == EIP_OK && status == 0x13;
        }
        Expect(ok, "msp: count 0 and an offset past the end -> 0x13");
    }

    /* More replies than fit: 40 Get_Attributes_All of the Identity (about 33 bytes each) in a 512-byte reply.
     * The first ones succeed, the rest are answered 0x11, and the reply stays whole. */
    at = 0;
    for (i = 0; i < 40; i++) {
        lens[i] = Embedded(reqs + at, 16, 0x01, 0x01, 1, -1, 0, 0);
        at = (uint16_t)(at + lens[i]);
    }
    rc = Multiple(&adapter, &session, 1, reqs, lens, 40, &status, &data, &data_len, response);
    ok = rc == EIP_OK && status == 0x1E && eip_load_le16(data) == 40 && data_len + 4u <= 512u;
    {
        int good = 0, big = 0;
        for (i = 0; i < 40 && ok; i++) {
            ok = Entry(data, data_len, i, &est, &ed, &elen) == EIP_OK;
            if (est == 0) {
                good++;
            } else if (est == 0x11) {
                big++;
            }
        }
        printf("  msp overflow: %d answered, %d too large, reply %u bytes\n", good, big, (unsigned)(data_len + 4u));
        ok = ok && good > 5 && big > 0 && good + big == 40;
    }
    Expect(ok, "msp: replies that don't fit are answered 0x11, the rest stand");
}

/* The adapter's Forward_Open reply must report each direction's own interval. The originator
 * sends at the O->T API, and the watchdog runs from the O->T RPI, so a slower T->O must not
 * stretch the O->T value. */
static void TestOpenReportsOwnIntervals(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {172, 16, 82, 10};
    const uint8_t body[4] = {1, 0, 0, 0};
    uint32_t session = 0;
    uint8_t response[700];
    uint16_t response_len = 0;
    uint8_t path[32];
    uint16_t path_len = 0;
    uint8_t cip[128];
    uint16_t cip_len = 0;
    uint8_t rr[256];
    uint16_t rr_len = 0;
    EipForwardOpenInfo info;
    uint8_t service = 0;
    uint8_t status = 0;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    uint32_t o2t_id = 0, t2o_id = 0, api_a = 0, api_b = 0;
    int rc;

    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_set_clock(&adapter, 1000);
    rc = RoundTrip(&adapter, &session, EIP_CMD_REGISTER, body, 4, response, &response_len);
    Expect(rc == EIP_OK && session != 0, "api register");

    memset(&info, 0, sizeof info);
    info.priority_tick = 5;
    info.timeout_ticks = 20;
    info.o2t_id = 0x11111111u;
    info.connection_serial = 7;
    info.vendor_id = 1;
    info.originator_serial = 99;
    info.timeout_multiplier = 2;
    info.o2t_rpi_us = 10000;
    info.t2o_rpi_us = 100000;
    info.o2t_params = eip_large_conn_params((uint16_t)(2u + 4u + 8u), 0, 0, 2, 0);
    info.t2o_params = eip_large_conn_params((uint16_t)(2u + 8u), 0, 0, 2, 0);
    info.transport = 0x01;
    rc = eip_build_assembly_path(path, sizeof path, &path_len, 150, 100, 101);
    rc |= eip_build_forward_open_request(cip, sizeof cip, &cip_len, &info, path, path_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    Expect(rc == EIP_OK, "api open send");
    rc = CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0, "api open status");
    Expect(eip_parse_forward_open_success(data, data_len, &o2t_id, &t2o_id, &api_a, &api_b) == EIP_OK,
           "api open body");
    Expect(api_a == 10000u, "O->T API is the O->T RPI");
    Expect(api_b == 100000u, "T->O API is the T->O interval");
}

/* An assembly can be larger than an explicit reply holds. The read must fail with CIP status 0x11
 * and keep the connection: a non-OK return makes eip_server.c drop the TCP client. */
static void TestExplicitReplyTooLarge(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {10, 0, 0, 2};
    const uint8_t body[4] = {1, 0, 0, 0};
    uint32_t session = 0;
    uint8_t response[700];
    uint16_t response_len = 0;
    uint8_t path[16];
    uint16_t path_len = 0;
    uint8_t cip[32];
    uint16_t cip_len = 0;
    uint8_t rr[96];
    uint16_t rr_len = 0;
    uint8_t service = 0;
    uint8_t status = 0;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    int rc;

    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_clear_assemblies(&adapter);
    Expect(eip_adapter_add_assembly(&adapter, 100, EIP_ASM_PRODUCED, 510, "Big", 0) == EIP_OK,
           "oversize: assembly added");
    rc = RoundTrip(&adapter, &session, EIP_CMD_REGISTER, body, 4, response, &response_len);
    Expect(rc == EIP_OK && session != 0, "oversize: register");

    rc = eip_put_logical(path, sizeof path, &path_len, 0x20, 4);
    rc |= eip_put_logical(path, sizeof path, &path_len, 0x24, 100);
    rc |= eip_put_logical(path, sizeof path, &path_len, 0x30, 3);
    rc |= eip_build_cip_req(cip, sizeof cip, EIP_CIP_GET_ATTRIBUTE, path, path_len, 0, 0, &cip_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    Expect(rc == EIP_OK, "oversize: request built");
    rc = RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    Expect(rc == EIP_OK, "oversize: the client is answered, not dropped");
    if (rc == EIP_OK) {
        rc = CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
        Expect(rc == EIP_OK && service == (EIP_CIP_GET_ATTRIBUTE | 0x80u) && status == 0x11,
               "oversize: status 0x11");
    }
}

/* A Forward_Open from vendor 1, serial 99, connection serial 7, with an electronic key for vendor
 * 48, type 12, product 1234, major 3, and the given minor. */
static int OpenWithKey(EipAdapter *adapter, uint32_t *session, uint8_t key_minor, uint8_t *response,
                       uint16_t *response_len)
{
    uint8_t path[32] = {0x34, 0x04, 0x30, 0x00, 0x0C, 0x00, 0xD2, 0x04, 0x03, 0x00};
    uint16_t path_len = 10;
    uint8_t cip[128];
    uint16_t cip_len = 0;
    uint8_t rr[256];
    uint16_t rr_len = 0;
    uint16_t assembly_len = 0;
    EipForwardOpenInfo info;
    int rc;

    path[9] = key_minor;
    memset(&info, 0, sizeof info);
    info.priority_tick = 5;
    info.timeout_ticks = 20;
    info.o2t_id = 0x11111111u;
    info.connection_serial = 7;
    info.vendor_id = 1;
    info.originator_serial = 99;
    info.timeout_multiplier = 2;
    info.o2t_rpi_us = 20000;
    info.t2o_rpi_us = 20000;
    info.o2t_params = eip_large_conn_params((uint16_t)(2u + 4u + 8u), 0, 0, 2, 0);
    info.t2o_params = eip_large_conn_params((uint16_t)(2u + 8u), 0, 0, 2, 0);
    info.transport = 0x01;
    rc = eip_build_assembly_path(path + 10, (uint16_t)(sizeof path - 10u), &assembly_len, 150, 100, 101);
    path_len = (uint16_t)(path_len + assembly_len);
    rc |= eip_build_forward_open_request(cip, sizeof cip, &cip_len, &info, path, path_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(adapter, session, EIP_CMD_SEND_RR, rr, rr_len, response, response_len);
    return rc;
}

/* Sets up a registered adapter whose identity is vendor 48, type 12, product 1234, revision 3.3. */
static void KeyedAdapter(EipAdapter *adapter, uint32_t *session, uint8_t *response)
{
    const uint8_t ip[4] = {172, 16, 82, 10};
    const uint8_t body[4] = {1, 0, 0, 0};
    uint16_t response_len = 0;

    *session = 0;
    eip_adapter_init(adapter, ip, 1);
    eip_adapter_set_clock(adapter, 1000);
    eip_adapter_set_identity(adapter, 48, 12, 1234, 3, 3, "Keyed");
    Expect(RoundTrip(adapter, session, EIP_CMD_REGISTER, body, 4, response, &response_len) == EIP_OK,
           "key register");
}

/* A key's minor revision of 0 means any minor. A PLC with "exact match" keying on a module defined
 * with no minor sends this, and must not be refused. */
static void TestKeyMinorZeroIsWildcard(void)
{
    static EipAdapter adapter;
    uint32_t session = 0;
    uint8_t response[700];
    uint16_t response_len = 0;
    uint8_t service = 0;
    uint8_t status = 0;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    int rc;

    KeyedAdapter(&adapter, &session, response);
    rc = OpenWithKey(&adapter, &session, 0, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0, "key minor 0 matches any minor");

    KeyedAdapter(&adapter, &session, response);
    rc = OpenWithKey(&adapter, &session, 3, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0, "key with the right minor");

    KeyedAdapter(&adapter, &session, response);
    rc = OpenWithKey(&adapter, &session, 9, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0x01 && extended == 0x0116, "key with the wrong minor is refused");
}

/* A refused Forward_Open or Forward_Close carries the connection triad, then the remaining path
 * size and a reserved byte, so the originator can match the failure to its request. */
static void TestRefusalCarriesTriad(void)
{
    static EipAdapter adapter;
    const uint8_t triad[10] = {0x07, 0x00, 0x01, 0x00, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint32_t session = 0;
    uint8_t response[700];
    uint16_t response_len = 0;
    uint8_t path[16];
    uint16_t path_len = 0;
    uint8_t cip[64];
    uint16_t cip_len = 0;
    uint8_t rr[128];
    uint16_t rr_len = 0;
    uint8_t service = 0;
    uint8_t status = 0;
    uint16_t extended = 0;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    int rc;

    KeyedAdapter(&adapter, &session, response);
    rc = OpenWithKey(&adapter, &session, 9, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0x01, "refused open");
    Expect(data_len == 10 && memcmp(data, triad, 10) == 0, "refused open carries the triad");

    rc = eip_build_assembly_path(path, sizeof path, &path_len, 150, 100, 101);
    rc |= eip_build_forward_close_request(cip, sizeof cip, &cip_len, 5, 20, 7, 1, 99, path, path_len);
    rc |= eip_build_rr_body(rr, sizeof rr, 1, cip, cip_len, 0, 0, 0, &rr_len);
    rc |= RoundTrip(&adapter, &session, EIP_CMD_SEND_RR, rr, rr_len, response, &response_len);
    rc |= CipFromReply(response, response_len, &service, &status, &extended, &data, &data_len);
    Expect(rc == EIP_OK && status == 0x01 && extended == 0x0107, "close of no connection");
    Expect(data_len == 10 && memcmp(data, triad, 10) == 0, "refused close carries the triad");
}

/* data_len near 65535 must be refused before the size is cut to 16 bits. */
static void TestIoPacketLengthOverflow(void)
{
    static uint8_t data[65530];
    uint8_t out[64];
    uint16_t out_len = 0;

    Expect(eip_build_io_packet(out, sizeof out, &out_len, 1, 1, 1, 1, 1, data, sizeof data) ==
               EIP_ERR_TOO_BIG,
           "I/O packet length does not wrap");
}

/* Stored settings: TCP/IP Interface attr 13 and the QoS object (0x48). Pins the defaults, the range
 * checks, that a Set needs a caller who can store, that the QoS object is absent until the caller
 * says packets are marked, and the T->O priority a Forward_Open leaves for the caller's DSCP choice. */
static void TestStoredSettings(void)
{
    static EipAdapter adapter;
    const uint8_t ip[4] = {172, 16, 82, 148};
    uint8_t response[700];
    uint16_t response_len = 0;
    uint32_t session = 0;
    uint8_t body[4];
    uint8_t status = 0xFF;
    const uint8_t *data = 0;
    uint16_t data_len = 0;
    uint8_t reg[4] = {0x01, 0x00, 0x00, 0x00};
    EipNvSettings nv;
    uint16_t attribute;
    int rc;
    static const uint8_t defaults[5] = {55, 47, 43, 31, 27};

    eip_adapter_init(&adapter, ip, 1);
    eip_adapter_set_clock(&adapter, 1000);
    eip_adapter_clear_assemblies(&adapter);
    eip_adapter_add_assembly(&adapter, 110, EIP_ASM_CONSUMED, 4, "Out", 0);
    eip_adapter_add_assembly(&adapter, 101, EIP_ASM_PRODUCED, 4, "In", 0);
    RoundTrip(&adapter, &session, EIP_CMD_REGISTER, reg, 4, response, &response_len);

    /* TCP/IP class revision 4, and attr 13 with its default. */
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF5, 0, 1, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 2 && eip_load_le16(data) == 4, "nv: tcp/ip class rev 4");
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF6, 0, 1, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 2 && eip_load_le16(data) == 3, "nv: ethernet link class rev 3");
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF5, 1, 13, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 2 && eip_load_le16(data) == 120, "nv: attr 13 default 120");
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0xF5, 1, 14, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x14, "nv: attr 14 does not exist");

    /* Nobody to store: read-only. */
    eip_store_le16(body, 30);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 13, body, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x0E && adapter.nv_dirty == 0, "nv: attr 13 not settable without a store");

    /* The QoS object is absent until the caller says the port marks packets. */
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0x48, 1, 4, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x16, "nv: no QoS object before qos_present");
    eip_adapter_set_qos_present(&adapter, 1);
    for (attribute = 4; attribute <= 8u; attribute++) {
        rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0x48, 1, (int)attribute, 0, 0, &status, &data,
                    &data_len, response);
        Expect(rc == EIP_OK && status == 0 && data_len == 1 && data[0] == defaults[attribute - 4u],
               "nv: QoS default DSCP");
    }
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0x48, 1, 1, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x14, "nv: QoS attr 1 not implemented");
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0x48, 1, 9, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x14, "nv: QoS attr 9 does not exist");
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0x48, 2, 4, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x16, "nv: QoS instance 2 does not exist");
    rc = CipRaw(&adapter, &session, 0x01, 0x48, 1, -1, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x08, "nv: QoS has no Get_Attributes_All");
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0x48, 0, 1, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 2 && eip_load_le16(data) == 1, "nv: QoS class rev 1");
    body[0] = 10;
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0x48, 1, 4, body, 1, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x0E, "nv: QoS not settable without a store");

    /* With a store. Stored values out of range fall back to their defaults. */
    eip_nv_defaults(&nv);
    nv.encap_timeout_s = 4000;
    nv.dscp_high = 64;
    nv.dscp_low = 12;
    eip_adapter_set_nv(&adapter, &nv, 1);
    Expect(adapter.nv.encap_timeout_s == 120 && adapter.nv.dscp_high == 43 && adapter.nv.dscp_low == 12,
           "nv: out-of-range stored values take defaults");

    eip_store_le16(body, 3600);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 13, body, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && adapter.nv.encap_timeout_s == 3600 && adapter.nv_dirty == 1,
           "nv: attr 13 set to the maximum");
    adapter.nv_dirty = 0;
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 13, body, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && adapter.nv_dirty == 0, "nv: an unchanged value is not stored again");
    eip_store_le16(body, 3601);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 13, body, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x09 && adapter.nv.encap_timeout_s == 3600, "nv: attr 13 over 3600 refused");
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 13, body, 1, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x13, "nv: attr 13 short");
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 13, body, 3, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x15, "nv: attr 13 long");
    eip_store_le16(body, 0);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 13, body, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && adapter.nv.encap_timeout_s == 0, "nv: attr 13 set to 0 (off)");
    /* Attr 13 does not need, and does not unlock, the interface configuration. */
    eip_store_le32(body, 0);
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0xF5, 1, 3, body, 4, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x0E, "nv: attr 3 still needs set_interface");

    adapter.nv_dirty = 0;
    body[0] = 63;
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0x48, 1, 8, body, 1, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && adapter.nv.dscp_explicit == 63 && adapter.nv_dirty == 1,
           "nv: QoS explicit DSCP set");
    rc = CipRaw(&adapter, &session, EIP_CIP_GET_ATTRIBUTE, 0x48, 1, 8, 0, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0 && data_len == 1 && data[0] == 63, "nv: QoS reads back the new value");
    body[0] = 64;
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0x48, 1, 4, body, 1, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x09 && adapter.nv.dscp_urgent == 55, "nv: DSCP over 63 refused");
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0x48, 1, 4, body, 2, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x15, "nv: DSCP long");
    rc = CipRaw(&adapter, &session, EIP_CIP_SET_ATTRIBUTE, 0x48, 1, 4, body, 0, &status, &data, &data_len, response);
    Expect(rc == EIP_OK && status == 0x13, "nv: DSCP short");

    /* RobotOpen sends priority bits 0 (low). */
    adapter.t2o_priority = 9;
    rc = RobotOpen(&adapter, &session, 2, 0x3001, 0x77, 1, response, &response_len);
    Expect(rc == EIP_OK && adapter.io_open == 1 && adapter.t2o_priority == 0, "nv: open records T->O priority");
}

int main(void)
{
    TestStoredSettings();
    TestKeyMinorZeroIsWildcard();
    TestRefusalCarriesTriad();
    TestOpenReportsOwnIntervals();
    TestExplicitReplyTooLarge();
    TestParseIpv4();
    TestAssemblyPool();
    TestSeq16Fallback();
    TestTick();
    TestTagPath();
    TestEncap();
    TestWrapAndParams();
    TestForwardOpen();
    TestIoRoundTrip();
    TestIdentity();
    TestMicro800();
    TestAdapter();
    TestRobotShape();
    TestNetObjects();
    TestMicro850Open();
    TestNetConfigSet();
    TestInputHardening();
    TestMultipleService();
    TestIoPacketLengthOverflow();
    if (g_failed != 0) {
        printf("%d failed\n", g_failed);
        return 1;
    }
    printf("codec tests passed\n");
    return 0;
}
