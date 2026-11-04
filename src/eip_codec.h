/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Wire-format constants and helpers, implemented in eip_codec.c. Shared by the scanner and the
 * adapter. Not public API. Packet layouts are described at each function in eip_codec.c.
 *
 * Builders take a buffer and capacity and report the length written through out_len or used. They
 * return EIP_OK, EIP_ERR_ARG, EIP_ERR_TOO_BIG or EIP_ERR_PATH. Parsers mostly return EIP_OK or
 * EIP_ERR_PROTOCOL. Returned data pointers refer into the input buffer. */
#ifndef EIP_CODEC_H_
#define EIP_CODEC_H_

#include "eip.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Encapsulation commands. */
    EIP_CMD_LIST_IDENTITY = 0x0063,
    EIP_CMD_REGISTER = 0x0065,
    EIP_CMD_UNREGISTER = 0x0066,
    EIP_CMD_SEND_RR = 0x006F,
    /* CPF item types. */
    EIP_ITEM_NULL = 0x0000,
    EIP_ITEM_IDENTITY = 0x000C,
    EIP_ITEM_CONNECTED = 0x00B1,
    EIP_ITEM_UNCONNECTED = 0x00B2,
    EIP_ITEM_SOCK_O2T = 0x8000,
    EIP_ITEM_SOCK_T2O = 0x8001,
    EIP_ITEM_SEQUENCED = 0x8002,
    EIP_ITEM_CONN_ADDR = 0x00A1,
    /* CIP service codes. A code means different things on different objects, which is why
     * READ_FRAG (tag services) and UNCONNECTED_SEND (Connection Manager) are both 0x52. */
    EIP_CIP_READ_TAG = 0x4C,
    EIP_CIP_WRITE_TAG = 0x4D,
    EIP_CIP_READ_FRAG = 0x52,
    EIP_CIP_WRITE_FRAG = 0x53,
    EIP_CIP_FORWARD_CLOSE = 0x4E,
    EIP_CIP_LARGE_FORWARD_OPEN = 0x5B,
    EIP_CIP_UNCONNECTED_SEND = 0x52,
    EIP_CIP_GET_ATTRIBUTE = 0x0E,
    EIP_CIP_SET_ATTRIBUTE = 0x10,
    EIP_CIP_LIST_SYMBOLS = 0x55,
    /* A CIP general status, not a service: more data follows this reply. */
    EIP_CIP_PARTIAL = 0x06
};

/* A parsed encapsulation header. body points into the packet that was parsed. */
typedef struct EipEncapView {
    uint16_t command;
    uint16_t length;
    uint32_t session;
    uint32_t status;
    uint8_t context[8];
    const uint8_t *body;
} EipEncapView;

/* Fixed fields of a Large_Forward_Open request. */
typedef struct EipForwardOpenInfo {
    uint8_t priority_tick;      /* from eip_tick_pair */
    uint8_t timeout_ticks;      /* from eip_tick_pair */
    uint32_t o2t_id;            /* proposed; the reply holds the id to use */
    uint32_t t2o_id;            /* proposed; the reply holds the id to use */
    uint16_t connection_serial;
    uint16_t vendor_id;
    uint32_t originator_serial;
    uint8_t timeout_multiplier; /* 0..7: x4, x8, x16, ... */
    uint32_t o2t_rpi_us;
    uint32_t o2t_params;        /* from eip_large_conn_params */
    uint32_t t2o_rpi_us;
    uint32_t t2o_params;        /* from eip_large_conn_params */
    uint8_t transport;          /* transport class and trigger; 0x01 is Class 1, cyclic */
} EipForwardOpenInfo;

/* Encode timeout_ms as tick and ticks, where the timeout is 2^tick x ticks ms. */
void eip_tick_pair(uint32_t timeout_ms, uint8_t *tick, uint8_t *ticks);

/* 32-bit network connection parameters for a Large_Forward_Open. conn_type 2 is point-to-point. */
uint32_t eip_large_conn_params(uint16_t size_bytes, int variable, int priority, int conn_type,
                               int redundant_owner);

/* Tag name to symbolic EPATH. EIP_ERR_PATH on a name it cannot encode. */
int eip_build_tag_path(const char *tag, uint8_t *out, uint16_t cap, uint16_t *out_len);

/* Append a logical segment at *used, which advances. segment is the 8-bit form (0x20 class,
 * 0x24 instance, 0x2C connection point, 0x30 attribute). value is at most 65535. */
int eip_put_logical(uint8_t *out, uint16_t cap, uint16_t *used, uint8_t segment, uint32_t value);

/* Header plus body. body may be 0 when body_len is 0. */
int eip_build_encap(uint8_t *out, uint16_t cap, uint16_t command, uint32_t session,
                    const uint8_t context[8], const uint8_t *body, uint16_t body_len,
                    uint16_t *out_len);

/* packet_len must equal the header plus the claimed body length. */
int eip_parse_encap(const uint8_t *packet, uint16_t packet_len, EipEncapView *view);

/* path_len is in bytes and must be even. */
int eip_build_cip_req(uint8_t *out, uint16_t cap, uint8_t service, const uint8_t *path,
                      uint16_t path_len, const uint8_t *data, uint16_t data_len, uint16_t *out_len);

/* EIP_OK for any well-formed reply: a nonzero *status is for the caller to judge. */
int eip_parse_cip_rsp(const uint8_t *message, uint16_t len, uint8_t *service, uint8_t *status,
                      uint16_t *extended, const uint8_t **data, uint16_t *data_len);

/* Wrap a CIP message in Unconnected Send, routed over backplane port 1 to slot. */
int eip_wrap_unconnected(uint8_t *out, uint16_t cap, const uint8_t *inner, uint16_t inner_len,
                         uint8_t slot, uint8_t tick, uint8_t ticks, uint16_t *out_len);

/* SendRRData body. extra is extra_items encoded CPF items placed after the CIP item. */
int eip_build_rr_body(uint8_t *out, uint16_t cap, uint16_t timeout_sec, const uint8_t *cip,
                      uint16_t cip_len, const uint8_t *extra, uint16_t extra_len,
                      uint16_t extra_items, uint16_t *out_len);

/* First CPF item of a type. cpf starts at the item count. EIP_ERR_PROTOCOL when it is not found
 * or the list is malformed. */
int eip_cpf_find(const uint8_t *cpf, uint16_t len, uint16_t type, const uint8_t **data,
                 uint16_t *data_len);

/* A full 20-byte sockaddr item. item_type is EIP_ITEM_SOCK_O2T or EIP_ITEM_SOCK_T2O. */
int eip_build_sockaddr_item(uint8_t *out, uint16_t cap, uint16_t *out_len, uint16_t item_type,
                            const uint8_t ip[4], uint16_t port);

/* data is the item data from eip_cpf_find, without the type and length. */
int eip_parse_sockaddr(const uint8_t *data, uint16_t len, uint8_t ip[4], uint16_t *port);

/* Connection path for Class 1: assembly class, config instance, O->T then T->O connection point. */
int eip_build_assembly_path(uint8_t *out, uint16_t cap, uint16_t *out_len, uint16_t config_instance,
                            uint16_t o2t_point, uint16_t t2o_point);

/* A complete Large_Forward_Open CIP message. path_len is even and at most 40, else EIP_ERR_ARG. */
int eip_build_forward_open_request(uint8_t *out, uint16_t cap, uint16_t *out_len,
                                   const EipForwardOpenInfo *info, const uint8_t *conn_path,
                                   uint16_t path_len);

/* A complete Forward_Close CIP message. path_len is even and at most 64, else EIP_ERR_ARG. */
int eip_build_forward_close_request(uint8_t *out, uint16_t cap, uint16_t *out_len,
                                    uint8_t priority_tick, uint8_t timeout_ticks,
                                    uint16_t connection_serial, uint16_t vendor_id,
                                    uint32_t originator_serial, const uint8_t *conn_path,
                                    uint16_t path_len);

/* data is the reply data after the CIP header. o2t_api and t2o_api are the actual packet intervals
 * in microseconds. */
int eip_parse_forward_open_success(const uint8_t *data, uint16_t len, uint32_t *o2t_id,
                                   uint32_t *t2o_id, uint32_t *o2t_api, uint32_t *t2o_api);

/* A Class 1 datagram. When run_idle is set, a run/idle header carrying run precedes data. */
int eip_build_io_packet(uint8_t *out, uint16_t cap, uint16_t *out_len, uint32_t connection_id,
                        uint32_t sequence_32, uint16_t sequence_16, int run_idle, int run,
                        const uint8_t *data, uint16_t data_len);

/* Like eip_parse_io_packet, plus the 32-bit encapsulation sequence number from a Sequenced Address item
 * (has_seq32 = 0 when the packet only carries a Connected Address item). sequence is the 16-bit CIP count. */
int eip_parse_io_packet_seq(const uint8_t *packet, uint16_t len, uint32_t *connection_id, uint32_t *seq32,
                            int *has_seq32, uint16_t *sequence, const uint8_t **data, uint16_t *data_len);
int eip_parse_io_packet(const uint8_t *packet, uint16_t len, uint32_t *connection_id,
                        uint16_t *sequence, const uint8_t **data, uint16_t *data_len);

/* body is the encapsulation body of a ListIdentity reply. */
int eip_parse_identity(const uint8_t *body, uint16_t len, EipIdentity *identity);

/* Rockwell vendor 1 and a Micro800 catalog or family name. */
int eip_name_is_micro800(uint16_t vendor_id, const char *product_name);

/* Rockwell vendor 1 and a CompactLogix catalog (1768-, 1769-, 5069-) or name. */
int eip_name_is_compactlogix(uint16_t vendor_id, const char *product_name);

/* Keeps letters, digits, space, and _ - . / . Other bytes become '_'. */
void eip_sanitize_product_name(char *text);

/* CIP service 0x55 request: symbol class 0x6B, 16-bit instance, attributes
 * 2 (type), 7 (element size), 8 (dimensions), 1 (name). */
int eip_build_symbol_list(uint8_t *out, uint16_t cap, uint16_t *out_len, uint16_t instance);

/* Append symbol records from one list-tags page. count is in/out.
 * parsed is the number of records consumed, including ones not stored.
 * last_instance is the last instance id consumed. A full array returns
 * EIP_ERR_TOO_BIG after storing what fits. A record with an empty name
 * or a name past the end of the data returns EIP_ERR_PROTOCOL and keeps
 * the records already stored. Trailing bytes too short to hold a record
 * header are ignored. */
int eip_parse_symbol_page(const uint8_t *data, uint16_t data_len, EipSymbol *symbols, uint16_t cap,
                          uint16_t *count, uint16_t *parsed, uint32_t *last_instance);

#ifdef __cplusplus
}
#endif

#endif /* EIP_CODEC_H_ */
