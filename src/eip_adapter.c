/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* The sans-I/O adapter (target) core: encapsulation commands, the CIP objects and one Class 1
 * connection. It owns no socket and no clock. eip_server.c, or a caller's own event loop, hands it
 * packets and a tick count and sends what comes back. eip_codec.c does the shared wire encoding.
 *
 * The file runs from the bottom of the stack to the top:
 *
 *   assembly table and setters    the configuration API from eip_adapter.h
 *   reply helpers                 encapsulation and CIP replies, ListIdentity, ListServices
 *   WalkPath, ParseCip            EPATH and CIP request parsing
 *   HandleIdentity, HandleAssembly
 *   AcceptOpen, HandleForwardClose   the Connection Manager
 *   HandleNetObject               TCP/IP Interface and Ethernet Link
 *   HandleMultiple, HandleCip     Multiple Service Packet and the CIP dispatch
 *   eip_adapter_on_encap          entry point for TCP and UDP 44818
 *   eip_adapter_take_input_from, eip_adapter_build_output, eip_adapter_check_timeout
 *                                 the Class 1 data path on UDP 2222
 *
 * Nothing here locks. s_body is shared by every adapter in the program, so the caller serializes
 * all calls (EipServer holds eip_hal_lock around them). */
#include "eip_adapter.h"
#include "eip_codec.h"

#include <string.h>

enum {
    /* Encapsulation commands only the adapter uses. The others are in eip_codec.h. */
    EIP_CMD_NOP = 0x0000,
    EIP_CMD_LIST_SERVICES = 0x0004,
    EIP_CMD_LIST_INTERFACES = 0x0064,
    /* CIP service codes. */
    EIP_CIP_GET_ALL = 0x01,
    EIP_CIP_MULTIPLE = 0x0A, /* Multiple Service Packet (Message Router) */
    EIP_CIP_FORWARD_OPEN = 0x54,
    /* Encapsulation status codes, sent in the status field of the encapsulation header. */
    EIP_ENCAP_BAD_COMMAND = 0x0001,
    EIP_ENCAP_BAD_DATA = 0x0003,
    EIP_ENCAP_BAD_SESSION = 0x0064,
    EIP_ENCAP_BAD_LENGTH = 0x0065,
    EIP_ENCAP_BAD_VERSION = 0x0069,
    /* CIP general status codes, with one exception: EIP_CIP_RESET is the Identity Reset service
     * code, which happens to share 0x05 with EIP_CIP_PATH_UNKNOWN. */
    EIP_CIP_PATH_UNKNOWN = 0x05,
    EIP_CIP_NO_OBJECT = 0x16, /* class or instance does not exist (what a real FEN20 answers) */
    EIP_CIP_SERVICE = 0x08,
    EIP_CIP_NOT_SETTABLE = 0x0E,
    EIP_CIP_RESET = 0x05,
    EIP_CIP_INVALID_VALUE = 0x09,
    EIP_CIP_STATE_CONFLICT = 0x0C,
    EIP_CIP_INVALID_PARAM = 0x20,
    EIP_CIP_NOT_ENOUGH = 0x13,
    EIP_CIP_ATTR = 0x14,
    EIP_CIP_TOO_MUCH = 0x15,
    EIP_CIP_CONNECTION = 0x01, /* connection failure; the EIP_CM_ code below is the extended status */
    EIP_CIP_REPLY_TOO_LARGE = 0x11,
    EIP_CIP_EMBEDDED_ERROR = 0x1E, /* Multiple Service Packet: an embedded service failed */
    /* Connection Manager extended status codes, sent under general status 0x01. */
    EIP_CM_CLASS_TRIGGER = 0x0103,
    EIP_CM_OWNERSHIP = 0x0106,
    EIP_CM_NOT_FOUND = 0x0107,
    EIP_CM_BAD_PARAM = 0x0108,
    EIP_CM_OUT_OF_CONNECTIONS = 0x0113,
    EIP_CM_VENDOR = 0x0114, /* vendor id or product code mismatch */
    EIP_CM_TYPE = 0x0115,
    EIP_CM_REVISION = 0x0116,
    EIP_CM_BAD_PATH = 0x0117,
    EIP_CM_CLASS_UNSUPPORTED = 0x011C,
    EIP_CM_TRIGGER = 0x011D,
    EIP_CM_DIRECTION = 0x011E,
    EIP_CM_OT_FIXED = 0x011F,
    EIP_CM_TO_FIXED = 0x0120,
    EIP_CM_OT_TYPE = 0x0123,
    EIP_CM_TO_TYPE = 0x0124,
    EIP_CM_REDUNDANT = 0x0125,
    EIP_CM_CONFIG_SIZE = 0x0126,
    EIP_CM_OT_SIZE = 0x0127,
    EIP_CM_TO_SIZE = 0x0128,
    EIP_CM_CONFIG_PATH = 0x0129,
    EIP_CM_CONSUME_PATH = 0x012A,
    EIP_CM_PRODUCE_PATH = 0x012B
};

/* Encapsulation reply body under construction. Nothing in it outlives one eip_adapter_on_encap call,
 * but it is the reason calls on all adapters must be serialized. */
static uint8_t s_body[EIP_MAX_PACKET];

static EipAsm *FindAsm(EipAdapter *adapter, uint16_t instance);

/* Identity status word (attr 5, also sent in ListIdentity). Bit 0 owned, bit 2 configured, bits 4-7
 * extended device status: 3 no I/O connection, 6 a connection in run, 7 connected but all idle.
 * With no connection the run flag is forced to 0 as well. */
static void RefreshStatus(EipAdapter *adapter)
{
    if (adapter->io_open == 0) {
        adapter->identity_status = 0x0034; /* configured, no I/O connection (real FEN20: 0x0034) */
        adapter->run = 0;
        return;
    }
    if (adapter->run) {
        adapter->identity_status = 0x0065; /* owned, configured, run */
    } else {
        adapter->identity_status = 0x0075; /* owned, configured, idle */
    }
}

/* Defined "no data" state for the consumed (O->T) assembly: all zero. */
static void ClearConsumed(EipAdapter *adapter)
{
    uint8_t i;
    for (i = 0; i < adapter->asm_count; i++) {
        if (adapter->asms[i].instance == adapter->o2t_instance && adapter->asms[i].direction == EIP_ASM_CONSUMED) {
            memset(adapter->asms[i].data, 0, adapter->asms[i].size);
        }
    }
}

/* Back to no connection. Reached from Forward_Close, the inactivity watchdog, a reopen by the same
 * originator and eip_adapter_clear_assemblies. The triad and the header/null flags keep their old
 * values; everything that reads them checks io_open first. */
static void CloseIo(EipAdapter *adapter)
{
    ClearConsumed(adapter);                  /* before o2t_instance is forgotten */
    adapter->o2t_seq_valid = 0;
    adapter->o2t_seq16_valid = 0;
    memset(adapter->o2t_source_ip, 0, 4);
    adapter->io_open = 0;
    adapter->o2t_id = 0;
    adapter->t2o_id = 0;
    adapter->o2t_instance = 0;
    adapter->t2o_instance = 0;
    adapter->originator_port = 0;
    adapter->t2o_multicast = 0;
    memset(adapter->originator_ip, 0, 4);
    RefreshStatus(adapter);
}

/* Append an assembly and carve its data from the shared pool. Only eip_adapter_clear_assemblies gives
 * pool space back. A full table or pool is ignored silently: eip_adapter_add_assembly reports it. */
static void AddAssembly(EipAdapter *adapter, uint16_t instance, uint8_t direction, uint16_t size,
                        const char *name, const uint8_t *initial)
{
    EipAsm *slot;

    if (adapter->asm_count >= EIP_ADAPTER_MAX_ASSEMBLIES ||
        (uint32_t)adapter->pool_used + size > (uint32_t)EIP_ADAPTER_DATA_POOL) {
        return;
    }
    slot = &adapter->asms[adapter->asm_count++];
    memset(slot, 0, sizeof *slot);
    slot->instance = instance;
    slot->direction = direction;
    slot->size = size;
    slot->data = adapter->pool + adapter->pool_used;
    adapter->pool_used = (uint16_t)(adapter->pool_used + size);
    memset(slot->data, 0, size);
    if (initial != 0 && size != 0) {
        memcpy(slot->data, initial, size);
    }
    if (name != 0) {
        uint16_t n = 0;
        while (name[n] != '\0' && n + 1u < sizeof slot->name) {
            slot->name[n] = name[n];
            n++;
        }
        slot->name[n] = '\0';
    }
}

/* Starts as the built-in test device (vendor 65000, assemblies 100, 101, 102, 150). The 20 ticks/s
 * clock and 2000 ms timeout floor suit a NetBurner tick; eip_server.c replaces both. */
void eip_adapter_init(EipAdapter *adapter, const uint8_t ip[4], uint32_t serial_number)
{
    static const uint8_t input_bytes[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    static const uint8_t status_bytes[4] = {0x01, 0x00, 0x00, 0x00};

    if (adapter == 0) {
        return;
    }
    memset(adapter, 0, sizeof *adapter);
    adapter->vendor_id = 65000;
    adapter->device_type = 12; /* communications adapter */
    adapter->product_code = 1;
    adapter->revision_major = 1;
    adapter->revision_minor = 1;
    adapter->serial_number = (serial_number == 0) ? 1u : serial_number;
    adapter->state = 3; /* operational */
    adapter->ticks_per_second = 20;
    adapter->next_session = 1;
    adapter->next_t2o_id = 0x00010001u;
    memcpy(adapter->product_name, "EIP Test Adapter", 17);
    if (ip != 0) {
        memcpy(adapter->ip, ip, 4);
    }
    adapter->netmask[0] = 255;
    adapter->netmask[1] = 255;
    adapter->netmask[2] = 255;
    adapter->netmask[3] = 0;
    adapter->timeout_floor_ms = 2000;
    AddAssembly(adapter, 100, EIP_ASM_CONSUMED, 8, "Output", 0);
    AddAssembly(adapter, 101, EIP_ASM_PRODUCED, 8, "Input", input_bytes);
    AddAssembly(adapter, 102, EIP_ASM_PRODUCED, 4, "Status", status_bytes);
    AddAssembly(adapter, 150, EIP_ASM_CONFIG, 4, "Config", 0);
    RefreshStatus(adapter);
}

void eip_adapter_set_ip(EipAdapter *adapter, const uint8_t ip[4])
{
    if (adapter != 0 && ip != 0) {
        memcpy(adapter->ip, ip, 4);
    }
}

void eip_adapter_set_clock(EipAdapter *adapter, uint32_t ticks_per_second)
{
    if (adapter != 0 && ticks_per_second != 0) {
        adapter->ticks_per_second = ticks_per_second;
    }
}

void eip_adapter_set_identity(EipAdapter *adapter, uint16_t vendor_id, uint16_t device_type,
                              uint16_t product_code, uint8_t revision_major, uint8_t revision_minor,
                              const char *product_name)
{
    if (adapter == 0) {
        return;
    }
    adapter->vendor_id = vendor_id;
    adapter->device_type = device_type;
    adapter->product_code = product_code;
    adapter->revision_major = revision_major;
    adapter->revision_minor = revision_minor;
    if (product_name != 0) {
        uint16_t n = 0;
        while (product_name[n] != '\0' && n + 1u < sizeof adapter->product_name) {
            adapter->product_name[n] = product_name[n];
            n++;
        }
        adapter->product_name[n] = '\0';
    }
}

void eip_adapter_clear_assemblies(EipAdapter *adapter)
{
    if (adapter != 0) {
        CloseIo(adapter); /* an open connection refers to the assemblies being removed */
        memset(adapter->asms, 0, sizeof adapter->asms);
        adapter->asm_count = 0;
        adapter->pool_used = 0;
    }
}

int eip_adapter_add_assembly(EipAdapter *adapter, uint16_t instance, uint8_t direction, uint16_t size,
                             const char *name, const uint8_t *initial)
{
    uint8_t before;

    if (adapter == 0 || instance == 0 || direction < EIP_ASM_CONSUMED || direction > EIP_ASM_CONFIG) {
        return EIP_ERR_ARG;
    }
    if (FindAsm(adapter, instance) != 0) {
        return EIP_ERR_ARG;
    }
    /* AddAssembly returns nothing: a count that did not grow means the table or pool is full. */
    before = adapter->asm_count;
    AddAssembly(adapter, instance, direction, size, name, initial);
    return (adapter->asm_count != before) ? EIP_OK : EIP_ERR_TOO_BIG;
}

int eip_adapter_set_implicit_only(EipAdapter *adapter, uint16_t instance, int implicit_only)
{
    EipAsm *slot = (adapter != 0) ? FindAsm(adapter, instance) : 0;

    if (slot == 0) {
        return EIP_ERR_PATH;
    }
    slot->implicit_only = implicit_only ? 1u : 0u;
    return EIP_OK;
}

void eip_adapter_set_netmask(EipAdapter *adapter, const uint8_t netmask[4])
{
    if (adapter != 0 && netmask != 0) {
        memcpy(adapter->netmask, netmask, 4);
    }
}

void eip_adapter_set_timeout_floor_ms(EipAdapter *adapter, uint32_t floor_ms)
{
    if (adapter != 0) {
        adapter->timeout_floor_ms = floor_ms;
    }
}

void eip_adapter_set_interface(EipAdapter *adapter, const uint8_t mac[6], const uint8_t gateway[4],
                               uint8_t config_control, const char *host_name)
{
    if (adapter == 0) {
        return;
    }
    if (mac != 0) {
        memcpy(adapter->mac, mac, 6);
    }
    if (gateway != 0) {
        memcpy(adapter->gateway, gateway, 4);
    }
    adapter->ip_config_control = config_control;
    adapter->ip_config_capability = 0x04u; /* DHCP client capable; not settable over CIP */
    if (host_name != 0) {
        uint16_t n = 0;
        while (host_name[n] != '\0' && n + 1u < sizeof adapter->host_name) {
            adapter->host_name[n] = host_name[n];
            n++;
        }
        adapter->host_name[n] = '\0';
    }
}

void eip_adapter_set_callbacks(EipAdapter *adapter, EipSetInterfaceFn set_interface, EipResetFn reset,
                               void *ctx)
{
    if (adapter != 0) {
        adapter->set_interface = set_interface;
        adapter->reset = reset;
        adapter->callback_ctx = ctx;
    }
}

void eip_adapter_set_dns(EipAdapter *adapter, const uint8_t dns1[4], const uint8_t dns2[4], const char *domain)
{
    if (adapter == 0) {
        return;
    }
    if (dns1 != 0) {
        memcpy(adapter->dns1, dns1, 4);
    }
    if (dns2 != 0) {
        memcpy(adapter->dns2, dns2, 4);
    }
    if (domain != 0) {
        uint16_t n = 0;
        while (domain[n] != '\0' && n + 1u < sizeof adapter->domain) {
            adapter->domain[n] = domain[n];
            n++;
        }
        adapter->domain[n] = '\0';
    }
}

void eip_adapter_set_link(EipAdapter *adapter, int link_up, uint32_t speed_mbps, int full_duplex,
                          int autonegotiated)
{
    if (adapter == 0) {
        return;
    }
    adapter->link_speed_mbps = link_up ? speed_mbps : 0;
    /* Interface flags: bit 0 link up, bit 1 full duplex, bits 2-4 negotiation status (3 negotiated,
     * 4 forced, 0 while the link is down),
     * bit 5 "manual setting requires reset" as on the real FEN20 (0x2F when up, 100 FD, negotiated) */
    adapter->link_flags = (link_up ? 0x01u : 0) | ((link_up && full_duplex) ? 0x02u : 0) |
                          ((link_up ? (autonegotiated ? 3u : 4u) : 0u) << 2) | 0x20u;
}

void eip_adapter_multicast_address(const uint8_t ip[4], const uint8_t netmask[4], uint8_t out[4])
{
    uint32_t host = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
    uint32_t mask = ((uint32_t)netmask[0] << 24) | ((uint32_t)netmask[1] << 16) |
                    ((uint32_t)netmask[2] << 8) | netmask[3];
    uint32_t index = ((host & ~mask) - 1u) & 0x3FFu; /* host id - 1, wrapped to 1024 slots */
    uint32_t group = 0xEFC00100u + (index << 5);     /* 239.192.1.0, 32 addresses per slot */

    out[0] = (uint8_t)(group >> 24);
    out[1] = (uint8_t)(group >> 16);
    out[2] = (uint8_t)(group >> 8);
    out[3] = (uint8_t)group;
}

static EipAsm *FindAsm(EipAdapter *adapter, uint16_t instance)
{
    uint8_t i;

    for (i = 0; i < adapter->asm_count; i++) {
        if (adapter->asms[i].instance == instance) {
            return &adapter->asms[i];
        }
    }
    return 0;
}

/* Application access to assembly data, in any direction, with no effect on connection state.
 * copy needs a buffer at least the assembly's size; store needs exactly that size. */
int eip_adapter_copy(const EipAdapter *adapter, uint16_t instance, uint8_t *data, uint16_t *data_len)
{
    uint8_t i;

    if (adapter == 0 || data == 0 || data_len == 0) {
        return EIP_ERR_ARG;
    }
    /* Not FindAsm: that takes a non-const adapter. */
    for (i = 0; i < adapter->asm_count; i++) {
        if (adapter->asms[i].instance == instance) {
            if (*data_len < adapter->asms[i].size) {
                return EIP_ERR_TOO_BIG;
            }
            memcpy(data, adapter->asms[i].data, adapter->asms[i].size);
            *data_len = adapter->asms[i].size;
            return EIP_OK;
        }
    }
    return EIP_ERR_PATH;
}

int eip_adapter_store(EipAdapter *adapter, uint16_t instance, const uint8_t *data, uint16_t data_len)
{
    EipAsm *slot;

    if (adapter == 0 || (data_len != 0 && data == 0)) {
        return EIP_ERR_ARG;
    }
    slot = FindAsm(adapter, instance);
    if (slot == 0) {
        return EIP_ERR_PATH;
    }
    if (data_len != slot->size) {
        return EIP_ERR_ARG;
    }
    if (data_len != 0) {
        memcpy(slot->data, data, data_len);
    }
    return EIP_OK;
}

/* Encapsulation reply echoing the request's command and sender context. session is separate because
 * RegisterSession answers with the new handle, or 0 on failure. eip_build_encap always writes status
 * 0, so the real status is patched in at header offset 8. */
static int EncapReply(const EipEncapView *request, uint32_t session, uint32_t status, const uint8_t *body,
                      uint16_t body_len, uint8_t *response, uint16_t cap, uint16_t *response_len)
{
    int rc = eip_build_encap(response, cap, request->command, session, request->context, body, body_len,
                             response_len);
    if (rc != EIP_OK) {
        return rc;
    }
    eip_store_le32(response + 8, status);
    return EIP_OK;
}

/* CIP reply header: service | 0x80, reserved, general status, number of extended status words, then
 * the extended status if there is one. cap must hold the 6-byte form even when the 4-byte form is
 * sent, which is why kMultipleEntryMin is 6. CipOk below is the same header followed by data. */
static int CipStatus(uint8_t *out, uint16_t cap, uint16_t *out_len, uint8_t service, uint8_t status,
                     uint16_t extended)
{
    if (cap < 6u) {
        return EIP_ERR_TOO_BIG;
    }
    out[0] = (uint8_t)(service | 0x80u);
    out[1] = 0;
    out[2] = status;
    if (extended == 0) {
        out[3] = 0;
        *out_len = 4;
        return EIP_OK;
    }
    out[3] = 1;
    eip_store_le16(out + 4, extended);
    *out_len = 6;
    return EIP_OK;
}

static int CipOk(uint8_t *out, uint16_t cap, uint16_t *out_len, uint8_t service, const uint8_t *data,
                 uint16_t data_len)
{
    if ((uint32_t)data_len + 4u > cap) {
        return EIP_ERR_TOO_BIG;
    }
    out[0] = (uint8_t)(service | 0x80u);
    out[1] = 0;
    out[2] = 0;
    out[3] = 0;
    if (data_len != 0 && data != 0) {
        memcpy(out + 4, data, data_len);
    }
    *out_len = (uint16_t)(4u + data_len);
    return EIP_OK;
}

/* SendRRData reply around a CIP reply: interface handle 0, timeout 0, then the CPF list of a null
 * address item, the unconnected data item and extra_items more items (the sockaddr items of a
 * Forward_Open reply). */
static int ReplyCip(const EipEncapView *request, const uint8_t *cip, uint16_t cip_len, const uint8_t *extra,
                    uint16_t extra_len, uint16_t extra_items, uint8_t *response, uint16_t cap,
                    uint16_t *response_len)
{
    uint16_t body_len = 0;
    int rc = eip_build_rr_body(s_body, sizeof s_body, 0, cip, cip_len, extra, extra_len, extra_items,
                               &body_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return EncapReply(request, request->session, 0, s_body, body_len, response, cap, response_len);
}

/* Assembly class attr 2. Instance numbers are sparse, so this is not asm_count. */
static uint16_t HighestInstance(const EipAdapter *adapter)
{
    uint8_t i;
    uint16_t highest = 0;

    for (i = 0; i < adapter->asm_count; i++) {
        if (adapter->asms[i].instance > highest) {
            highest = adapter->asms[i].instance;
        }
    }
    return highest;
}

/* Body of the ListIdentity CPF item (type 0x0C):
 *
 *   0    UINT    encapsulation protocol version (1)
 *   2    16      sockaddr, big-endian: family 2, port 44818, IP, 8 zero bytes
 *   18   UINT    vendor id
 *   20   UINT    device type
 *   22   UINT    product code
 *   24   2       revision major, minor
 *   26   WORD    identity status
 *   28   UDINT   serial number
 *   32   SHORT_STRING product name: length byte, then the characters
 *   33+n USINT   identity state
 */
static int BuildIdentityItem(const EipAdapter *adapter, uint8_t *item, uint16_t cap, uint16_t *item_len)
{
    uint8_t name_len = 0;
    uint16_t used;

    while (adapter->product_name[name_len] != '\0' && name_len < 32u) {
        name_len++;
    }
    used = (uint16_t)(34u + name_len);
    if (used > cap) {
        return EIP_ERR_TOO_BIG;
    }
    memset(item, 0, used);
    eip_store_le16(item, 1);
    item[2] = 0x00;
    item[3] = 0x02;
    item[4] = (uint8_t)(EIP_TCP_PORT >> 8);
    item[5] = (uint8_t)(EIP_TCP_PORT & 0xFFu);
    memcpy(item + 6, adapter->ip, 4);
    eip_store_le16(item + 18, adapter->vendor_id);
    eip_store_le16(item + 20, adapter->device_type);
    eip_store_le16(item + 22, adapter->product_code);
    item[24] = adapter->revision_major;
    item[25] = adapter->revision_minor;
    eip_store_le16(item + 26, adapter->identity_status);
    eip_store_le32(item + 28, adapter->serial_number);
    item[32] = name_len;
    memcpy(item + 33, adapter->product_name, name_len);
    item[33 + name_len] = adapter->state;
    *item_len = (uint16_t)(used);
    return EIP_OK;
}

static int BuildListIdentity(const EipAdapter *adapter, uint8_t *body, uint16_t cap, uint16_t *body_len)
{
    uint8_t item[96];
    uint16_t item_len = 0;
    int rc = BuildIdentityItem(adapter, item, sizeof item, &item_len);

    if (rc != EIP_OK) {
        return rc;
    }
    if ((uint32_t)item_len + 6u > cap) {
        return EIP_ERR_TOO_BIG;
    }
    eip_store_le16(body, 1);
    eip_store_le16(body + 2, EIP_ITEM_IDENTITY);
    eip_store_le16(body + 4, item_len);
    memcpy(body + 6, item, item_len);
    *body_len = (uint16_t)(6u + item_len);
    return EIP_OK;
}

/* One CPF item of type 0x0100, length 20: UINT version (1), UINT capability flags, 16-byte name. */
static int BuildListServices(uint8_t *body, uint16_t cap, uint16_t *body_len)
{
    static const char name[16] = "Communications";

    if (cap < 26u) {
        return EIP_ERR_TOO_BIG;
    }
    eip_store_le16(body, 1);
    eip_store_le16(body + 2, 0x0100);
    eip_store_le16(body + 4, 20);
    eip_store_le16(body + 6, 1);
    eip_store_le16(body + 8, 0x0120); /* TCP encapsulation + class 0/1 UDP I/O (real FEN20: 0x0120) */
    memcpy(body + 10, name, 16);
    *body_len = 26;
    return EIP_OK;
}

/* What WalkPath found. Ordinary requests use class, instance and attribute. In a Forward_Open
 * connection path, instance is the configuration assembly, points are the connection points in path
 * order and config_data is the configuration data. config_data points into the request. */
typedef struct CipPath {
    uint16_t class_id;
    uint16_t instance;
    uint16_t attribute;
    uint8_t has_class;
    uint8_t has_instance;
    uint8_t has_attribute;
    uint8_t has_key;
    uint16_t key_vendor;
    uint16_t key_type;
    uint16_t key_product;
    uint8_t key_major;
    uint8_t key_minor;
    uint16_t points[4];
    uint8_t point_count;
    const uint8_t *config_data;
    uint16_t config_len;
} CipPath;

/* Decode a padded EPATH. The segment type is the top three bits of each segment's first byte.
 *
 *   type 0   port segment. Skipped, so a path that still carries a route is handled here. Bit 4
 *            means a link address with a size byte follows. Extended port ids (low nibble 0xF) are
 *            refused.
 *   type 1   logical segment. Bits 2-4 are the logical type: 0 class, 1 instance, 3 connection
 *            point, 4 attribute, 5 electronic key. Others are stepped over and dropped. Bits 0-1
 *            are the format: 0 8-bit, 1 16-bit after a pad byte, 2 32-bit after a pad byte. A value
 *            above 16 bits is refused.
 *   0x80     simple data segment: word count, then the data.
 *
 * Anything else, symbolic segments included, is EIP_ERR_PATH. */
static int WalkPath(const uint8_t *path, uint16_t len, CipPath *out)
{
    uint16_t offset = 0;

    memset(out, 0, sizeof *out);
    while (offset < len) {
        uint8_t seg = path[offset];
        uint8_t kind = (uint8_t)(seg >> 5);

        if (kind == 0) {
            if ((seg & 0x0Fu) == 0x0Fu) {
                return EIP_ERR_PATH;
            }
            if (seg & 0x10u) {
                uint8_t size;
                uint16_t step;
                if ((uint16_t)(offset + 2u) > len) {
                    return EIP_ERR_PATH;
                }
                size = path[offset + 1];
                step = (uint16_t)(2u + size + (size & 1u));
                if ((uint32_t)offset + step > len) {
                    return EIP_ERR_PATH;
                }
                offset = (uint16_t)(offset + step);
            } else {
                if ((uint16_t)(offset + 2u) > len) {
                    return EIP_ERR_PATH;
                }
                offset = (uint16_t)(offset + 2u);
            }
            continue;
        }
        if (kind == 1) {
            uint8_t format = (uint8_t)(seg & 0x03u);
            uint8_t logical = (uint8_t)((seg >> 2) & 0x07u);
            uint32_t value = 0;

            if (logical == 5) {
                /* Key format 4: vendor, device type, product code, major, minor. 10 bytes in all. */
                if ((uint32_t)offset + 10u > len || path[offset + 1] != 0x04u) {
                    return EIP_ERR_PATH;
                }
                out->has_key = 1;
                out->key_vendor = eip_load_le16(path + offset + 2);
                out->key_type = eip_load_le16(path + offset + 4);
                out->key_product = eip_load_le16(path + offset + 6);
                out->key_major = path[offset + 8];
                out->key_minor = path[offset + 9];
                offset = (uint16_t)(offset + 10u);
                continue;
            }
            if (format == 0) {
                if ((uint16_t)(offset + 2u) > len) {
                    return EIP_ERR_PATH;
                }
                value = path[offset + 1];
                offset = (uint16_t)(offset + 2u);
            } else if (format == 1) {
                if ((uint32_t)offset + 4u > len) {
                    return EIP_ERR_PATH;
                }
                value = eip_load_le16(path + offset + 2);
                offset = (uint16_t)(offset + 4u);
            } else if (format == 2) {
                if ((uint32_t)offset + 6u > len) {
                    return EIP_ERR_PATH;
                }
                value = eip_load_le32(path + offset + 2);
                offset = (uint16_t)(offset + 6u);
            } else {
                return EIP_ERR_PATH;
            }
            if (value > 65535u) {
                return EIP_ERR_PATH;
            }
            if (logical == 0) {
                out->class_id = (uint16_t)value;
                out->has_class = 1;
            } else if (logical == 1) {
                /* The first instance counts; a later one is ignored. */
                if (out->has_instance == 0) {
                    out->instance = (uint16_t)value;
                    out->has_instance = 1;
                }
            } else if (logical == 3) {
                /* Points past the fourth are dropped. */
                if (out->point_count < 4u) {
                    out->points[out->point_count++] = (uint16_t)value;
                }
            } else if (logical == 4) {
                out->attribute = (uint16_t)value;
                out->has_attribute = 1;
            }
            continue;
        }
        if (seg == 0x80u) {
            uint8_t words;
            uint16_t bytes;
            if ((uint16_t)(offset + 2u) > len) {
                return EIP_ERR_PATH;
            }
            words = path[offset + 1];
            bytes = (uint16_t)words * 2u;
            if ((uint32_t)offset + 2u + bytes > len) {
                return EIP_ERR_PATH;
            }
            if (out->config_data == 0) {
                out->config_data = path + offset + 2;
                out->config_len = bytes;
            }
            offset = (uint16_t)(offset + 2u + bytes);
            continue;
        }
        return EIP_ERR_PATH;
    }
    return EIP_OK;
}

/* CIP request: service, path size in words, path, data. *data points into message. */
static int ParseCip(const uint8_t *message, uint16_t len, uint8_t *service, CipPath *path,
                    const uint8_t **data, uint16_t *data_len)
{
    uint16_t path_len;
    int rc;

    if (len < 2u) {
        return EIP_ERR_PROTOCOL;
    }
    *service = message[0];
    path_len = (uint16_t)message[1] * 2u;
    if ((uint32_t)path_len + 2u > len) {
        return EIP_ERR_PROTOCOL;
    }
    rc = WalkPath(message + 2, path_len, path);
    if (rc != EIP_OK) {
        return rc;
    }
    *data = message + 2 + path_len;
    *data_len = (uint16_t)(len - (uint16_t)(2u + path_len));
    return EIP_OK;
}

/* SHORT_STRING: length byte, then at most 32 characters, no pad. */
static int PutShortString(uint8_t *out, uint16_t cap, uint16_t *used, const char *text)
{
    uint8_t n = 0;
    uint16_t at = *used;

    while (text[n] != '\0' && n < 32u) {
        n++;
    }
    if ((uint32_t)at + 1u + n > cap) {
        return EIP_ERR_TOO_BIG;
    }
    out[at++] = n;
    memcpy(out + at, text, n);
    *used = (uint16_t)(at + n);
    return EIP_OK;
}

/* Identity instance attrs 1-8 in order: vendor, device type, product code, revision, status, serial,
 * name, state. EIP_ERR_PATH for any other attribute. */
static int IdentityValue(const EipAdapter *adapter, uint16_t attribute, uint8_t *out, uint16_t cap,
                         uint16_t *out_len)
{
    uint16_t used = 0;

    if (attribute == 1) {
        if (cap < 2u) {
            return EIP_ERR_TOO_BIG;
        }
        eip_store_le16(out, adapter->vendor_id);
        *out_len = 2;
        return EIP_OK;
    }
    if (attribute == 2) {
        if (cap < 2u) {
            return EIP_ERR_TOO_BIG;
        }
        eip_store_le16(out, adapter->device_type);
        *out_len = 2;
        return EIP_OK;
    }
    if (attribute == 3) {
        if (cap < 2u) {
            return EIP_ERR_TOO_BIG;
        }
        eip_store_le16(out, adapter->product_code);
        *out_len = 2;
        return EIP_OK;
    }
    if (attribute == 4) {
        if (cap < 2u) {
            return EIP_ERR_TOO_BIG;
        }
        out[0] = adapter->revision_major;
        out[1] = adapter->revision_minor;
        *out_len = 2;
        return EIP_OK;
    }
    if (attribute == 5) {
        if (cap < 2u) {
            return EIP_ERR_TOO_BIG;
        }
        eip_store_le16(out, adapter->identity_status);
        *out_len = 2;
        return EIP_OK;
    }
    if (attribute == 6) {
        if (cap < 4u) {
            return EIP_ERR_TOO_BIG;
        }
        eip_store_le32(out, adapter->serial_number);
        *out_len = 4;
        return EIP_OK;
    }
    if (attribute == 7) {
        if (PutShortString(out, cap, &used, adapter->product_name) != EIP_OK) {
            return EIP_ERR_TOO_BIG;
        }
        *out_len = used;
        return EIP_OK;
    }
    if (attribute == 8) {
        if (cap < 1u) {
            return EIP_ERR_TOO_BIG;
        }
        out[0] = adapter->state;
        *out_len = 1;
        return EIP_OK;
    }
    return EIP_ERR_PATH;
}

/* Get_Attribute_All: attrs 1-7 packed in order. State (attr 8) is not part of it. */
static int IdentityAll(const EipAdapter *adapter, uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    uint16_t used = 0;

    if (cap < 16u) {
        return EIP_ERR_TOO_BIG;
    }
    eip_store_le16(out + used, adapter->vendor_id);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, adapter->device_type);
    used = (uint16_t)(used + 2u);
    eip_store_le16(out + used, adapter->product_code);
    used = (uint16_t)(used + 2u);
    out[used++] = adapter->revision_major;
    out[used++] = adapter->revision_minor;
    eip_store_le16(out + used, adapter->identity_status);
    used = (uint16_t)(used + 2u);
    eip_store_le32(out + used, adapter->serial_number);
    used = (uint16_t)(used + 4u);
    if (PutShortString(out, cap, &used, adapter->product_name) != EIP_OK) {
        return EIP_ERR_TOO_BIG;
    }
    *out_len = used;
    return EIP_OK;
}

/* Class attrs 1-3, shared by every object: revision, highest instance, number of instances. */
static int ClassValue(uint16_t revision, uint16_t max_instance, uint16_t count, uint16_t attribute,
                      uint8_t *out, uint16_t cap, uint16_t *out_len)
{
    uint16_t value;

    if (cap < 2u) {
        return EIP_ERR_TOO_BIG;
    }
    if (attribute == 1) {
        value = revision;
    } else if (attribute == 2) {
        value = max_instance;
    } else if (attribute == 3) {
        value = count;
    } else {
        return EIP_ERR_PATH;
    }
    eip_store_le16(out, value);
    *out_len = 2;
    return EIP_OK;
}

/* Identity (class 1), read-only. Class level: revision 1, one instance. Instance 1: Get_Attribute_All
 * and Get_Attribute_Single. A Get with no attribute in the path answers 0x08, as does any other
 * service; Reset on instance 1 is taken by HandleCip before this runs. */
static int HandleIdentity(EipAdapter *adapter, uint8_t service, const CipPath *path, uint8_t *cip,
                          uint16_t cap, uint16_t *cip_len)
{
    uint8_t value[48];
    uint16_t value_len = 0;
    int rc;

    if (path->has_instance == 0 || path->instance == 0) {
        if (service == EIP_CIP_GET_ALL) {
            if (cap < 10u) {
                return EIP_ERR_TOO_BIG;
            }
            eip_store_le16(value, 1);
            eip_store_le16(value + 2, 1);
            eip_store_le16(value + 4, 1);
            return CipOk(cip, cap, cip_len, service, value, 6);
        }
        if (service != EIP_CIP_GET_ATTRIBUTE || path->has_attribute == 0) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_SERVICE, 0);
        }
        rc = ClassValue(1, 1, 1, path->attribute, value, sizeof value, &value_len);
        if (rc != EIP_OK) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_ATTR, 0);
        }
        return CipOk(cip, cap, cip_len, service, value, value_len);
    }
    if (path->instance != 1) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_NO_OBJECT, 0);
    }
    if (service == EIP_CIP_GET_ALL) {
        rc = IdentityAll(adapter, value, sizeof value, &value_len);
        if (rc != EIP_OK) {
            return rc;
        }
        return CipOk(cip, cap, cip_len, service, value, value_len);
    }
    if (service != EIP_CIP_GET_ATTRIBUTE || path->has_attribute == 0) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_SERVICE, 0);
    }
    rc = IdentityValue(adapter, path->attribute, value, sizeof value, &value_len);
    if (rc != EIP_OK) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_ATTR, 0);
    }
    return CipOk(cip, cap, cip_len, service, value, value_len);
}

/* Assembly (class 4). Class level: revision 2, attrs 1-3. Instance level: Get_Attribute_All and attr
 * 3 return the data, attr 4 the size, Set on attr 3 replaces the data and needs the exact size. Any
 * direction may be read or set, and an open connection makes no difference. An implicit_only instance
 * answers 0x16 as if absent. Data too big for cap returns EIP_ERR_TOO_BIG, not a CIP status. */
static int HandleAssembly(EipAdapter *adapter, uint8_t service, const CipPath *path, const uint8_t *data,
                          uint16_t data_len, uint8_t *cip, uint16_t cap, uint16_t *cip_len)
{
    EipAsm *slot;
    uint8_t value[16]; /* class attributes and attr 4 (size) only; data goes out from slot->data */
    uint16_t value_len = 0;
    int rc;

    if (path->has_instance == 0 || path->instance == 0) {
        if (service == EIP_CIP_GET_ALL) {
            eip_store_le16(value, 2);
            eip_store_le16(value + 2, HighestInstance(adapter));
            eip_store_le16(value + 4, adapter->asm_count);
            return CipOk(cip, cap, cip_len, service, value, 6);
        }
        if (service != EIP_CIP_GET_ATTRIBUTE || path->has_attribute == 0) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_SERVICE, 0);
        }
        rc = ClassValue(2, HighestInstance(adapter), adapter->asm_count, path->attribute, value,
                        sizeof value, &value_len);
        if (rc != EIP_OK) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_ATTR, 0);
        }
        return CipOk(cip, cap, cip_len, service, value, value_len);
    }
    slot = FindAsm(adapter, path->instance);
    if (slot == 0 || slot->implicit_only) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_NO_OBJECT, 0);
    }
    if (service == EIP_CIP_GET_ALL) {
        return CipOk(cip, cap, cip_len, service, slot->data, slot->size);
    }
    if (service == EIP_CIP_GET_ATTRIBUTE) {
        if (path->has_attribute == 0) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_PATH_UNKNOWN, 0);
        }
        if (path->attribute == 3) {
            return CipOk(cip, cap, cip_len, service, slot->data, slot->size);
        }
        if (path->attribute == 4) {
            eip_store_le16(value, slot->size);
            return CipOk(cip, cap, cip_len, service, value, 2);
        }
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_ATTR, 0);
    }
    if (service == EIP_CIP_SET_ATTRIBUTE) {
        if (path->attribute != 3) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_ATTR, 0);
        }
        if (data_len < slot->size) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_NOT_ENOUGH, 0);
        }
        if (data_len > slot->size) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_TOO_MUCH, 0);
        }
        memcpy(slot->data, data, slot->size);
        return CipOk(cip, cap, cip_len, service, 0, 0);
    }
    return CipStatus(cip, cap, cip_len, service, EIP_CIP_SERVICE, 0);
}

/* Network connection parameters:
 *
 *   Forward_Open, 16-bit        bits 0-8 size, 9 variable, 13-14 type, 15 redundant owner
 *   Large_Forward_Open, 32-bit  bits 0-15 size, 25 variable, 29-30 type, 31 redundant owner
 *
 * Type is 0 null, 1 multicast, 2 point-to-point, 3 reserved. Size is the whole connected payload in
 * bytes: the 2-byte sequence count, the run/idle header if any, then the assembly. The priority bits
 * in between are not used. */
static void DecodeParam(uint32_t raw, int large, uint16_t *size, uint8_t *variable, uint8_t *type,
                        uint8_t *redundant)
{
    if (large) {
        *size = (uint16_t)raw;
        *variable = (uint8_t)((raw >> 25) & 1u);
        *type = (uint8_t)((raw >> 29) & 3u);
        *redundant = (uint8_t)((raw >> 31) & 1u);
    } else {
        *size = (uint16_t)(raw & 0x1FFu);
        *variable = (uint8_t)((raw >> 9) & 1u);
        *type = (uint8_t)((raw >> 13) & 3u);
        *redundant = (uint8_t)((raw >> 15) & 1u);
    }
}

/* Electronic key check. EIP_OK, or EIP_ERR_CIP with *extended set. No key, or a key with vendor,
 * type, product and major all zero, passes. Otherwise a zero vendor, type, product or major matches
 * anything. key_major holds the major revision in 7 bits and the compatibility bit in bit 7: when
 * set, this device's minor may be equal or higher; when clear, it must be equal. The minor is
 * compared only when the key's major is non-zero, and a zero minor matches any minor. */
static int CheckKey(const EipAdapter *adapter, const CipPath *path, uint16_t *extended)
{
    uint8_t major;

    if (path->has_key == 0) {
        return EIP_OK;
    }
    if (path->key_vendor == 0 && path->key_type == 0 && path->key_product == 0 &&
        (path->key_major & 0x7Fu) == 0) {
        return EIP_OK;
    }
    if (path->key_vendor != 0 && path->key_vendor != adapter->vendor_id) {
        *extended = EIP_CM_VENDOR;
        return EIP_ERR_CIP;
    }
    if (path->key_type != 0 && path->key_type != adapter->device_type) {
        *extended = EIP_CM_TYPE;
        return EIP_ERR_CIP;
    }
    if (path->key_product != 0 && path->key_product != adapter->product_code) {
        *extended = EIP_CM_VENDOR; /* one status covers vendor and product code */
        return EIP_ERR_CIP;
    }
    major = (uint8_t)(path->key_major & 0x7Fu);
    if (major != 0 && major != adapter->revision_major) {
        *extended = EIP_CM_REVISION;
        return EIP_ERR_CIP;
    }
    if (major != 0) {
        if (path->key_major & 0x80u) {
            if (adapter->revision_minor < path->key_minor) {
                *extended = EIP_CM_REVISION;
                return EIP_ERR_CIP;
            }
        } else if (path->key_minor != 0 && adapter->revision_minor != path->key_minor) {
            *extended = EIP_CM_REVISION;
            return EIP_ERR_CIP;
        }
    }
    return EIP_OK;
}

/* Rounds up, and never returns less than one tick. */
static uint32_t UsToTicks(uint32_t microseconds, uint32_t ticks_per_second)
{
    uint32_t ticks = (uint32_t)(((uint64_t)microseconds * ticks_per_second + 999999ull) / 1000000ull);
    if (ticks < 1u) {
        ticks = 1u;
    }
    return ticks;
}

/* Forward_Open (0x54) and Large_Forward_Open (0x5B). data follows the CIP service and path:
 *
 *   Open  Large
 *   0     0      priority/tick and timeout ticks (not used)
 *   2     2      UDINT O->T connection id
 *   6     6      UDINT T->O connection id
 *   10    10     UINT connection serial     } with vendor and originator serial,
 *   12    12     UINT originator vendor     } the triad that names the connection
 *   14    14     UDINT originator serial    }
 *   18    18     USINT timeout multiplier, then 3 reserved bytes
 *   22    22     UDINT O->T RPI in microseconds
 *   26    26     O->T parameters (WORD / DWORD, see DecodeParam)
 *   28    30     UDINT T->O RPI in microseconds
 *   32    34     T->O parameters
 *   34    38     transport class and trigger
 *   35    39     connection path size in words
 *   36    40     connection path
 *
 * The adapter has one connection. Every check runs before anything changes, so a refused request
 * leaves a running connection and the config assembly alone. The same originator (vendor and serial,
 * whatever the connection serial) replaces its own connection. Anyone else is refused, even for a
 * multicast T->O already being produced.
 *
 * Refusals go out as a CIP status and return EIP_OK, and HandleCip adds the triad they must carry.
 * On success cip holds the reply and extra the sockaddr items for the CPF list. cpf is the request's CPF list, searched for a T->O
 * sockaddr item, and may be 0. */
static int AcceptOpen(EipAdapter *adapter, uint8_t service, const uint8_t *data, uint16_t data_len,
                      const uint8_t peer_ip[4], const uint8_t *cpf, uint16_t cpf_len, uint32_t now_tick,
                      uint8_t *cip, uint16_t cap, uint16_t *cip_len, uint8_t *extra, uint16_t extra_cap,
                      uint16_t *extra_len)
{
    int large = (service == EIP_CIP_LARGE_FORWARD_OPEN);
    uint16_t fixed = large ? 40u : 36u;
    uint16_t path_bytes;
    CipPath conn;
    uint32_t o2t_id;
    uint32_t t2o_request;
    uint16_t conn_serial;
    uint16_t vendor;
    uint32_t originator_serial;
    uint8_t multiplier;
    uint32_t o2t_rpi;
    uint32_t t2o_rpi;
    uint32_t o2t_raw;
    uint32_t t2o_raw;
    uint8_t transport;
    uint16_t o2t_size = 0;
    uint16_t t2o_size = 0;
    uint8_t o2t_var = 0;
    uint8_t t2o_var = 0;
    uint8_t o2t_type = 0;
    uint8_t t2o_type = 0;
    uint8_t o2t_owner = 0;
    uint8_t t2o_owner = 0;
    uint8_t o2t_header = 0;
    uint8_t t2o_header = 0;
    uint16_t o2t_point = 0;
    uint16_t t2o_point = 0;
    EipAsm *consumed = 0;
    EipAsm *produced = 0;
    EipAsm *config = 0;
    uint16_t extended = EIP_CM_BAD_PATH;
    uint8_t success[26];
    uint32_t t2o_id;
    uint32_t api_us;
    const uint8_t *sock = 0;
    uint16_t sock_len = 0;
    uint8_t dest_ip[4];
    uint16_t dest_port = EIP_UDP_PORT;
    int rc;

    if (data_len < fixed) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_NOT_ENOUGH, 0);
    }
    path_bytes = (uint16_t)data[fixed - 1u] * 2u;
    if ((uint32_t)fixed + path_bytes > data_len) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_NOT_ENOUGH, 0);
    }
    if (WalkPath(data + fixed, path_bytes, &conn) != EIP_OK) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_BAD_PATH);
    }
    o2t_id = eip_load_le32(data + 2);
    t2o_request = eip_load_le32(data + 6);
    conn_serial = eip_load_le16(data + 10);
    vendor = eip_load_le16(data + 12);
    originator_serial = eip_load_le32(data + 14);
    multiplier = data[18];
    o2t_rpi = eip_load_le32(data + 22);
    if (large) {
        o2t_raw = eip_load_le32(data + 26);
        t2o_rpi = eip_load_le32(data + 30);
        t2o_raw = eip_load_le32(data + 34);
        transport = data[38];
    } else {
        o2t_raw = eip_load_le16(data + 26);
        t2o_rpi = eip_load_le32(data + 28);
        t2o_raw = eip_load_le16(data + 32);
        transport = data[34];
    }
    DecodeParam(o2t_raw, large, &o2t_size, &o2t_var, &o2t_type, &o2t_owner);
    DecodeParam(t2o_raw, large, &t2o_size, &t2o_var, &t2o_type, &t2o_owner);
    (void)t2o_request;

    if (multiplier > 7u || o2t_rpi == 0 || t2o_rpi == 0) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_BAD_PARAM);
    }
    /* Transport byte: bits 0-3 class, bits 4-6 production trigger, bit 7 direction. Only Class 1
     * client connections pass. Triggers 0 (cyclic) and 1 (change of state) are both accepted, and
     * both produce on the RPI schedule. */
    if ((transport & 0x0Fu) != 1u) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_CLASS_UNSUPPORTED);
    }
    if (((transport >> 4) & 0x07u) > 1u) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_TRIGGER);
    }
    if (transport & 0x80u) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_DIRECTION);
    }
    if (o2t_var) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_OT_FIXED);
    }
    if (t2o_var) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_TO_FIXED);
    }
    if (o2t_owner || t2o_owner) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_REDUNDANT);
    }
    if (o2t_type == 3u) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_OT_TYPE);
    }
    if (t2o_type == 3u) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_TO_TYPE);
    }
    if (o2t_type == 1u) { /* multicast is T->O only */
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_OT_TYPE);
    }
    if (CheckKey(adapter, &conn, &extended) != EIP_OK) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, extended);
    }
    /* The path need not name a class, but one that does must name Assembly. */
    if (conn.has_class && conn.class_id != 4u) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_BAD_PATH);
    }

    /* Connection points come as O->T then T->O. With one direction null, O->T takes the first point
     * and T->O the last, so a path that keeps both points still resolves. */
    if (o2t_type != 0 && t2o_type != 0) {
        if (conn.point_count < 2u) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_BAD_PATH);
        }
        o2t_point = conn.points[0];
        t2o_point = conn.points[1];
    } else if (o2t_type != 0) {
        if (conn.point_count < 1u) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_CONSUME_PATH);
        }
        o2t_point = conn.points[0];
    } else if (t2o_type != 0) {
        if (conn.point_count < 1u) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_PRODUCE_PATH);
        }
        t2o_point = conn.points[conn.point_count - 1u];
    }

    /* An instance segment names the config assembly. Config data is optional, but must fit exactly:
     * a size-0 assembly accepts a config point with no data. */
    if (conn.has_instance && conn.instance != 0) {
        config = FindAsm(adapter, conn.instance);
        if (config == 0 || config->direction != EIP_ASM_CONFIG) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_CONFIG_PATH);
        }
        if (conn.config_data != 0) {
            if (conn.config_len != config->size) {
                return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_CONFIG_SIZE);
            }
        }
    }
    /* The declared size tells whether the direction carries a run/idle header: 2 + 4 + assembly means
     * yes, 2 + assembly means no. A null direction must declare size 0. */
    if (o2t_type != 0) {
        consumed = FindAsm(adapter, o2t_point);
        if (consumed == 0 || consumed->direction != EIP_ASM_CONSUMED) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_CONSUME_PATH);
        }
        if (o2t_size == (uint16_t)(2u + 4u + consumed->size)) {
            o2t_header = 1;
        } else if (o2t_size != (uint16_t)(2u + consumed->size)) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_OT_SIZE);
        }
    } else if (o2t_size != 0) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_OT_SIZE);
    }
    if (t2o_type != 0) {
        produced = FindAsm(adapter, t2o_point);
        if (produced == 0 || produced->direction != EIP_ASM_PRODUCED) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_PRODUCE_PATH);
        }
        if (t2o_size == (uint16_t)(2u + 4u + produced->size)) {
            t2o_header = 1;
        } else if (t2o_size != (uint16_t)(2u + produced->size)) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_TO_SIZE);
        }
    } else if (t2o_size != 0) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_TO_SIZE);
    }
    if (o2t_type == 0 && t2o_type == 0) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION, EIP_CM_BAD_PARAM);
    }

    /* Past this point the request is accepted. */
    if (adapter->io_open) {
        int same = (adapter->originator_vendor == vendor && adapter->originator_serial == originator_serial);
        if (!same) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_CONNECTION,
                             (consumed != 0) ? EIP_CM_OWNERSHIP : EIP_CM_OUT_OF_CONNECTIONS);
        }
        CloseIo(adapter);
    }

    if (config != 0 && conn.config_data != 0) {
        memcpy(config->data, conn.config_data, config->size);
    }

    /* A non-zero O->T id from the originator is kept; 0 asks for one. Both directions draw on
     * next_t2o_id, and 0 is never handed out. */
    if (o2t_id == 0) {
        o2t_id = adapter->next_t2o_id++;
        if (adapter->next_t2o_id == 0) {
            adapter->next_t2o_id = 1;
        }
    }
    /* The consumer picks a point-to-point connection id, so a P2P T->O keeps the originator's
     * value. A multicast T->O id is picked by the producer (this adapter). */
    if (t2o_type == 2u && t2o_request != 0 && t2o_request != o2t_id) {
        t2o_id = t2o_request;
    } else {
        t2o_id = adapter->next_t2o_id++;
        if (t2o_id == 0 || t2o_id == o2t_id) {
            t2o_id = adapter->next_t2o_id++;
            if (t2o_id == 0) {
                t2o_id = 1;
            }
        }
    }

    /* T->O destination: the device's multicast group, or the originator's T->O sockaddr item, or the
     * request's source on UDP 2222. An item with port 0 has already overwritten dest_ip and dest_port
     * by the time the condition fails, so the last branch sets both again. */
    memcpy(dest_ip, peer_ip, 4);
    if (t2o_type == 1u) {
        eip_adapter_multicast_address(adapter->ip, adapter->netmask, dest_ip);
        dest_port = EIP_UDP_PORT;
    } else if (cpf != 0 && eip_cpf_find(cpf, cpf_len, EIP_ITEM_SOCK_T2O, &sock, &sock_len) == EIP_OK &&
               eip_parse_sockaddr(sock, sock_len, dest_ip, &dest_port) == EIP_OK && dest_port != 0) {
        /* Originator socket. 0.0.0.0 means "the address this request came from". */
        if (dest_ip[0] == 0 && dest_ip[1] == 0 && dest_ip[2] == 0 && dest_ip[3] == 0) {
            memcpy(dest_ip, peer_ip, 4);
        }
    } else {
        memcpy(dest_ip, peer_ip, 4);
        dest_port = EIP_UDP_PORT;
    }

    adapter->io_open = 1;
    adapter->o2t_header = o2t_header;
    adapter->t2o_header = t2o_header;
    adapter->o2t_null = (o2t_type == 0) ? 1u : 0u;
    adapter->t2o_null = (t2o_type == 0) ? 1u : 0u;
    adapter->o2t_instance = (consumed != 0) ? consumed->instance : 0;
    adapter->t2o_instance = (produced != 0) ? produced->instance : 0;
    adapter->o2t_bytes = (consumed != 0) ? consumed->size : 0;
    adapter->t2o_bytes = (produced != 0) ? produced->size : 0;
    adapter->o2t_id = o2t_id;
    adapter->t2o_id = t2o_id;
    adapter->conn_serial = conn_serial;
    adapter->originator_vendor = vendor;
    adapter->originator_serial = originator_serial;
    memcpy(adapter->originator_ip, dest_ip, 4);  /* T->O destination: the group when multicast */
    adapter->originator_port = dest_port;
    adapter->t2o_multicast = (t2o_type == 1u) ? 1u : 0u;
    memcpy(adapter->o2t_source_ip, peer_ip, 4);  /* O->T must come from the originator of this Forward_Open */
    adapter->o2t_seq_valid = 0;
    adapter->o2t_last_seq32 = 0;
    adapter->o2t_seq16_valid = 0;
    ClearConsumed(adapter);                      /* no stale data from an earlier connection */
    adapter->interval_ticks = UsToTicks(t2o_rpi, adapter->ticks_per_second);
    {
        /* Inactivity timeout watches consumed (O->T) data: O->T RPI x 4 << multiplier.
         * In ticks it is capped at 0x00FFFFFF, then raised to the floor if below it. */
        uint64_t timeout_us = (uint64_t)o2t_rpi << ((uint32_t)multiplier + 2u);
        uint32_t timeout = (timeout_us > 0xFFFFFFFFull) ? 0xFFFFFFFFu
                                                         : UsToTicks((uint32_t)timeout_us,
                                                                     adapter->ticks_per_second);
        uint32_t floor_ticks = (uint32_t)(((uint64_t)adapter->timeout_floor_ms *
                                           adapter->ticks_per_second + 999u) / 1000u);
        if (timeout > 0x00FFFFFFu) {
            timeout = 0x00FFFFFFu;
        }
        if (timeout < floor_ticks) {
            timeout = floor_ticks;
        }
        adapter->timeout_ticks = timeout;
    }
    adapter->last_rx_tick = now_tick;
    adapter->next_tx_tick = now_tick;            /* the first T->O packet is due at once */
    adapter->seq32 = 0;
    adapter->seq16 = 0;
    adapter->run = o2t_header ? 0u : 1u;         /* a run/idle header starts out idle */
    RefreshStatus(adapter);

    /* The T->O API is the interval really used: the RPI rounded up to whole ticks, never below it. */
    api_us = (uint32_t)(((uint64_t)adapter->interval_ticks * 1000000ull) / adapter->ticks_per_second);
    if (api_us < t2o_rpi) {
        api_us = t2o_rpi;
    }
    /* Reply: O->T id, T->O id, the triad, O->T API, T->O API, application reply size 0, reserved.
     * The O->T API is the RPI as requested: it is the rate the originator sends at, and the rate
     * the inactivity watchdog was computed from. */
    eip_store_le32(success, o2t_id);
    eip_store_le32(success + 4, t2o_id);
    eip_store_le16(success + 8, conn_serial);
    eip_store_le16(success + 10, vendor);
    eip_store_le32(success + 12, originator_serial);
    eip_store_le32(success + 16, o2t_rpi);
    eip_store_le32(success + 20, api_us);
    success[24] = 0;
    success[25] = 0;
    rc = CipOk(cip, cap, cip_len, service, success, 26);
    if (rc != EIP_OK) {
        CloseIo(adapter);
        return rc;
    }
    /* Sockaddr items are best effort: the open stands even if extra is too small for them. */
    rc = eip_build_sockaddr_item(extra, extra_cap, extra_len, EIP_ITEM_SOCK_O2T, adapter->ip, EIP_UDP_PORT);
    if (rc != EIP_OK) {
        *extra_len = 0;
    } else if (adapter->t2o_multicast) {
        /* The originator must learn the group to join from a T->O sockaddr item. */
        uint16_t one = 0;
        rc = eip_build_sockaddr_item(extra + *extra_len, (uint16_t)(extra_cap - *extra_len), &one,
                                     EIP_ITEM_SOCK_T2O, dest_ip, EIP_UDP_PORT);
        if (rc == EIP_OK) {
            *extra_len = (uint16_t)(*extra_len + one);
        }
    }
    adapter->opens++;
    return EIP_OK;
}

/* Forward_Close. data: priority/tick, timeout ticks, then the triad at offset 2. The connection path
 * after the triad is not read: the triad alone decides, whichever session or address sent it. The
 * reply is the triad, application reply size 0 and a reserved byte. */
static int HandleForwardClose(EipAdapter *adapter, const uint8_t *data, uint16_t data_len, uint8_t *cip,
                              uint16_t cap, uint16_t *cip_len)
{
    uint16_t serial;
    uint16_t vendor;
    uint32_t originator;
    uint8_t reply[10];

    if (data_len < 10u) {
        return CipStatus(cip, cap, cip_len, EIP_CIP_FORWARD_CLOSE, EIP_CIP_NOT_ENOUGH, 0);
    }
    serial = eip_load_le16(data + 2);
    vendor = eip_load_le16(data + 4);
    originator = eip_load_le32(data + 6);
    if (adapter->io_open == 0 || adapter->conn_serial != serial || adapter->originator_vendor != vendor ||
        adapter->originator_serial != originator) {
        return CipStatus(cip, cap, cip_len, EIP_CIP_FORWARD_CLOSE, EIP_CIP_CONNECTION, EIP_CM_NOT_FOUND);
    }
    eip_store_le16(reply, serial);
    eip_store_le16(reply + 2, vendor);
    eip_store_le32(reply + 4, originator);
    reply[8] = 0;
    reply[9] = 0;
    CloseIo(adapter);
    return CipOk(cip, cap, cip_len, EIP_CIP_FORWARD_CLOSE, reply, 10);
}

/* CIP UDINT holding an IPv4 address: 192.168.1.91 = 0xC0A8015B, sent little-endian. */
static uint16_t PutIp(uint8_t *out, const uint8_t ip[4])
{
    out[0] = ip[3];
    out[1] = ip[2];
    out[2] = ip[1];
    out[3] = ip[0];
    return 4;
}

/* CIP STRING: UINT length, characters, pad byte to an even length. */
static uint16_t PutCipString(uint8_t *out, const char *text)
{
    uint16_t n = 0;

    while (text[n] != '\0' && n < 64u) {
        n++;
    }
    eip_store_le16(out, n);
    memcpy(out + 2, text, n);
    if (n & 1u) {
        out[2 + n] = 0;
        return (uint16_t)(3u + n);
    }
    return (uint16_t)(2u + n);
}

/* Layout copied from a real FEN20-4DIP-4DXP (fw 3.3.4.0): TCP/IP class rev 3, one instance, physical link =
 * Ethernet Link instance 3 ("internal"); Ethernet Link class rev 3 with ETH1, ETH2 (external switch ports) and
 * "internal". This board has one port, so ETH2 reports link down. */
enum { NET_CLASS_REV = 3, ETH_LINK_INSTANCES = 3 };

/* TCP/IP Interface instance attribute into out. Returns its length, or 0 for an attribute that is not
 * implemented (7, and anything past 12). out must hold the largest value; the caller's does. */
static uint16_t TcpIpAttr(const EipAdapter *adapter, uint16_t attribute, uint8_t *out)
{
    uint16_t n = 0;

    switch (attribute) {
    case 1: /* status: valid configuration obtained */
        eip_store_le32(out, 0x00000001u);
        return 4;
    case 2: /* bit 4 = configuration settable (attrs 3/5/6 over CIP) */
        eip_store_le32(out, adapter->ip_config_capability | (adapter->set_interface ? 0x10u : 0u));
        return 4;
    case 3:
        eip_store_le32(out, adapter->ip_config_control);
        return 4;
    case 4: /* physical link object: 2 words, class 0xF6 instance 3 (internal port) */
        eip_store_le16(out, 2);
        out[2] = 0x20;
        out[3] = 0xF6;
        out[4] = 0x24;
        out[5] = ETH_LINK_INSTANCES;
        return 6;
    case 5: /* interface configuration: IP, netmask, gateway, two name servers, domain */
        n = (uint16_t)(n + PutIp(out + n, adapter->ip));
        n = (uint16_t)(n + PutIp(out + n, adapter->netmask));
        n = (uint16_t)(n + PutIp(out + n, adapter->gateway));
        n = (uint16_t)(n + PutIp(out + n, adapter->dns1));
        n = (uint16_t)(n + PutIp(out + n, adapter->dns2));
        n = (uint16_t)(n + PutCipString(out + n, adapter->domain));
        return n;
    case 6:
        return PutCipString(out, adapter->host_name);
    case 8: /* TTL value (the real FEN20 does not implement 8/9) */
        out[0] = 1;
        return 1;
    case 9: { /* multicast config: default algorithm, 1 address */
        uint8_t group[4];
        eip_adapter_multicast_address(adapter->ip, adapter->netmask, group);
        out[0] = 0;
        out[1] = 0;
        eip_store_le16(out + 2, 1);
        PutIp(out + 4, group);
        return 8;
    }
    case 10: /* SelectAcd: ACD not implemented */
        out[0] = 0;
        return 1;
    case 11: /* LastConflictDetected: activity, remote MAC, ARP PDU (28) */
        memset(out, 0, 35);
        return 35;
    case 12: /* EtherNet/IP QuickConnect */
        out[0] = 0;
        return 1;
    default:
        return 0;
    }
}

/* Ethernet Link instance attribute into out. Returns its length, or 0 for an attribute that is not
 * implemented (the counters among them). instance must be 1..ETH_LINK_INSTANCES: it indexes labels.
 * All three instances show the one MAC, and instances 1 and 3 show the one real link. */
static uint16_t EthLinkAttr(const EipAdapter *adapter, uint16_t instance, uint16_t attribute, uint8_t *out)
{
    static const char *const labels[ETH_LINK_INSTANCES] = {"ETH1", "ETH2", "internal"};
    int up = (instance != 2u);

    switch (attribute) {
    case 1:
        eip_store_le32(out, up ? adapter->link_speed_mbps : 0u);
        return 4;
    case 2:
        eip_store_le32(out, up ? adapter->link_flags : 0u);
        return 4;
    case 3:
        memcpy(out, adapter->mac, 6);
        return 6;
    case 6: /* interface control: auto-negotiate, speed 0 */
        eip_store_le32(out, 0x00000001u);
        return 4;
    case 7: /* interface type: 1 internal, 2 twisted pair */
        out[0] = (instance == 3u) ? 1u : 2u;
        return 1;
    case 10: { /* interface label: SHORT_STRING */
        const char *t = labels[instance - 1u];
        uint8_t n = 0;
        while (t[n] != '\0') {
            n++;
        }
        out[0] = n;
        memcpy(out + 1, t, n);
        return (uint16_t)(1u + n);
    }
    default:
        return 0;
    }
}

static uint32_t IpValue(const uint8_t ip[4])
{
    return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) | ((uint32_t)ip[2] << 8) | ip[3];
}

/* The inverse of PutIp. */
static void GetIp(const uint8_t *in, uint8_t ip[4])
{
    ip[0] = in[3];
    ip[1] = in[2];
    ip[2] = in[1];
    ip[3] = in[0];
}

/* Usable unicast host address with a contiguous mask; gateway 0 or inside the subnet. */
static int ValidAddress(const uint8_t ip_b[4], const uint8_t mask_b[4], const uint8_t gw_b[4])
{
    uint32_t ip = IpValue(ip_b);
    uint32_t mask = IpValue(mask_b);
    uint32_t gw = IpValue(gw_b);
    uint32_t inv = ~mask;

    if (mask == 0 || ((inv + 1u) & inv) != 0) {
        return 0; /* zero or not contiguous */
    }
    if ((ip >> 24) == 0u || (ip >> 24) == 127u || (ip >> 24) >= 224u) {
        return 0; /* 0.x, loopback, multicast, class E */
    }
    /* A /31 or /32 has no network or broadcast address to avoid. */
    if (inv > 1u && ((ip & inv) == 0 || (ip & inv) == inv)) {
        return 0; /* network or broadcast address */
    }
    if (gw != 0 && ((gw & mask) != (ip & mask) || gw == ip)) {
        return 0;
    }
    return 1;
}

/* Letters, digits, '-' and '.'. Used for both the host name and the domain name. */
static int ValidHostChars(const uint8_t *p, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '.')) {
            return 0;
        }
    }
    return 1;
}

static void CurrentInterface(const EipAdapter *adapter, EipInterfaceConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->control = adapter->ip_config_control;
    memcpy(cfg->ip, adapter->ip, 4);
    memcpy(cfg->netmask, adapter->netmask, 4);
    memcpy(cfg->gateway, adapter->gateway, 4);
    memcpy(cfg->dns1, adapter->dns1, 4);
    memcpy(cfg->dns2, adapter->dns2, 4);
    memcpy(cfg->domain, adapter->domain, sizeof cfg->domain);
    memcpy(cfg->host_name, adapter->host_name, sizeof cfg->host_name);
}

/* Set_Attribute_Single on TCP/IP Interface attrs 3, 5, 6. Returns a CIP general status.
 * The new value is checked and merged into a copy of the whole configuration, then offered to the
 * set_interface callback. The adapter's fields change only if the callback returns 0, and the network
 * stack is the callback's business. */
static uint8_t SetTcpIp(EipAdapter *adapter, uint16_t attribute, const uint8_t *data, uint16_t len)
{
    EipInterfaceConfig cfg;
    uint8_t status;

    if (adapter->set_interface == 0) {
        return EIP_CIP_NOT_SETTABLE;
    }
    CurrentInterface(adapter, &cfg);
    if (attribute == 3) {
        uint32_t v;
        if (len < 4u) {
            return EIP_CIP_NOT_ENOUGH;
        }
        if (len > 4u) {
            return EIP_CIP_TOO_MUCH;
        }
        v = eip_load_le32(data);
        /* Bits 0-3 pick the method. Bit 4 (DNS enable) is allowed through but not stored. */
        if ((v & ~0x1Fu) != 0 || ((v & 0x0Fu) != 0 && (v & 0x0Fu) != 2u)) {
            return EIP_CIP_INVALID_VALUE; /* static (0) or DHCP (2) only; no BOOTP */
        }
        cfg.control = (uint8_t)(v & 0x0Fu);
    } else if (attribute == 5) {
        uint16_t dlen;
        uint16_t need;
        if (adapter->ip_config_control != 0) {
            return EIP_CIP_STATE_CONFLICT; /* only while configuration control = static */
        }
        /* Five UDINT addresses, then a STRING: UINT length at offset 20, text at 22. An odd-length
         * string may arrive with or without its pad byte. */
        if (len < 22u) {
            return EIP_CIP_NOT_ENOUGH;
        }
        dlen = eip_load_le16(data + 20);
        if (dlen > 48u) {
            return EIP_CIP_INVALID_VALUE;
        }
        need = (uint16_t)(22u + dlen);
        if (len < need) {
            return EIP_CIP_NOT_ENOUGH;
        }
        if (len > (uint16_t)(need + (dlen & 1u))) {
            return EIP_CIP_TOO_MUCH;
        }
        GetIp(data, cfg.ip);
        GetIp(data + 4, cfg.netmask);
        GetIp(data + 8, cfg.gateway);
        GetIp(data + 12, cfg.dns1);
        GetIp(data + 16, cfg.dns2);
        if (!ValidAddress(cfg.ip, cfg.netmask, cfg.gateway) || !ValidHostChars(data + 22, dlen)) {
            return EIP_CIP_INVALID_VALUE;
        }
        memcpy(cfg.domain, data + 22, dlen);
        cfg.domain[dlen] = '\0';
    } else if (attribute == 6) {
        uint16_t hlen;
        uint16_t need;
        if (len < 2u) {
            return EIP_CIP_NOT_ENOUGH;
        }
        hlen = eip_load_le16(data);
        if (hlen > 64u) {
            return EIP_CIP_INVALID_VALUE;
        }
        need = (uint16_t)(2u + hlen);
        if (len < need) {
            return EIP_CIP_NOT_ENOUGH;
        }
        if (len > (uint16_t)(need + (hlen & 1u))) {
            return EIP_CIP_TOO_MUCH;
        }
        if (!ValidHostChars(data + 2, hlen)) {
            return EIP_CIP_INVALID_VALUE;
        }
        memcpy(cfg.host_name, data + 2, hlen);
        cfg.host_name[hlen] = '\0';
    } else {
        return EIP_CIP_NOT_SETTABLE;
    }
    status = adapter->set_interface(adapter->callback_ctx, attribute, &cfg);
    if (status == 0) {
        adapter->ip_config_control = cfg.control;
        if (attribute == 5) {
            memcpy(adapter->ip, cfg.ip, 4);
            memcpy(adapter->netmask, cfg.netmask, 4);
            memcpy(adapter->gateway, cfg.gateway, 4);
            eip_adapter_set_dns(adapter, cfg.dns1, cfg.dns2, cfg.domain);
        }
        if (attribute == 6) {
            memcpy(adapter->host_name, cfg.host_name, sizeof adapter->host_name);
        }
    }
    return status;
}

/* Get_Attribute_Single / Get_Attribute_All for TCP/IP Interface (0xF5) and Ethernet Link (0xF6), and
 * Set_Attribute_Single at instance level. Only 0xF5 attrs 3, 5 and 6 can actually be set. */
static int HandleNetObject(EipAdapter *adapter, uint16_t class_id, uint8_t service, const CipPath *path,
                           const uint8_t *data, uint16_t data_len, uint8_t *cip, uint16_t cap,
                           uint16_t *cip_len)
{
    /* What Get_Attribute_All returns: a subset of the attributes Get_Attribute_Single serves. */
    static const uint8_t tcp_all[] = {1, 2, 3, 4, 5, 6};
    static const uint8_t link_all[] = {1, 2, 3};
    uint8_t value[160]; /* the largest use is Get_Attribute_All on 0xF5: at most 154 bytes */
    uint16_t used = 0;
    uint16_t n;
    int tcp = (class_id == 0xF5u);
    uint16_t instances = tcp ? 1u : (uint16_t)ETH_LINK_INSTANCES;

    if (path->has_instance == 0 || path->instance == 0) {
        uint16_t max_attr = tcp ? 12u : 10u; /* highest instance attribute */
        if (service == EIP_CIP_GET_ALL) { /* revision, highest instance, instance count, max_attr */
            eip_store_le16(value, NET_CLASS_REV);
            eip_store_le16(value + 2, instances);
            eip_store_le16(value + 4, instances);
            eip_store_le16(value + 6, max_attr);
            return CipOk(cip, cap, cip_len, service, value, 8);
        }
        if (service != EIP_CIP_GET_ATTRIBUTE || path->has_attribute == 0) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_SERVICE, 0);
        }
        if (path->attribute == 6 || path->attribute == 7) { /* both answer max_attr */
            eip_store_le16(value, max_attr);
            return CipOk(cip, cap, cip_len, service, value, 2);
        }
        if (ClassValue(NET_CLASS_REV, instances, instances, path->attribute, value, sizeof value, &n) != EIP_OK) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_ATTR, 0);
        }
        return CipOk(cip, cap, cip_len, service, value, n);
    }
    if (path->instance > instances) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_NO_OBJECT, 0);
    }
    if (service == EIP_CIP_GET_ALL) {
        const uint8_t *list = tcp ? tcp_all : link_all;
        uint8_t count = tcp ? (uint8_t)sizeof tcp_all : (uint8_t)sizeof link_all;
        uint8_t i;
        for (i = 0; i < count; i++) {
            used = (uint16_t)(used + (tcp ? TcpIpAttr(adapter, list[i], value + used)
                                          : EthLinkAttr(adapter, path->instance, list[i], value + used)));
        }
        return CipOk(cip, cap, cip_len, service, value, used);
    }
    if (service == EIP_CIP_SET_ATTRIBUTE) {
        uint8_t st;
        if (path->has_attribute == 0) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_PATH_UNKNOWN, 0);
        }
        /* The attribute getters double as a probe: no such attribute answers 0x14, while one that
         * exists but cannot be set answers 0x0E. */
        if (!tcp) {
            st = EthLinkAttr(adapter, path->instance, path->attribute, value) ? EIP_CIP_NOT_SETTABLE : EIP_CIP_ATTR;
            return CipStatus(cip, cap, cip_len, service, st, 0);
        }
        if (TcpIpAttr(adapter, path->attribute, value) == 0) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_ATTR, 0);
        }
        st = SetTcpIp(adapter, path->attribute, data, data_len);
        if (st != 0) {
            return CipStatus(cip, cap, cip_len, service, st, 0);
        }
        return CipOk(cip, cap, cip_len, service, 0, 0);
    }
    if (service != EIP_CIP_GET_ATTRIBUTE || path->has_attribute == 0) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_SERVICE, 0);
    }
    n = tcp ? TcpIpAttr(adapter, path->attribute, value)
            : EthLinkAttr(adapter, path->instance, path->attribute, value);
    if (n == 0) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_ATTR, 0);
    }
    return CipOk(cip, cap, cip_len, service, value, n);
}

static int HandleCip(EipAdapter *adapter, const uint8_t *message, uint16_t message_len,
                     const uint8_t peer_ip[4], const uint8_t *cpf, uint16_t cpf_len, uint32_t now_tick,
                     uint8_t *cip, uint16_t cap, uint16_t *cip_len, uint8_t *extra, uint16_t extra_cap,
                     uint16_t *extra_len);

/* Multiple Service Packet (0x0A) to the Message Router: several requests in one message, one reply.
 *
 *   request data  UINT count, UINT offset[count], the requests   (offsets from the count)
 *   reply data    UINT count, UINT offset[count], the replies    (offsets from the count)
 *
 * Each embedded request goes through HandleCip, so it gets exactly the answer it would get on its own
 * (Sets and Resets still through the application's callbacks). Connection services and a nested packet
 * are refused per entry (0x08). General status 0x1E when any entry failed, 0x13 for a malformed count or
 * offset table, 0x11 when the replies can't fit; an entry whose own reply doesn't fit is answered 0x11.
 * Entries run in order and later failures undo nothing. */
enum { kMultipleEntryMin = 6 }; /* room kept for each entry's reply: a status with extended status */

static int HandleMultiple(EipAdapter *adapter, const uint8_t *data, uint16_t data_len, const uint8_t peer_ip[4],
                          uint32_t now_tick, uint8_t *cip, uint16_t cap, uint16_t *cip_len)
{
    uint16_t count;
    uint32_t head;
    uint32_t used;
    uint16_t i;
    int failed = 0;

    if (data_len < 2u) {
        return CipStatus(cip, cap, cip_len, EIP_CIP_MULTIPLE, EIP_CIP_NOT_ENOUGH, 0);
    }
    count = eip_load_le16(data);
    head = 2u + 2u * (uint32_t)count;
    if (count == 0 || head > data_len) {
        return CipStatus(cip, cap, cip_len, EIP_CIP_MULTIPLE, EIP_CIP_NOT_ENOUGH, 0);
    }
    /* Check the whole table first, so a malformed packet runs nothing. An entry ends where the next
     * one starts, which makes the offsets ascending; the last ends with the data. */
    for (i = 0; i < count; i++) {
        uint16_t start = eip_load_le16(data + 2u + 2u * i);
        uint16_t end = (uint16_t)((i + 1u < count) ? eip_load_le16(data + 4u + 2u * i) : data_len);
        if (start < head || end > data_len || end < start) {
            return CipStatus(cip, cap, cip_len, EIP_CIP_MULTIPLE, EIP_CIP_NOT_ENOUGH, 0);
        }
    }
    if (4u + head + (uint32_t)kMultipleEntryMin * count > cap) {
        return CipStatus(cip, cap, cip_len, EIP_CIP_MULTIPLE, EIP_CIP_REPLY_TOO_LARGE, 0);
    }

    /* Reply layout: CIP header at 0, count at 4, offset table at 6, replies from 4 + head. */
    used = 4u + head;
    for (i = 0; i < count; i++) {
        uint16_t start = eip_load_le16(data + 2u + 2u * i);
        uint16_t end = (uint16_t)((i + 1u < count) ? eip_load_le16(data + 4u + 2u * i) : data_len);
        const uint8_t *sub = data + start;
        uint16_t sub_len = (uint16_t)(end - start);
        /* What this entry may use: what is left, less the minimum kept for the entries after it. */
        uint16_t room = (uint16_t)(cap - used - (uint32_t)kMultipleEntryMin * (uint32_t)(count - 1u - i));
        uint16_t n = 0;
        uint8_t svc = (sub_len >= 1u) ? sub[0] : 0u;
        int rc;

        eip_store_le16(cip + 6u + 2u * i, (uint16_t)(used - 4u));
        if (sub_len < 2u) {
            rc = CipStatus(cip + used, room, &n, svc, EIP_CIP_NOT_ENOUGH, 0);
        } else if (svc == EIP_CIP_FORWARD_OPEN || svc == EIP_CIP_LARGE_FORWARD_OPEN ||
                   svc == EIP_CIP_FORWARD_CLOSE || svc == EIP_CIP_MULTIPLE) {
            rc = CipStatus(cip + used, room, &n, svc, EIP_CIP_SERVICE, 0);
        } else {
            uint8_t extra[48]; /* required by HandleCip; stays empty with Forward_Open refused above */
            uint16_t extra_len = 0;
            rc = HandleCip(adapter, sub, sub_len, peer_ip, 0, 0, now_tick, cip + used, room, &n, extra,
                           sizeof extra, &extra_len);
            if (rc == EIP_ERR_TOO_BIG) {
                rc = CipStatus(cip + used, room, &n, svc, EIP_CIP_REPLY_TOO_LARGE, 0);
            } else if (rc != EIP_OK) {
                rc = CipStatus(cip + used, room, &n, svc, EIP_CIP_SERVICE, 0);
            }
        }
        if (rc != EIP_OK) {
            return rc; /* cannot happen: room >= kMultipleEntryMin */
        }
        if (cip[used + 2u] != 0) {
            failed = 1;
        }
        used += n;
    }
    cip[0] = (uint8_t)(EIP_CIP_MULTIPLE | 0x80u);
    cip[1] = 0;
    cip[2] = failed ? (uint8_t)EIP_CIP_EMBEDDED_ERROR : 0u;
    cip[3] = 0;
    eip_store_le16(cip + 4u, count);
    *cip_len = (uint16_t)used;
    return EIP_OK;
}

/* Route one CIP request. The reply, good or bad, lands in cip and the return is EIP_OK; only a reply
 * too big for cap returns an error. A class the adapter does not have answers 0x16. extra receives
 * the sockaddr items of a Forward_Open reply. cpf is the request's CPF list, or 0 inside a Multiple
 * Service Packet. */
static int HandleCip(EipAdapter *adapter, const uint8_t *message, uint16_t message_len,
                     const uint8_t peer_ip[4], const uint8_t *cpf, uint16_t cpf_len, uint32_t now_tick,
                     uint8_t *cip, uint16_t cap, uint16_t *cip_len, uint8_t *extra, uint16_t extra_cap,
                     uint16_t *extra_len)
{
    int rc;
    uint16_t triad_at;
    uint8_t service = 0;
    CipPath path;
    const uint8_t *data = 0;
    uint16_t data_len = 0;

    *extra_len = 0;
    if (ParseCip(message, message_len, &service, &path, &data, &data_len) != EIP_OK) {
        return CipStatus(cip, cap, cip_len, (message_len != 0) ? message[0] : 0u, EIP_CIP_PATH_UNKNOWN, 0);
    }
    if (service == EIP_CIP_FORWARD_OPEN || service == EIP_CIP_LARGE_FORWARD_OPEN ||
        service == EIP_CIP_FORWARD_CLOSE) {
        /* Connection Manager, instance 1 only. */
        if (path.has_class == 0 || path.class_id != 6u || path.instance != 1u) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_PATH_UNKNOWN, 0);
        }
        if (service == EIP_CIP_FORWARD_CLOSE) {
            triad_at = 2;
            rc = HandleForwardClose(adapter, data, data_len, cip, cap, cip_len);
        } else {
            triad_at = 10;
            rc = AcceptOpen(adapter, service, data, data_len, peer_ip, cpf, cpf_len, now_tick, cip, cap,
                            cip_len, extra, extra_cap, extra_len);
        }
        /* A refusal repeats the triad, then remaining path size 0 and a reserved byte, so the
         * originator can match it to its request. A request too short to hold a triad has none. */
        if (rc == EIP_OK && cip[2] != 0 && data_len >= (uint16_t)(triad_at + 8u)) {
            if ((uint32_t)*cip_len + 10u > cap) {
                return EIP_ERR_TOO_BIG;
            }
            memcpy(cip + *cip_len, data + triad_at, 8);
            cip[*cip_len + 8u] = 0;
            cip[*cip_len + 9u] = 0;
            *cip_len = (uint16_t)(*cip_len + 10u);
        }
        return rc;
    }
    if (path.has_class == 0) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_PATH_UNKNOWN, 0);
    }
    if (path.class_id == 1u && service == EIP_CIP_RESET && path.has_instance && path.instance == 1u) {
        uint8_t type = (data_len >= 1u) ? data[0] : 0u; /* optional: no data means type 0 */
        uint8_t st;
        if (adapter->reset == 0) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_SERVICE, 0);
        }
        if (data_len > 1u) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_TOO_MUCH, 0);
        }
        if (type > 1u) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_INVALID_PARAM, 0);
        }
        st = adapter->reset(adapter->callback_ctx, type);
        if (st != 0) {
            return CipStatus(cip, cap, cip_len, service, st, 0);
        }
        return CipOk(cip, cap, cip_len, service, 0, 0);
    }
    if (path.class_id == 1u) {
        return HandleIdentity(adapter, service, &path, cip, cap, cip_len);
    }
    if (path.class_id == 4u) {
        return HandleAssembly(adapter, service, &path, data, data_len, cip, cap, cip_len);
    }
    if (path.class_id == 0xF5u || path.class_id == 0xF6u) {
        return HandleNetObject(adapter, path.class_id, service, &path, data, data_len, cip, cap, cip_len);
    }
    if (path.class_id == 2u && service == EIP_CIP_MULTIPLE) {
        if (path.has_instance && path.instance != 1u) {
            return CipStatus(cip, cap, cip_len, service, EIP_CIP_NO_OBJECT, 0);
        }
        return HandleMultiple(adapter, data, data_len, peer_ip, now_tick, cip, cap, cip_len);
    }
    /* Message Router and Connection Manager exist, but have no other services. */
    if (path.class_id == 2u || path.class_id == 6u) {
        return CipStatus(cip, cap, cip_len, service, EIP_CIP_SERVICE, 0);
    }
    return CipStatus(cip, cap, cip_len, service, EIP_CIP_NO_OBJECT, 0);
}

/* *conn_session is the session of this transport connection, kept by the caller and 0 before
 * RegisterSession. eip_server.c keeps one per TCP client and passes a fresh 0 for each UDP datagram,
 * so SendRRData over UDP is refused while the List commands work. Session handles are not tied to
 * the Class 1 connection: ending a session leaves I/O running. peer_ip may be 0; a Forward_Open then
 * has no source address to pin O->T to. */
int eip_adapter_on_encap(EipAdapter *adapter, uint32_t *conn_session, const uint8_t peer_ip[4],
                         const uint8_t *request, uint16_t request_len, uint32_t now_tick,
                         uint8_t *response, uint16_t response_cap, uint16_t *response_len)
{
    EipEncapView view;
    uint8_t peer[4];
    uint8_t cip[512]; /* caps an explicit reply at 508 bytes of data */
    uint16_t cip_len = 0;
    uint8_t extra[48];
    uint16_t extra_len = 0;
    int rc;

    if (adapter == 0 || conn_session == 0 || request == 0 || response == 0 || response_len == 0) {
        return EIP_ERR_ARG;
    }
    *response_len = 0;
    if (eip_parse_encap(request, request_len, &view) != EIP_OK) {
        return EIP_ERR_PROTOCOL;
    }
    memset(peer, 0, sizeof peer);
    if (peer_ip != 0) {
        memcpy(peer, peer_ip, 4);
    }

    /* These two have no reply. UnregisterSession forgets the session and nothing else: closing the
     * socket is not done here, whatever handle the request carried. */
    if (view.command == EIP_CMD_NOP) {
        return EIP_OK;
    }
    if (view.command == EIP_CMD_UNREGISTER) {
        *conn_session = 0;
        return EIP_OK;
    }
    if (view.command == EIP_CMD_LIST_SERVICES) {
        rc = BuildListServices(s_body, sizeof s_body, &cip_len);
        if (rc != EIP_OK) {
            return rc;
        }
        return EncapReply(&view, view.session, 0, s_body, cip_len, response, response_cap, response_len);
    }
    if (view.command == EIP_CMD_LIST_IDENTITY) {
        rc = BuildListIdentity(adapter, s_body, sizeof s_body, &cip_len);
        if (rc != EIP_OK) {
            return rc;
        }
        return EncapReply(&view, view.session, 0, s_body, cip_len, response, response_cap, response_len);
    }
    if (view.command == EIP_CMD_LIST_INTERFACES) {
        eip_store_le16(s_body, 0); /* item count: no interfaces listed */
        return EncapReply(&view, view.session, 0, s_body, 2, response, response_cap, response_len);
    }
    if (view.command == EIP_CMD_REGISTER) {
        uint32_t handle;
        /* Body: UINT protocol version, which must be 1, and UINT options, echoed unchecked. A second
         * RegisterSession on a connection replaces its session. Handles are never 0. */
        if (view.length < 4u || eip_load_le16(view.body) != 1u) {
            uint32_t status = (view.length < 4u) ? EIP_ENCAP_BAD_LENGTH : EIP_ENCAP_BAD_VERSION;
            return EncapReply(&view, 0, status, 0, 0, response, response_cap, response_len);
        }
        handle = adapter->next_session++;
        if (adapter->next_session == 0) {
            adapter->next_session = 1;
        }
        *conn_session = handle;
        s_body[0] = view.body[0];
        s_body[1] = view.body[1];
        s_body[2] = view.body[2];
        s_body[3] = view.body[3];
        return EncapReply(&view, handle, 0, s_body, 4, response, response_cap, response_len);
    }
    /* SendRRData is the only command left, and the only one that needs a session. SendUnitData is
     * not supported: explicit messaging is unconnected here. */
    if (view.command != EIP_CMD_SEND_RR) {
        return EncapReply(&view, view.session, EIP_ENCAP_BAD_COMMAND, 0, 0, response, response_cap,
                          response_len);
    }
    if (*conn_session == 0 || view.session != *conn_session) {
        return EncapReply(&view, view.session, EIP_ENCAP_BAD_SESSION, 0, 0, response, response_cap,
                          response_len);
    }
    /* SendRRData body: UDINT interface handle, UINT timeout, then the CPF list. */
    if (view.length < 8u) {
        return EncapReply(&view, view.session, EIP_ENCAP_BAD_DATA, 0, 0, response, response_cap,
                          response_len);
    }
    {
        const uint8_t *cpf = view.body + 6;
        uint16_t cpf_len = (uint16_t)(view.length - 6u);
        const uint8_t *unconnected = 0;
        uint16_t unconnected_len = 0;

        if (eip_cpf_find(cpf, cpf_len, EIP_ITEM_UNCONNECTED, &unconnected, &unconnected_len) != EIP_OK) {
            return EncapReply(&view, view.session, EIP_ENCAP_BAD_DATA, 0, 0, response, response_cap,
                              response_len);
        }
        rc = HandleCip(adapter, unconnected, unconnected_len, peer, cpf, cpf_len, now_tick, cip,
                       sizeof cip, &cip_len, extra, sizeof extra, &extra_len);
        if (rc == EIP_ERR_TOO_BIG) {
            /* The reply outgrew cip, for example an assembly over 508 bytes. Answer with a status:
             * a non-OK return here would make eip_server.c drop the client. Parsing succeeded to
             * get this far, so message holds a service byte. */
            extra_len = 0;
            rc = CipStatus(cip, sizeof cip, &cip_len, unconnected[0], EIP_CIP_REPLY_TOO_LARGE, 0);
        }
        if (rc != EIP_OK) {
            return rc;
        }
        /* Each sockaddr item is 20 bytes. */
        return ReplyCip(&view, cip, cip_len, extra, extra_len, (uint16_t)(extra_len / 20u), response,
                        response_cap, response_len);
    }
}

int eip_adapter_take_input(EipAdapter *adapter, const uint8_t *packet, uint16_t packet_len, uint32_t now_tick)
{
    return eip_adapter_take_input_from(adapter, packet, packet_len, now_tick, 0);
}

int eip_adapter_take_input_from(EipAdapter *adapter, const uint8_t *packet, uint16_t packet_len,
                                uint32_t now_tick, const uint8_t src_ip[4])
{
    static const uint8_t zero_ip[4] = {0, 0, 0, 0};
    uint32_t connection_id = 0;
    uint32_t seq32 = 0;
    int has_seq32 = 0;
    uint16_t sequence = 0;
    const uint8_t *payload = 0;
    uint16_t payload_len = 0;
    EipAsm *slot;
    const uint8_t *assembly;
    uint16_t assembly_len;
    uint8_t run;
    int same_data = 0;

    if (adapter == 0 || packet == 0) {
        return EIP_ERR_ARG;
    }
    if (adapter->io_open == 0 || adapter->o2t_null) {
        return EIP_ERR_STATE;
    }
    if (eip_parse_io_packet_seq(packet, packet_len, &connection_id, &seq32, &has_seq32, &sequence, &payload,
                                &payload_len) != EIP_OK || connection_id != adapter->o2t_id) {
        adapter->rx_dropped_bad++;
        return EIP_ERR_PROTOCOL;
    }
    /* A zero o2t_source_ip means the Forward_Open came with no peer address: nothing to compare. */
    if (src_ip != 0 && memcmp(adapter->o2t_source_ip, zero_ip, 4) != 0 &&
        memcmp(src_ip, adapter->o2t_source_ip, 4) != 0) {
        adapter->rx_dropped_source++;
        return EIP_ERR_PROTOCOL;
    }
    /* Duplicate or out-of-order: the 32-bit sequence number must advance (serial-number arithmetic). */
    if (has_seq32 && adapter->o2t_seq_valid && (int32_t)(seq32 - adapter->o2t_last_seq32) <= 0) {
        adapter->rx_dropped_seq++;
        return EIP_ERR_PROTOCOL;
    }
    /* Without a Sequenced Address item, fall back on the 16-bit CIP count. An unchanged count means
     * "same data again" (an originator may hold it while the data does not change): keep the connection
     * alive but leave the data alone. A count that went backwards is stale. */
    if (!has_seq32 && adapter->o2t_seq16_valid) {
        uint16_t advance = (uint16_t)(sequence - adapter->o2t_last_seq16);
        if (advance >= 0x8000u) {
            adapter->rx_dropped_seq++;
            return EIP_ERR_PROTOCOL;
        }
        same_data = (advance == 0u);
    }
    /* A connection with no run/idle header is always in run. The header is 4 bytes; only bit 0 of the
     * first is read. */
    assembly = payload;
    assembly_len = payload_len;
    run = 1;
    if (adapter->o2t_header) {
        if (payload_len < 4u) {
            adapter->rx_dropped_bad++;
            return EIP_ERR_PROTOCOL;
        }
        run = (payload[0] & 0x01u) ? 1u : 0u;
        assembly += 4;
        assembly_len = (uint16_t)(payload_len - 4u);
    }
    /* Short is dropped; bytes beyond the assembly size are ignored. */
    if (assembly_len < adapter->o2t_bytes) {
        adapter->rx_dropped_bad++;
        return EIP_ERR_PROTOCOL;
    }
    slot = FindAsm(adapter, adapter->o2t_instance);
    if (slot == 0) {
        return EIP_ERR_STATE;
    }
    /* Accepted: only now refresh the watchdog and the sequence tracking. */
    adapter->last_rx_tick = now_tick;
    if (has_seq32) {
        adapter->o2t_last_seq32 = seq32;
        adapter->o2t_seq_valid = 1;
    }
    adapter->o2t_last_seq16 = sequence;
    adapter->o2t_seq16_valid = 1;
    adapter->run = run;
    RefreshStatus(adapter);
    if (run && same_data) {
        /* repeated 16-bit count: watchdog refreshed, data kept */
    } else if (run) {
        memcpy(slot->data, assembly, adapter->o2t_bytes);
    } else {
        memset(slot->data, 0, adapter->o2t_bytes);   /* idle: data is not valid, outputs go to the idle state */
        adapter->rx_idle++;
    }
    adapter->rx_packets++;
    return EIP_OK;
}

int eip_adapter_io_due(const EipAdapter *adapter, uint32_t now_tick)
{
    if (adapter == 0 || adapter->io_open == 0 || adapter->t2o_null) {
        return 0;
    }
    /* Signed difference: holds across the 32-bit tick wrap. */
    return ((int32_t)(now_tick - adapter->next_tx_tick) >= 0) ? 1 : 0;
}

/* Build the next T->O packet and advance the schedule. ip and port receive where to send it: the
 * originator or the multicast group. Both sequence numbers advance on every packet, changed data or
 * not, and skip 0 when they wrap. A T->O run/idle header, if the connection has one, always says run. */
int eip_adapter_build_output(EipAdapter *adapter, uint32_t now_tick, uint8_t *packet, uint16_t cap,
                             uint16_t *packet_len, uint8_t ip[4], uint16_t *port)
{
    EipAsm *slot;
    int rc;

    if (adapter == 0 || packet == 0 || packet_len == 0 || ip == 0 || port == 0) {
        return EIP_ERR_ARG;
    }
    if (adapter->io_open == 0 || adapter->t2o_null) {
        return EIP_ERR_STATE;
    }
    slot = FindAsm(adapter, adapter->t2o_instance);
    if (slot == 0) {
        return EIP_ERR_STATE;
    }
    adapter->seq32++;
    if (adapter->seq32 == 0) {
        adapter->seq32 = 1;
    }
    adapter->seq16++;
    if (adapter->seq16 == 0) {
        adapter->seq16 = 1;
    }
    rc = eip_build_io_packet(packet, cap, packet_len, adapter->t2o_id, adapter->seq32, adapter->seq16,
                             adapter->t2o_header, 1, slot->data, slot->size);
    if (rc != EIP_OK) {
        return rc;
    }
    /* Anchor to the schedule so late wake-ups do not stretch the average RPI.
     * If we fell a whole interval behind, restart the schedule from now. */
    adapter->next_tx_tick += adapter->interval_ticks;
    if ((int32_t)(now_tick - adapter->next_tx_tick) >= 0) {
        adapter->next_tx_tick = now_tick + adapter->interval_ticks;
    }
    adapter->tx_packets++;
    memcpy(ip, adapter->originator_ip, 4);
    *port = adapter->originator_port;
    return EIP_OK;
}

/* Inactivity watchdog. Unsigned subtraction holds across the tick wrap. With a null O->T nothing feeds
 * it, so such a connection never times out: only Forward_Close or a reopen ends it. */
void eip_adapter_check_timeout(EipAdapter *adapter, uint32_t now_tick)
{
    if (adapter == 0 || adapter->io_open == 0 || adapter->o2t_null) {
        return;
    }
    if ((now_tick - adapter->last_rx_tick) >= adapter->timeout_ticks) {
        adapter->timeouts++;
        CloseIo(adapter);
    }
}
