/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* EtherNet/IP and CIP wire formats: pure build and parse functions, no sockets, clock or state.
 * eip_session.c, eip_io.c and eip_scan.c use them for the scanner. eip_adapter.c shares the
 * encapsulation, CPF, sockaddr and I/O packet code.
 *
 * Sections, in file order: timing and connection parameters, EPATH, encapsulation, CIP messages,
 * Unconnected Send, SendRRData and CPF, sockaddr items, Forward_Open and Forward_Close, Class 1
 * packets, ListIdentity, Rockwell name heuristics, the symbol list.
 *
 * Multi-byte fields are little-endian, except inside sockaddr items. Parsers do not copy: the
 * pointers they return refer to the input buffer. */
#include "eip_codec.h"

#include <string.h>

const char *eip_status_text(int code)
{
    switch (code) {
    case EIP_OK:
        return "ok";
    case EIP_ERR_ARG:
        return "bad argument";
    case EIP_ERR_CONNECT:
        return "connect failed";
    case EIP_ERR_IO:
        return "socket error";
    case EIP_ERR_TIMEOUT:
        return "timeout";
    case EIP_ERR_PROTOCOL:
        return "unexpected packet";
    case EIP_ERR_CIP:
        return "CIP status";
    case EIP_ERR_TOO_BIG:
        return "message too large";
    case EIP_ERR_PATH:
        return "bad tag or path";
    case EIP_ERR_STATE:
        return "session is not open";
    default:
        return "error";
    }
}

/* ---- timing and connection parameters ---- */

/* Encode timeout_ms as a CIP tick pair: timeout = 2^tick x ticks ms. These are the priority/tick
 * and timeout-ticks bytes of Unconnected Send, Forward_Open and Forward_Close. The priority bits
 * above the tick nibble stay 0. tick is the smallest value that fits ticks in a byte, up to a
 * limit of 15. ticks rounds up, then clamps to 1..255, so the encoded time can fall just short
 * near the clamp. 5000 ms gives tick 5, ticks 157, which is 5024 ms. */
void eip_tick_pair(uint32_t timeout_ms, uint8_t *tick, uint8_t *ticks)
{
    uint8_t exponent = 0;
    uint32_t unit = 1;
    uint32_t count;

    if (timeout_ms == 0) {
        timeout_ms = 1;
    }
    while (exponent < 15 && (timeout_ms / unit) > 255u) {
        exponent++;
        unit <<= 1;
    }
    count = (timeout_ms + unit - 1u) / unit;
    if (count < 1u) {
        count = 1u;
    }
    if (count > 255u) {
        count = 255u;
    }
    *tick = exponent;
    *ticks = (uint8_t)count;
}

/* 32-bit network connection parameters for a Large_Forward_Open, as built here:
 *   bits 15..0   connection size in bytes
 *   bit  25      variable size (0 is fixed)
 *   bits 27..26  priority
 *   bits 30..29  connection type (1 multicast, 2 point-to-point)
 *   bit  31      redundant owner
 * This is the 16-bit Forward_Open layout with its flags moved up 16 bits. DecodeParam in
 * eip_adapter.c reads the same layout. */
uint32_t eip_large_conn_params(uint16_t size_bytes, int variable, int priority, int conn_type,
                               int redundant_owner)
{
    uint32_t params = size_bytes;

    if (redundant_owner) {
        params |= 1u << 31;
    }
    params |= ((uint32_t)(conn_type & 3)) << 29;
    params |= ((uint32_t)(priority & 3)) << 26;
    if (variable) {
        params |= 1u << 25;
    }
    return params;
}

/* ---- EPATH ---- */

/* An array index is a logical member segment: 0x28 8-bit, 0x29 16-bit, 0x2A 32-bit. The two wider
 * forms put a 0x00 pad after the segment byte so the value starts on a 16-bit boundary. */
static int emit_element(uint8_t *out, uint16_t cap, uint16_t *used, uint32_t index)
{
    uint16_t n = *used;

    if (index <= 255u) {
        if ((uint32_t)n + 2u > cap) {
            return EIP_ERR_TOO_BIG;
        }
        out[n++] = 0x28;
        out[n++] = (uint8_t)index;
    } else if (index <= 65535u) {
        if ((uint32_t)n + 4u > cap) {
            return EIP_ERR_TOO_BIG;
        }
        out[n++] = 0x29;
        out[n++] = 0x00;
        eip_store_le16(out + n, (uint16_t)index);
        n = (uint16_t)(n + 2u);
    } else {
        if ((uint32_t)n + 6u > cap) {
            return EIP_ERR_TOO_BIG;
        }
        out[n++] = 0x2A;
        out[n++] = 0x00;
        eip_store_le32(out + n, index);
        n = (uint16_t)(n + 4u);
    }
    *used = n;
    return EIP_OK;
}

/* Encode a Logix tag name as a symbolic EPATH.
 *   name         ANSI extended symbolic segment: 0x91, length, the characters, and a 0x00 pad
 *                when the length is odd
 *   [n], [n,m]   one element segment per index; [n][m] encodes the same way
 *   .            starts the next name segment
 * A name is letters, digits, '_' and ':', so "Program:Main" is a single segment. Anything else,
 * including spaces and '@', is EIP_ERR_PATH. There is no bit addressing: "Tag.3" is a member
 * named "3". Index digits are not range checked, and a value past 32 bits wraps. */
int eip_build_tag_path(const char *tag, uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    const char *cursor;
    uint16_t used = 0;

    if (tag == 0 || tag[0] == '\0' || out == 0 || out_len == 0) {
        return EIP_ERR_ARG;
    }
    cursor = tag;
    while (*cursor != '\0') {
        const char *start;
        uint16_t length;
        uint16_t segment;

        if (*cursor == '.' || *cursor == '[') {
            return EIP_ERR_PATH;
        }
        start = cursor;
        while (*cursor != '\0' && *cursor != '.' && *cursor != '[') {
            unsigned char ch = (unsigned char)*cursor;
            int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                     ch == '_' || ch == ':';
            if (!ok) {
                return EIP_ERR_PATH;
            }
            cursor++;
        }
        length = (uint16_t)(cursor - start);
        if (length == 0 || length > 255u) {
            return EIP_ERR_PATH;
        }
        /* Segment byte, length byte, name, and the pad that keeps the path word aligned. */
        segment = (uint16_t)(2u + length + (length & 1u));
        if ((uint32_t)used + segment > cap) {
            return EIP_ERR_TOO_BIG;
        }
        out[used++] = 0x91;
        out[used++] = (uint8_t)length;
        memcpy(out + used, start, length);
        used = (uint16_t)(used + length);
        if (length & 1u) {
            out[used++] = 0x00;
        }
        while (*cursor == '[') {
            cursor++;
            for (;;) {
                uint32_t index = 0;
                int rc;

                if (*cursor < '0' || *cursor > '9') {
                    return EIP_ERR_PATH;
                }
                while (*cursor >= '0' && *cursor <= '9') {
                    index = index * 10u + (uint32_t)(*cursor - '0');
                    cursor++;
                }
                rc = emit_element(out, cap, &used, index);
                if (rc != EIP_OK) {
                    return rc;
                }
                if (*cursor == ',') {
                    cursor++;
                    continue;
                }
                if (*cursor == ']') {
                    cursor++;
                    break;
                }
                return EIP_ERR_PATH;
            }
        }
        if (*cursor == '.') {
            cursor++;
            if (*cursor == '\0') {
                return EIP_ERR_PATH;
            }
            continue;
        }
        if (*cursor != '\0') {
            return EIP_ERR_PATH;
        }
    }
    if (used == 0 || (used & 1u)) {
        return EIP_ERR_PATH;
    }
    *out_len = used;
    return EIP_OK;
}

/* Append one logical segment. segment is the 8-bit form: 0x20 class, 0x24 instance, 0x2C connection
 * point, 0x30 attribute. A value over 255 uses the 16-bit form, which is segment + 1 and a 0x00
 * pad. A value over 65535 is EIP_ERR_ARG. */
int eip_put_logical(uint8_t *out, uint16_t cap, uint16_t *used, uint8_t segment, uint32_t value)
{
    uint16_t n;

    if (out == 0 || used == 0) {
        return EIP_ERR_ARG;
    }
    n = *used;
    if (value <= 255u) {
        if ((uint32_t)n + 2u > cap) {
            return EIP_ERR_TOO_BIG;
        }
        out[n++] = segment;
        out[n++] = (uint8_t)value;
    } else if (value <= 65535u) {
        if ((uint32_t)n + 4u > cap) {
            return EIP_ERR_TOO_BIG;
        }
        out[n++] = (uint8_t)(segment + 1u);
        out[n++] = 0x00;
        eip_store_le16(out + n, (uint16_t)value);
        n = (uint16_t)(n + 2u);
    } else {
        return EIP_ERR_ARG;
    }
    *used = n;
    return EIP_OK;
}

/* ---- encapsulation ---- */

/* The 24-byte encapsulation header, followed by the command body:
 *    0  command
 *    2  body length (excludes this header)
 *    4  session handle
 *    8  status, 0 in a request
 *   12  sender context, 8 bytes, echoed back in the reply
 *   20  options, 0 */
int eip_build_encap(uint8_t *out, uint16_t cap, uint16_t command, uint32_t session,
                    const uint8_t context[8], const uint8_t *body, uint16_t body_len,
                    uint16_t *out_len)
{
    if (out == 0 || context == 0 || out_len == 0) {
        return EIP_ERR_ARG;
    }
    if ((uint32_t)body_len + 24u > cap) {
        return EIP_ERR_TOO_BIG;
    }
    if (body_len != 0 && body == 0) {
        return EIP_ERR_ARG;
    }
    eip_store_le16(out, command);
    eip_store_le16(out + 2, body_len);
    eip_store_le32(out + 4, session);
    eip_store_le32(out + 8, 0);
    memcpy(out + 12, context, 8);
    eip_store_le32(out + 20, 0);
    if (body_len != 0) {
        memcpy(out + 24, body, body_len);
    }
    *out_len = (uint16_t)(24u + body_len);
    return EIP_OK;
}

/* packet must be exactly one message: header plus the body length it claims. The header fields in
 * view are set even when that check fails, but view->body is not. */
int eip_parse_encap(const uint8_t *packet, uint16_t packet_len, EipEncapView *view)
{
    if (packet == 0 || view == 0 || packet_len < 24u) {
        return EIP_ERR_PROTOCOL;
    }
    view->command = eip_load_le16(packet);
    view->length = eip_load_le16(packet + 2);
    view->session = eip_load_le32(packet + 4);
    view->status = eip_load_le32(packet + 8);
    memcpy(view->context, packet + 12, 8);
    if ((uint32_t)view->length + 24u != packet_len) {
        return EIP_ERR_PROTOCOL;
    }
    view->body = packet + 24;
    return EIP_OK;
}

/* ---- CIP messages ---- */

/* CIP request: 0 service, 1 path size in 16-bit words, 2 path, then the service data. The size byte
 * counts words, so path_len must be even and at most 510. */
int eip_build_cip_req(uint8_t *out, uint16_t cap, uint8_t service, const uint8_t *path,
                      uint16_t path_len, const uint8_t *data, uint16_t data_len, uint16_t *out_len)
{
    uint16_t words;
    uint32_t total;

    if (out == 0 || out_len == 0 || (path_len != 0 && path == 0) || (data_len != 0 && data == 0)) {
        return EIP_ERR_ARG;
    }
    if (path_len & 1u) {
        return EIP_ERR_PATH;
    }
    words = (uint16_t)(path_len / 2u);
    if (words > 255u) {
        return EIP_ERR_PATH;
    }
    total = 2u + path_len + data_len;
    if (total > cap) {
        return EIP_ERR_TOO_BIG;
    }
    out[0] = service;
    out[1] = (uint8_t)words;
    if (path_len != 0) {
        memcpy(out + 2, path, path_len);
    }
    if (data_len != 0) {
        memcpy(out + 2 + path_len, data, data_len);
    }
    *out_len = (uint16_t)total;
    return EIP_OK;
}

/* CIP reply: 0 service with bit 0x80 set, 1 reserved, 2 general status, 3 additional status size in
 * 16-bit words, 4 additional status, then the reply data. extended is the first additional word,
 * or 0 when there is none; any further words are skipped. A nonzero status is not a parse error:
 * the caller decides what it means. */
int eip_parse_cip_rsp(const uint8_t *message, uint16_t len, uint8_t *service, uint8_t *status,
                      uint16_t *extended, const uint8_t **data, uint16_t *data_len)
{
    uint8_t words;
    uint16_t extra;

    if (message == 0 || service == 0 || status == 0 || extended == 0 || data == 0 || data_len == 0 ||
        len < 4u) {
        return EIP_ERR_PROTOCOL;
    }
    *service = message[0];
    *status = message[2];
    words = message[3];
    extra = (uint16_t)words * 2u;
    if ((uint16_t)(4u + extra) > len) {
        return EIP_ERR_PROTOCOL;
    }
    *extended = (words == 0) ? 0 : eip_load_le16(message + 4);
    *data = message + 4 + extra;
    *data_len = (uint16_t)(len - (uint16_t)(4u + extra));
    return EIP_OK;
}

/* ---- Unconnected Send ---- */

/* Wrap a CIP message in Unconnected Send (0x52) to the Connection Manager, routed over port 1 to
 * one backplane slot:
 *    0  0x52
 *    1  path size, 2 words
 *    2  path 20 06 24 01: class 6, instance 1
 *    6  tick
 *    7  ticks
 *    8  embedded message length (excludes the pad)
 *   10  embedded message, then a 0x00 pad when its length is odd
 *    n  route path size, 1 word
 *  n+1  reserved
 *  n+2  port segment: 0x01 is port 1
 *  n+3  link address: the slot
 * One hop only. Multi-hop routes cannot be expressed. */
int eip_wrap_unconnected(uint8_t *out, uint16_t cap, const uint8_t *inner, uint16_t inner_len,
                         uint8_t slot, uint8_t tick, uint8_t ticks, uint16_t *out_len)
{
    uint16_t pad;
    uint32_t total;
    uint16_t route_at;

    if (out == 0 || out_len == 0 || (inner_len != 0 && inner == 0)) {
        return EIP_ERR_ARG;
    }
    pad = (uint16_t)(inner_len & 1u);
    total = 10u + inner_len + pad + 4u;
    if (total > cap) {
        return EIP_ERR_TOO_BIG;
    }
    out[0] = EIP_CIP_UNCONNECTED_SEND;
    out[1] = 0x02;
    out[2] = 0x20;
    out[3] = 0x06;
    out[4] = 0x24;
    out[5] = 0x01;
    out[6] = tick;
    out[7] = ticks;
    eip_store_le16(out + 8, inner_len);
    if (inner_len != 0) {
        memcpy(out + 10, inner, inner_len);
    }
    if (pad) {
        out[10 + inner_len] = 0x00;
    }
    route_at = (uint16_t)(10u + inner_len + pad);
    out[route_at] = 0x01;
    out[route_at + 1] = 0x00;
    out[route_at + 2] = 0x01;
    out[route_at + 3] = slot;
    *out_len = (uint16_t)total;
    return EIP_OK;
}

/* ---- SendRRData and CPF ---- */

/* SendRRData body:
 *    0  interface handle, 0 is CIP
 *    4  timeout in seconds
 *    6  CPF item count: 2 plus extra_items
 *    8  null address item: type 0, length 0
 *   12  unconnected data item: type 0x00B2, then its length
 *   16  the CIP message
 * extra is already-encoded CPF items placed after the data item. eip_io.c puts the two Forward_Open
 * sockaddr items there. */
int eip_build_rr_body(uint8_t *out, uint16_t cap, uint16_t timeout_sec, const uint8_t *cip,
                      uint16_t cip_len, const uint8_t *extra, uint16_t extra_len,
                      uint16_t extra_items, uint16_t *out_len)
{
    uint32_t total;
    uint16_t used = 0;

    if (out == 0 || out_len == 0 || (cip_len != 0 && cip == 0) || (extra_len != 0 && extra == 0)) {
        return EIP_ERR_ARG;
    }
    total = 16u + cip_len + extra_len;
    if (total > cap) {
        return EIP_ERR_TOO_BIG;
    }
    eip_store_le32(out + used, 0);
    used = (uint16_t)(used + 4u);
    eip_store_le16(out + used, timeout_sec);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, (uint16_t)(2u + extra_items));
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, EIP_ITEM_NULL);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, 0);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, EIP_ITEM_UNCONNECTED);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, cip_len);
    used = (uint16_t)(used + 2u);
    if (cip_len != 0) {
        memcpy(out + used, cip, cip_len);
        used = (uint16_t)(used + cip_len);
    }
    if (extra_len != 0) {
        memcpy(out + used, extra, extra_len);
        used = (uint16_t)(used + extra_len);
    }
    *out_len = used;
    return EIP_OK;
}

/* Common Packet Format: a 16-bit item count, then items of type (2), length (2), data. Finds the
 * first item of the given type. A missing item and a malformed list both return EIP_ERR_PROTOCOL.
 * Items after the match are not validated. */
int eip_cpf_find(const uint8_t *cpf, uint16_t len, uint16_t type, const uint8_t **data,
                 uint16_t *data_len)
{
    uint16_t count;
    uint16_t offset;
    uint16_t index;

    if (cpf == 0 || data == 0 || data_len == 0 || len < 2u) {
        return EIP_ERR_PROTOCOL;
    }
    count = eip_load_le16(cpf);
    offset = 2;
    for (index = 0; index < count; index++) {
        uint16_t item_type;
        uint16_t item_len;

        if ((uint32_t)offset + 4u > len) {
            return EIP_ERR_PROTOCOL;
        }
        item_type = eip_load_le16(cpf + offset);
        item_len = eip_load_le16(cpf + offset + 2);
        offset = (uint16_t)(offset + 4u);
        if ((uint32_t)offset + item_len > len) {
            return EIP_ERR_PROTOCOL;
        }
        if (item_type == type) {
            *data = cpf + offset;
            *data_len = item_len;
            return EIP_OK;
        }
        offset = (uint16_t)(offset + item_len);
    }
    return EIP_ERR_PROTOCOL;
}

/* ---- sockaddr items ---- */

/* A sockaddr info item is 20 bytes: type, length 16, then an image of sockaddr_in. Unlike every
 * other field here the image is big-endian: 4 family (2, AF_INET), 6 port, 8 address, 12 eight
 * zero bytes. */
int eip_build_sockaddr_item(uint8_t *out, uint16_t cap, uint16_t *out_len, uint16_t item_type,
                            const uint8_t ip[4], uint16_t port)
{
    if (out == 0 || out_len == 0 || ip == 0 || cap < 20u) {
        return (cap < 20u) ? EIP_ERR_TOO_BIG : EIP_ERR_ARG;
    }
    eip_store_le16(out, item_type);
    eip_store_le16(out + 2, 16);
    out[4] = 0x00;
    out[5] = 0x02;
    out[6] = (uint8_t)(port >> 8);
    out[7] = (uint8_t)port;
    out[8] = ip[0];
    out[9] = ip[1];
    out[10] = ip[2];
    out[11] = ip[3];
    memset(out + 12, 0, 8);
    *out_len = 20;
    return EIP_OK;
}

/* data is the item's 16 data bytes, without the type and length, as eip_cpf_find returns them. The
 * family field is not checked. */
int eip_parse_sockaddr(const uint8_t *data, uint16_t len, uint8_t ip[4], uint16_t *port)
{
    if (data == 0 || ip == 0 || port == 0 || len < 16u) {
        return EIP_ERR_PROTOCOL;
    }
    *port = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
    ip[0] = data[4];
    ip[1] = data[5];
    ip[2] = data[6];
    ip[3] = data[7];
    return EIP_OK;
}

/* ---- Forward_Open and Forward_Close ---- */

/* Connection path for a Class 1 open: class 4 (Assembly), the config instance, then two connection
 * points, O->T first and T->O second. A config instance of 0 is still encoded. */
int eip_build_assembly_path(uint8_t *out, uint16_t cap, uint16_t *out_len, uint16_t config_instance,
                            uint16_t o2t_point, uint16_t t2o_point)
{
    uint16_t used = 0;
    int rc;

    if (out == 0 || out_len == 0) {
        return EIP_ERR_ARG;
    }
    rc = eip_put_logical(out, cap, &used, 0x20, 0x04);
    if (rc != EIP_OK) {
        return rc;
    }
    rc = eip_put_logical(out, cap, &used, 0x24, config_instance);
    if (rc != EIP_OK) {
        return rc;
    }
    rc = eip_put_logical(out, cap, &used, 0x2C, o2t_point);
    if (rc != EIP_OK) {
        return rc;
    }
    rc = eip_put_logical(out, cap, &used, 0x2C, t2o_point);
    if (rc != EIP_OK) {
        return rc;
    }
    if (used & 1u) {
        return EIP_ERR_PATH;
    }
    *out_len = used;
    return EIP_OK;
}

/* Class 6 (Connection Manager), instance 1. */
static const uint8_t kConnectionManagerPath[4] = {0x20, 0x06, 0x24, 0x01};

/* Large_Forward_Open (0x5B), the only open the scanner sends. Request data:
 *    0  priority/tick
 *    1  timeout ticks
 *    2  O->T connection id
 *    6  T->O connection id
 *   10  connection serial
 *   12  originator vendor id
 *   14  originator serial
 *   18  timeout multiplier
 *   19  reserved, 3 bytes
 *   22  O->T RPI in microseconds
 *   26  O->T network connection parameters, 32 bits
 *   30  T->O RPI in microseconds
 *   34  T->O network connection parameters, 32 bits
 *   38  transport class and trigger
 *   39  connection path size in words
 *   40  connection path
 * The 40-byte limit on the path is what fits body[]. */
int eip_build_forward_open_request(uint8_t *out, uint16_t cap, uint16_t *out_len,
                                   const EipForwardOpenInfo *info, const uint8_t *conn_path,
                                   uint16_t path_len)
{
    uint8_t body[80];
    uint16_t body_len = 0;

    if (info == 0 || out_len == 0 || (path_len != 0 && conn_path == 0) || (path_len & 1u) ||
        path_len > 40u) {
        return EIP_ERR_ARG;
    }
    body[body_len++] = info->priority_tick;
    body[body_len++] = info->timeout_ticks;
    eip_store_le32(body + body_len, info->o2t_id);
    body_len = (uint16_t)(body_len + 4u);
    eip_store_le32(body + body_len, info->t2o_id);
    body_len = (uint16_t)(body_len + 4u);
    eip_store_le16(body + body_len, info->connection_serial);
    body_len = (uint16_t)(body_len + 2u);
    eip_store_le16(body + body_len, info->vendor_id);
    body_len = (uint16_t)(body_len + 2u);
    eip_store_le32(body + body_len, info->originator_serial);
    body_len = (uint16_t)(body_len + 4u);
    body[body_len++] = info->timeout_multiplier;
    body[body_len++] = 0x00;
    body[body_len++] = 0x00;
    body[body_len++] = 0x00;
    eip_store_le32(body + body_len, info->o2t_rpi_us);
    body_len = (uint16_t)(body_len + 4u);
    eip_store_le32(body + body_len, info->o2t_params);
    body_len = (uint16_t)(body_len + 4u);
    eip_store_le32(body + body_len, info->t2o_rpi_us);
    body_len = (uint16_t)(body_len + 4u);
    eip_store_le32(body + body_len, info->t2o_params);
    body_len = (uint16_t)(body_len + 4u);
    body[body_len++] = info->transport;
    body[body_len++] = (uint8_t)(path_len / 2u);
    if (path_len != 0) {
        memcpy(body + body_len, conn_path, path_len);
        body_len = (uint16_t)(body_len + path_len);
    }
    return eip_build_cip_req(out, cap, EIP_CIP_LARGE_FORWARD_OPEN, kConnectionManagerPath, 4, body,
                             body_len, out_len);
}

/* Forward_Close (0x4E). The serial, vendor id and originator serial must be the ones the open used:
 * together they name the connection. Request data:
 *    0  priority/tick
 *    1  timeout ticks
 *    2  connection serial
 *    4  originator vendor id
 *    6  originator serial
 *   10  connection path size in words
 *   11  reserved
 *   12  connection path */
int eip_build_forward_close_request(uint8_t *out, uint16_t cap, uint16_t *out_len,
                                    uint8_t priority_tick, uint8_t timeout_ticks,
                                    uint16_t connection_serial, uint16_t vendor_id,
                                    uint32_t originator_serial, const uint8_t *conn_path,
                                    uint16_t path_len)
{
    uint8_t body[16 + 64];
    uint16_t body_len = 0;

    if (out_len == 0 || (path_len != 0 && conn_path == 0) || (path_len & 1u) || path_len > 64u) {
        return EIP_ERR_ARG;
    }
    body[body_len++] = priority_tick;
    body[body_len++] = timeout_ticks;
    eip_store_le16(body + body_len, connection_serial);
    body_len = (uint16_t)(body_len + 2u);
    eip_store_le16(body + body_len, vendor_id);
    body_len = (uint16_t)(body_len + 2u);
    eip_store_le32(body + body_len, originator_serial);
    body_len = (uint16_t)(body_len + 4u);
    body[body_len++] = (uint8_t)(path_len / 2u);
    body[body_len++] = 0x00;
    if (path_len != 0) {
        memcpy(body + body_len, conn_path, path_len);
        body_len = (uint16_t)(body_len + path_len);
    }
    return eip_build_cip_req(out, cap, EIP_CIP_FORWARD_CLOSE, kConnectionManagerPath, 4, body,
                             body_len, out_len);
}

/* Success reply data of a Forward_Open, after the CIP reply header:
 *    0  O->T connection id
 *    4  T->O connection id
 *    8  connection serial
 *   10  originator vendor id
 *   12  originator serial
 *   16  O->T actual packet interval, microseconds
 *   20  T->O actual packet interval, microseconds
 *   24  application reply size in words
 *   25  reserved
 *   26  application reply
 * Only the ids and intervals are read. The echoed serials are not compared with the request. */
int eip_parse_forward_open_success(const uint8_t *data, uint16_t len, uint32_t *o2t_id,
                                   uint32_t *t2o_id, uint32_t *o2t_api, uint32_t *t2o_api)
{
    if (data == 0 || o2t_id == 0 || t2o_id == 0 || o2t_api == 0 || t2o_api == 0 || len < 26u) {
        return EIP_ERR_PROTOCOL;
    }
    *o2t_id = eip_load_le32(data);
    *t2o_id = eip_load_le32(data + 4);
    *o2t_api = eip_load_le32(data + 16);
    *t2o_api = eip_load_le32(data + 20);
    return EIP_OK;
}

/* ---- Class 1 packets ---- */

/* A Class 1 datagram is bare CPF, with no encapsulation header:
 *    0  item count, 2
 *    2  sequenced address item: type 0x8002, length 8
 *    6  connection id
 *   10  32-bit sequence number
 *   14  connected data item: type 0x00B1, then its length
 *   18  16-bit sequence count
 *   20  when run_idle is set, a 32-bit header with bit 0 = run
 *       then the assembly data
 * The item length is computed in 16 bits, so the capacity check relies on callers passing
 * assembly-sized data, nowhere near 65535 bytes. */
int eip_build_io_packet(uint8_t *out, uint16_t cap, uint16_t *out_len, uint32_t connection_id,
                        uint32_t sequence_32, uint16_t sequence_16, int run_idle, int run,
                        const uint8_t *data, uint16_t data_len)
{
    uint16_t payload;
    uint16_t used = 0;
    uint32_t total;

    if (out == 0 || out_len == 0 || (data_len != 0 && data == 0)) {
        return EIP_ERR_ARG;
    }
    /* Sized in 32 bits: data_len near 65535 would wrap a 16-bit sum and pass the check. */
    total = 18u + 2u + (run_idle ? 4u : 0u) + data_len;
    if (total > cap) {
        return EIP_ERR_TOO_BIG;
    }
    payload = (uint16_t)(total - 18u);
    eip_store_le16(out + used, 2);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, EIP_ITEM_SEQUENCED);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, 8);
    used = (uint16_t)(used + 2u);
    eip_store_le32(out + used, connection_id);
    used = (uint16_t)(used + 4u);
    eip_store_le32(out + used, sequence_32);
    used = (uint16_t)(used + 4u);
    eip_store_le16(out + used, EIP_ITEM_CONNECTED);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, payload);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, sequence_16);
    used = (uint16_t)(used + 2u);
    if (run_idle) {
        eip_store_le32(out + used, run ? 1u : 0u);
        used = (uint16_t)(used + 4u);
    }
    if (data_len != 0) {
        memcpy(out + used, data, data_len);
        used = (uint16_t)(used + data_len);
    }
    *out_len = used;
    return EIP_OK;
}

int eip_parse_io_packet(const uint8_t *packet, uint16_t len, uint32_t *connection_id,
                        uint16_t *sequence, const uint8_t **data, uint16_t *data_len)
{
    uint32_t seq32 = 0;
    int has_seq32 = 0;
    return eip_parse_io_packet_seq(packet, len, connection_id, &seq32, &has_seq32, sequence, data, data_len);
}

/* Takes the connection id from a sequenced address item, or else from a plain connected address
 * item. *data starts after the 16-bit sequence count, so it still holds the run/idle header on a
 * connection that has one. Stripping that is the caller's job. */
int eip_parse_io_packet_seq(const uint8_t *packet, uint16_t len, uint32_t *connection_id, uint32_t *seq32,
                            int *has_seq32, uint16_t *sequence, const uint8_t **data, uint16_t *data_len)
{
    const uint8_t *address = 0;
    uint16_t address_len = 0;
    const uint8_t *connected = 0;
    uint16_t connected_len = 0;
    int rc;

    if (packet == 0 || connection_id == 0 || seq32 == 0 || has_seq32 == 0 || sequence == 0 || data == 0 ||
        data_len == 0) {
        return EIP_ERR_ARG;
    }
    *has_seq32 = 0;
    *seq32 = 0;
    rc = eip_cpf_find(packet, len, EIP_ITEM_SEQUENCED, &address, &address_len);
    if (rc == EIP_OK) {
        if (address_len < 8u) {
            return EIP_ERR_PROTOCOL;
        }
        *connection_id = eip_load_le32(address);
        *seq32 = eip_load_le32(address + 4);
        *has_seq32 = 1;
    } else {
        rc = eip_cpf_find(packet, len, EIP_ITEM_CONN_ADDR, &address, &address_len);
        if (rc != EIP_OK || address_len < 4u) {
            return EIP_ERR_PROTOCOL;
        }
        *connection_id = eip_load_le32(address);
    }
    rc = eip_cpf_find(packet, len, EIP_ITEM_CONNECTED, &connected, &connected_len);
    if (rc != EIP_OK || connected_len < 2u) {
        return EIP_ERR_PROTOCOL;
    }
    *sequence = eip_load_le16(connected);
    *data = connected + 2;
    *data_len = (uint16_t)(connected_len - 2u);
    return EIP_OK;
}

/* ---- ListIdentity ---- */

/* body is a ListIdentity reply body: CPF holding an identity item (0x000C). Item layout:
 *    0  encapsulation protocol version
 *    2  sockaddr, 16 bytes
 *   18  vendor id
 *   20  device type
 *   22  product code
 *   24  revision major, then minor
 *   26  status
 *   28  serial number
 *   32  product name length
 *   33  product name, then one state byte
 * A long name is cut to fit identity->name. state stays 0xFF when the device omits it. identity
 * is cleared first and may be partly filled on an error return. */
int eip_parse_identity(const uint8_t *body, uint16_t len, EipIdentity *identity)
{
    const uint8_t *item = 0;
    uint16_t item_len = 0;
    uint8_t name_len;
    uint16_t name_at;
    int rc;

    if (body == 0 || identity == 0) {
        return EIP_ERR_ARG;
    }
    memset(identity, 0, sizeof *identity);
    identity->state = 0xFF;
    rc = eip_cpf_find(body, len, EIP_ITEM_IDENTITY, &item, &item_len);
    if (rc != EIP_OK) {
        return rc;
    }
    /* version(2) + sockaddr(16) + vendor through serial = 2+16+2+2+2+2+2+4 = 32, then name length */
    if (item_len < 33u) {
        return EIP_ERR_PROTOCOL;
    }
    identity->vendor_id = eip_load_le16(item + 18);
    identity->device_type = eip_load_le16(item + 20);
    identity->product_code = eip_load_le16(item + 22);
    identity->revision_major = item[24];
    identity->revision_minor = item[25];
    identity->status = eip_load_le16(item + 26);
    identity->serial_number = eip_load_le32(item + 28);
    name_len = item[32];
    name_at = 33;
    if ((uint32_t)name_at + name_len > item_len) {
        return EIP_ERR_PROTOCOL;
    }
    if (name_len >= sizeof identity->name) {
        name_len = (uint8_t)(sizeof identity->name - 1u);
    }
    memcpy(identity->name, item + name_at, name_len);
    identity->name[name_len] = '\0';
    /* name_len may have been cut above. The state byte follows the name as sent. */
    if ((uint32_t)name_at + item[32] < item_len) {
        identity->state = item[name_at + item[32]];
    }
    return EIP_OK;
}

/* ---- Rockwell name heuristics ---- */

static int AsciiLower(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (int)(c + (unsigned char)('a' - 'A'));
    }
    return (int)c;
}

static int ContainsNoCase(const char *text, const char *token)
{
    size_t text_len;
    size_t token_len;
    size_t i;

    if (text == 0 || token == 0 || token[0] == '\0') {
        return 0;
    }
    text_len = strlen(text);
    token_len = strlen(token);
    if (token_len > text_len) {
        return 0;
    }
    for (i = 0; i + token_len <= text_len; i++) {
        size_t j;
        for (j = 0; j < token_len; j++) {
            if (AsciiLower((unsigned char)text[i + j]) != AsciiLower((unsigned char)token[j])) {
                break;
            }
        }
        if (j == token_len) {
            return 1;
        }
    }
    return 0;
}

int eip_name_is_micro800(uint16_t vendor_id, const char *product_name)
{
    /* Rockwell Automation is vendor 1. Micro800 catalogs are 2080- and 2085-
     * (a Micro850 is 2080-L50E-24QWB). Some firmware puts the family name
     * in the product string instead of the catalog. */
    if (vendor_id != 0x0001u) {
        return 0;
    }
    if (ContainsNoCase(product_name, "2080-") || ContainsNoCase(product_name, "2085-")) {
        return 1;
    }
    return ContainsNoCase(product_name, "micro8") || ContainsNoCase(product_name, "micro800");
}

int eip_name_is_compactlogix(uint16_t vendor_id, const char *product_name)
{
    if (vendor_id != 0x0001u) {
        return 0;
    }
    /* Micro800 catalogs share the Rockwell vendor id. */
    if (ContainsNoCase(product_name, "2080-") || ContainsNoCase(product_name, "2085-")) {
        return 0;
    }
    if (ContainsNoCase(product_name, "1769-") || ContainsNoCase(product_name, "5069-") ||
        ContainsNoCase(product_name, "1768-") || ContainsNoCase(product_name, "compactlogix")) {
        return 1;
    }
    return 0;
}

int eip_device_kind(uint16_t vendor_id, const char *product_name)
{
    if (eip_name_is_micro800(vendor_id, product_name)) {
        return EIP_KIND_MICRO800;
    }
    if (eip_name_is_compactlogix(vendor_id, product_name)) {
        return EIP_KIND_COMPACTLOGIX;
    }
    return EIP_KIND_ASSEMBLY;
}

/* Up to three decimal digits per part, leading zeros allowed. ip is written only on success. */
int eip_parse_ipv4(const char *text, uint8_t ip[4])
{
    uint8_t out[4];
    int part;

    if (text == 0 || ip == 0) {
        return EIP_ERR_ARG;
    }
    for (part = 0; part < 4; part++) {
        uint32_t value = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            value = value * 10u + (uint32_t)(*text - '0');
            if (++digits > 3 || value > 255u) {
                return EIP_ERR_ARG;
            }
            text++;
        }
        if (digits == 0) {
            return EIP_ERR_ARG;
        }
        out[part] = (uint8_t)value;
        if (part < 3) {
            if (*text != '.') {
                return EIP_ERR_ARG;
            }
            text++;
        }
    }
    if (*text != '\0') {
        return EIP_ERR_ARG;
    }
    ip[0] = out[0];
    ip[1] = out[1];
    ip[2] = out[2];
    ip[3] = out[3];
    return EIP_OK;
}

void eip_sanitize_product_name(char *text)
{
    uint16_t i;

    if (text == 0) {
        return;
    }
    for (i = 0; text[i] != '\0'; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' ||
            c == '_' || c == '-' || c == '.' || c == '/') {
            continue;
        }
        text[i] = '_';
    }
}

/* ---- symbol list ---- */

/* Service 0x55 on the symbol class, starting at the given instance. The data is an attribute count
 * and the attribute ids. The controller answers with the attributes in this order, which is the
 * record layout eip_parse_symbol_page expects, so the two must change together. */
int eip_build_symbol_list(uint8_t *out, uint16_t cap, uint16_t *out_len, uint16_t instance)
{
    uint8_t path[6];
    uint8_t attrs[10];

    path[0] = 0x20; /* 8-bit class */
    path[1] = 0x6B; /* symbol object */
    path[2] = 0x25; /* 16-bit instance */
    path[3] = 0x00;
    eip_store_le16(path + 4, instance);
    eip_store_le16(attrs + 0, 4);
    eip_store_le16(attrs + 2, 0x0002); /* symbol type */
    eip_store_le16(attrs + 4, 0x0007); /* element size */
    eip_store_le16(attrs + 6, 0x0008); /* dimensions */
    eip_store_le16(attrs + 8, 0x0001); /* name */
    return eip_build_cip_req(out, cap, EIP_CIP_LIST_SYMBOLS, path, 6, attrs, 10, out_len);
}

/* Bytes kept in a stored symbol name. Unlike one tag path segment this allows '.', '[' and ']'. */
static int TagCharOk(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '.' || c == '[' || c == ']' || c == ':';
}

/* One reply record, following the attribute order of eip_build_symbol_list:
 *    0  instance id, 32 bits
 *    4  symbol type (attribute 2), stored raw
 *    6  element size (attribute 7)
 *    8  three 32-bit dimensions (attribute 8)
 *   20  name length, 16 bits
 *   22  name (attribute 1), no terminator and no pad
 * The walk stops after 512 records, or when fewer than 22 bytes remain, so a trailing fragment
 * shorter than a record header is ignored without error. A zero name length or a name past the
 * end of the data is EIP_ERR_PROTOCOL.
 *
 * Names are cut to 95 characters and other bytes become '_'. A record whose cleaned name is
 * already in symbols is dropped. That covers earlier pages too, so a page that overlaps the last
 * one adds nothing. */
int eip_parse_symbol_page(const uint8_t *data, uint16_t data_len, EipSymbol *symbols, uint16_t cap,
                          uint16_t *count, uint16_t *parsed, uint32_t *last_instance)
{
    uint16_t offset = 0;

    if (data == 0 || symbols == 0 || count == 0 || parsed == 0 || last_instance == 0) {
        return EIP_ERR_ARG;
    }
    *parsed = 0;
    while ((uint32_t)offset + 22u <= data_len && *parsed < 512u) {
        uint32_t instance = eip_load_le32(data + offset);
        uint16_t symbol_type = eip_load_le16(data + offset + 4);
        uint16_t element_size = eip_load_le16(data + offset + 6);
        uint32_t dim0 = eip_load_le32(data + offset + 8);
        uint32_t dim1 = eip_load_le32(data + offset + 12);
        uint32_t dim2 = eip_load_le32(data + offset + 16);
        uint16_t name_len = eip_load_le16(data + offset + 20);
        uint16_t name_at;
        char name[96];
        uint16_t n = 0;
        uint16_t i;
        int duplicate = 0;
        unsigned long long d0;
        unsigned long long d1;
        unsigned long long d2;
        unsigned long long total;

        offset = (uint16_t)(offset + 22u);
        if (name_len == 0 || (uint32_t)offset + name_len > data_len) {
            return EIP_ERR_PROTOCOL;
        }
        name_at = offset;
        offset = (uint16_t)(offset + name_len);
        for (i = 0; i < name_len && (uint16_t)(n + 1u) < sizeof name; i++) {
            unsigned char ch = data[name_at + i];
            name[n++] = TagCharOk(ch) ? (char)ch : '_';
        }
        name[n] = '\0';
        /* Counted before the store below, so a record refused for a full array is included. */
        *parsed = (uint16_t)(*parsed + 1u);
        *last_instance = instance;
        if (name[0] == '\0') {
            continue;
        }
        for (i = 0; i < *count; i++) {
            if (strcmp(symbols[i].name, name) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (*count >= cap) {
            return EIP_ERR_TOO_BIG;
        }
        memset(&symbols[*count], 0, sizeof symbols[*count]);
        memcpy(symbols[*count].name, name, (uint16_t)(n + 1u));
        symbols[*count].symbol_type = symbol_type;
        symbols[*count].element_size = element_size;
        symbols[*count].dim0 = dim0;
        symbols[*count].dim1 = dim1;
        symbols[*count].dim2 = dim2;
        /* Element count: the product of the dimensions, a 0 dimension counting as 1, saturated
         * at 32 bits. A one-element array therefore has is_array 0. */
        d0 = (dim0 == 0) ? 1ull : dim0;
        d1 = (dim1 == 0) ? 1ull : dim1;
        d2 = (dim2 == 0) ? 1ull : dim2;
        if (d1 != 0ull && d0 > 0xFFFFFFFFull / d1) {
            total = 0xFFFFFFFFull;
        } else {
            total = d0 * d1;
            if (d2 != 0ull && total > 0xFFFFFFFFull / d2) {
                total = 0xFFFFFFFFull;
            } else {
                total *= d2;
            }
        }
        symbols[*count].element_count = (uint32_t)total;
        symbols[*count].is_array = (total > 1ull) ? 1u : 0u;
        *count = (uint16_t)(*count + 1u);
    }
    return EIP_OK;
}
