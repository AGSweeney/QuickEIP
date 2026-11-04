/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP: a small EtherNet/IP stack for embedded targets.
 *
 * Scanner (originator) side:
 *   - encapsulation sessions on TCP 44818
 *   - CIP explicit messages (UCMM), optional routing to a ControlLogix slot
 *   - Logix symbolic tag read/write, with fragmentation
 *   - symbol-object tag list (Micro800 and Logix)
 *   - Class 1 UDP I/O as an originator
 *   - UDP ListIdentity, unicast or broadcast scan
 *
 * Adapter (target) side:
 *   - Identity, Assembly, TCP/IP Interface and Ethernet Link objects
 *   - ListIdentity / ListServices on TCP and UDP 44818
 *   - Forward_Open / Large_Forward_Open / Forward_Close, Class 1 I/O on UDP 2222
 *   - point-to-point or multicast T->O
 *
 * The protocol code is portable C99 with no OS calls. Each platform supplies
 * a small port (see eip_hal.h and docs/PORTING.md). Ports ship for NetBurner
 * NNDK, ESP-IDF, POSIX, and bare-metal lwIP (Teknic ClearCore).
 *
 * Values on the wire are little-endian, except the big-endian port and
 * address inside a sockaddr item. All timeouts are milliseconds.
 *
 * Threads: one task may use a given EipSession / EipIo at a time. The
 * adapter's read/write/snapshot calls may come from any task.
 *
 * slot -1 sends CIP straight to the device (CompactLogix, Micro800).
 * slot 0 or higher wraps the request in Unconnected Send and routes across
 * ControlLogix backplane port 1 to that slot. A zeroed EipOpenConfig has
 * slot 0. Micro800 is the Ethernet device itself: open it with slot -1.
 */
#ifndef EIP_H_
#define EIP_H_

#include <stdint.h>

#include "eip_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EIP_VERSION_MAJOR 2
#define EIP_VERSION_MINOR 0
#define EIP_VERSION_PATCH 0

/* Return codes. Calls that return int use these unless stated otherwise. */
enum {
    EIP_OK = 0,
    EIP_ERR_ARG = -1,      /* bad argument */
    EIP_ERR_CONNECT = -2,  /* TCP connect failed */
    EIP_ERR_IO = -3,       /* socket failed or the peer closed */
    EIP_ERR_TIMEOUT = -4,
    EIP_ERR_PROTOCOL = -5, /* malformed or unexpected reply, or a nonzero encapsulation status */
    EIP_ERR_CIP = -6,      /* the device answered with an error: see eip_last_cip_status */
    EIP_ERR_TOO_BIG = -7,  /* does not fit the caller's buffer or a protocol limit */
    EIP_ERR_PATH = -8,     /* bad tag or path text, or no such assembly instance */
    EIP_ERR_STATE = -9     /* wrong state: session not open, adapter not started or started twice */
};

enum {
    EIP_TCP_PORT = 44818,
    EIP_UDP_PORT = 2222,
    EIP_SCAN_PORT = 44819 /* local UDP port for the broadcast scan */
};

enum {
    EIP_KIND_ASSEMBLY = 0,      /* generic EtherNet/IP device, assembly object */
    EIP_KIND_COMPACTLOGIX = 1,  /* Rockwell 1768 / 1769 / 5069 or a CompactLogix name */
    EIP_KIND_MICRO800 = 2
};

/* CIP elementary data type codes, as used by eip_tag_read / eip_tag_write. */
enum {
    EIP_BOOL = 0x00C1,
    EIP_SINT = 0x00C2,
    EIP_INT = 0x00C3,
    EIP_DINT = 0x00C4,
    EIP_LINT = 0x00C5,
    EIP_USINT = 0x00C6,
    EIP_UINT = 0x00C7,
    EIP_UDINT = 0x00C8,
    EIP_ULINT = 0x00C9,
    EIP_REAL = 0x00CA,
    EIP_LREAL = 0x00CB
};

/* A socket handle owned by the port. Negative is "no socket". */
typedef int eip_sock_t;
#define EIP_SOCK_INVALID (-1)

/* ------------------------------------------------------------------------ */
/* Scanner                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct EipOpenConfig {
    const char *ip;         /* dotted IPv4, "192.168.1.10" */
    uint16_t tcp_port;      /* 0 selects 44818 */
    int16_t slot;           /* -1 direct, otherwise backplane slot */
    uint16_t vendor_id;     /* your ODVA vendor id, used by Class 1 open */
    uint32_t serial_number; /* 0 is replaced with 1 */
    uint32_t timeout_ms;    /* 0 selects 5000 */
} EipOpenConfig;

/* One encapsulation session. About 3 x EIP_MAX_PACKET bytes, so keep it
 * static rather than on a task stack. Treat the fields as private: use the
 * eip_session_* and eip_last_* calls. */
typedef struct EipSession {
    eip_sock_t sock;
    uint8_t sock_owned;     /* 1 while sock belongs to this session (a zeroed struct owns nothing) */
    uint32_t handle;        /* session handle from RegisterSession; 0 when not registered */
    uint8_t ip[4];
    uint16_t tcp_port;
    int16_t slot;
    uint16_t vendor_id;
    uint32_t serial_number;
    uint16_t next_conn_serial; /* connection serial for the next Forward_Open */
    uint32_t timeout_ms;
    uint32_t context_seq;   /* sender context of each request, checked against the reply */
    uint8_t cip_status;     /* general status of the last CIP reply */
    uint16_t cip_extended;  /* its extended status */
    uint32_t encap_status;  /* status field of the last encapsulation reply */
    uint8_t tx[EIP_MAX_PACKET];
    uint8_t rx[EIP_MAX_PACKET];
    uint8_t scratch[EIP_MAX_PACKET];
} EipSession;

/* A ListIdentity reply from one device. */
typedef struct EipIdentity {
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint8_t revision_major;
    uint8_t revision_minor;
    uint16_t status;        /* Identity object status word */
    uint32_t serial_number;
    char name[64];
    uint8_t state;          /* Identity object state */
} EipIdentity;

/* One replying device from a broadcast ListIdentity. */
typedef struct EipDevice {
    uint8_t ip[4];
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint8_t revision_major;
    uint8_t revision_minor;
    uint32_t serial_number;
    char name[64];
    uint8_t micro800; /* 1 when the name is a Rockwell Micro800 catalog */
} EipDevice;

/* One symbol from class 0x6B service 0x55 (Get Instance Attribute List). */
typedef struct EipSymbol {
    char name[96];
    uint16_t symbol_type;
    uint16_t element_size;
    uint32_t dim0;
    uint32_t dim1;
    uint32_t dim2;
    uint32_t element_count; /* product of the dimensions; a 0 dimension counts as 1 */
    uint8_t is_array;
} EipSymbol;

/* One Class 1 connection to open as an originator. Assembly sizes are
 * limited to 600 bytes each way. */
typedef struct EipIoConfig {
    uint16_t config_instance;  /* assembly object instance, from the EDS; 0 is sent as instance 0 */
    uint16_t output_instance;  /* O->T connection point (target consumes) */
    uint16_t input_instance;   /* T->O connection point (target produces) */
    uint16_t output_bytes;     /* assembly bytes we produce; 0 is a heartbeat */
    uint16_t input_bytes;      /* assembly bytes we consume */
    uint32_t o2t_rpi_us;       /* 0 selects 20000 (20 ms) */
    uint32_t t2o_rpi_us;
    uint16_t local_port;       /* 0 uses UDP 2223..2231; 2222 is the adapter */
    uint8_t run_idle;          /* 1 prefixes a 4-byte run/idle header on O->T */
    uint8_t timeout_multiplier; /* CIP multiplier 0..7 (x4, x8, x16, ...) */
} EipIoConfig;

/* An open Class 1 connection (originator side). Treat the fields as private. */
typedef struct EipIo {
    eip_sock_t sock;
    uint8_t sock_owned;
    uint32_t o2t_connection_id;
    uint32_t t2o_connection_id;
    uint16_t connection_serial; /* these three identify the connection to Forward_Close */
    uint16_t vendor_id;
    uint32_t originator_serial;
    uint32_t sequence_32;     /* Sequenced Address item count; skips 0 on wrap */
    uint16_t sequence_16;     /* CIP sequence count at the start of the payload; skips 0 on wrap */
    uint16_t output_bytes;
    uint16_t input_bytes;
    uint16_t remote_port;
    uint8_t remote_ip[4];
    uint8_t run_idle;
    uint8_t is_open;
    uint8_t path[64];         /* connection path, kept for Forward_Close */
    uint8_t path_len;
} EipIo;

/* Little-endian load and store helpers for wire data. */
static inline void eip_store_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline void eip_store_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static inline uint16_t eip_load_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t eip_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Short English text for an EIP_OK / EIP_ERR_* code. Never returns 0. */
const char *eip_status_text(int code);

/* Parse dotted-quad IPv4 text. EIP_OK or EIP_ERR_ARG. */
int eip_parse_ipv4(const char *text, uint8_t ip[4]);

/* Connect and register a session. Overwrites *session, so close an open one
 * first. config->slot must be -1..255. Blocks for up to timeout_ms on the
 * connect and again on the reply. When the device refuses the registration
 * the result is EIP_ERR_PROTOCOL and eip_last_encap_status holds the reason. */
int eip_session_open(EipSession *session, const EipOpenConfig *config);
/* Send UnregisterSession without waiting for an answer and close the socket.
 * Safe on a closed or zeroed session. Close any EipIo on it first, because
 * eip_io_close needs the session to send Forward_Close. */
void eip_session_close(EipSession *session);
/* Change the routing of later requests on an open session. A slot outside
 * -1..255 is ignored. */
void eip_session_set_slot(EipSession *session, int16_t slot);
/* 1 when the session is registered here. Safe on a zeroed struct. It does not
 * detect a lost peer: after EIP_ERR_IO, close and reopen the session.
 * A timeout with nothing received keeps the session, and a late reply is
 * discarded by the next request. A packet cut short or too large closes the
 * session, because the stream can't be resynchronized: the failing call
 * returns its error and later calls return EIP_ERR_STATE until it is reopened. */
int eip_session_is_open(const EipSession *session);

/* Detail behind EIP_ERR_CIP and EIP_ERR_PROTOCOL: the general and extended
 * status of the last CIP reply, and the status field of the last
 * encapsulation reply. A routed request can fail at the router as well as at
 * the target, for example 0x01 / 0x0204 when the slot does not answer. Each
 * returns 0 for a null session. */
uint8_t eip_last_cip_status(const EipSession *session);
uint16_t eip_last_cip_extended(const EipSession *session);
uint32_t eip_last_encap_status(const EipSession *session);

/* UDP ListIdentity sent to one address. Does not need an open session.
 * timeout_ms 0 waits 2 seconds. */
int eip_list_identity(const char *ip, EipIdentity *identity, uint32_t timeout_ms);

/* Broadcast ListIdentity on each interface (plus the global broadcast).
 * devices_len is the array capacity on the way in and the number of
 * distinct devices on the way out. timeout_ms 0 waits about 3 seconds,
 * resends once a second, and returns early after a quiet half second.
 * EIP_OK with a count of 0 means the scan ran and nobody answered.
 * A Micro800 match is Rockwell vendor 1 and a product name containing
 * "2080-", "2085-", or "micro8". */
int eip_scan_devices(EipDevice *devices, uint16_t *devices_len, uint32_t timeout_ms);

/* Classify a ListIdentity product name with Rockwell catalog heuristics.
 * Micro800 is tested before CompactLogix. */
int eip_device_kind(uint16_t vendor_id, const char *product_name);

/* Read the symbol object on an open session. symbols_len is capacity in
 * and the stored count out. Open a Micro800 with slot -1 so the request
 * is not wrapped for a backplane. On a later-page failure the count is
 * still the symbols stored so far. */
int eip_list_symbols(EipSession *session, EipSymbol *symbols, uint16_t *symbols_len);

/* Send one explicit message and wait for the reply. path is raw path bytes,
 * not including the path-size word. response_len is the capacity on the way
 * in and the copied size on the way out. response and response_len may both
 * be 0 to discard the reply data. cip_status and extended_status may be 0.
 * The encoded request is limited to 480 bytes. Returns EIP_ERR_CIP for a
 * nonzero general status, except 0x06 (partial transfer), which returns
 * EIP_OK with the status in *cip_status. Successful reply data too big for
 * response is EIP_ERR_TOO_BIG. */
int eip_cip(EipSession *session, uint8_t service, const uint8_t *path, uint16_t path_len,
            const uint8_t *request, uint16_t request_len, uint8_t *response,
            uint16_t *response_len, uint8_t *cip_status, uint16_t *extended_status);

/* Get Attribute Single. data_len is capacity in, byte count out. */
int eip_get_attribute(EipSession *session, uint16_t class_code, uint16_t instance,
                      uint16_t attribute, uint8_t *data, uint16_t *data_len,
                      uint8_t *cip_status);

/* Set Attribute Single. data is the attribute value, with no type prefix. */
int eip_set_attribute(EipSession *session, uint16_t class_code, uint16_t instance,
                      uint16_t attribute, const uint8_t *data, uint16_t data_len,
                      uint8_t *cip_status);

/* Tag names use '.' for members and [n] or [n,m] for elements.
 * data_len is capacity in, byte count out. cip_type receives the PLC type.
 * cip_type and cip_status may be 0. A reply too big for one packet continues
 * with Read Tag Fragmented until the device reports the end. */
int eip_tag_read(EipSession *session, const char *tag, uint16_t elements, uint8_t *data,
                 uint16_t *data_len, uint16_t *cip_type, uint8_t *cip_status);

/* data is little-endian element data, data_len bytes for `elements` elements
 * of cip_type. cip_status may be 0. Data over one request (at most 400 bytes,
 * less for a long tag path) goes out with Write Tag Fragmented. An error part
 * way through can leave the earlier fragments written. */
int eip_tag_write(EipSession *session, const char *tag, uint16_t cip_type, uint16_t elements,
                  const uint8_t *data, uint16_t data_len, uint8_t *cip_status);

/* Class 1 connection size sent to the target is 2 bytes of sequence count,
 * plus 4 bytes when run_idle is set, plus the assembly size.
 * The originator binds UDP 2223..2231 unless local_port is set.
 * Port 2222 stays with the adapter. */
int eip_io_open(EipSession *session, EipIo *io, const EipIoConfig *config);
/* Send one O->T packet. Call it once per RPI: the library does no pacing.
 * data_len must equal output_bytes. run sets the run/idle header when the
 * connection has one. */
int eip_io_produce(EipIo *io, const uint8_t *data, uint16_t data_len, int run);
/* Wait for the next T->O packet of this connection and copy input_bytes of
 * it. data_len is capacity in, byte count out. Packets for other connections
 * are skipped. timeout_ms 0 waits 1 second: there is no non-blocking form.
 * EIP_ERR_TIMEOUT when nothing arrives, which is how a lost connection shows:
 * eip_io_produce keeps succeeding, because it is UDP. */
int eip_io_consume(EipIo *io, uint8_t *data, uint16_t *data_len, uint32_t timeout_ms);
/* Send Forward_Close when the session is still open, then release the UDP
 * socket whatever the result. */
int eip_io_close(EipSession *session, EipIo *io);

/* ------------------------------------------------------------------------ */
/* Adapter                                                                  */
/* ------------------------------------------------------------------------ */

/* Assembly directions. Consumed is O->T (a scanner writes it). Produced is
 * T->O (this device sends it). Config arrives in the Forward_Open. */
enum {
    EIP_ASM_CONSUMED = 1,
    EIP_ASM_PRODUCED = 2,
    EIP_ASM_CONFIG = 3,
    EIP_ADAPTER_ASSEMBLIES = EIP_ADAPTER_MAX_ASSEMBLIES /* old name */
};

/* One assembly instance of the adapter. */
typedef struct EipAssemblyConfig {
    uint16_t instance;
    uint8_t direction;        /* EIP_ASM_CONSUMED / PRODUCED / CONFIG */
    uint16_t size;            /* bytes; 0 is allowed for a config point */
    const char *name;         /* up to 15 characters, may be 0 */
    const uint8_t *initial;   /* initial data, or 0 for zeros */
    uint8_t implicit_only;    /* 1: reachable only through Class 1, explicit Get/Set refused */
} EipAssemblyConfig;

/* TCP/IP Interface object (0xF5) settable values. */
typedef struct EipInterfaceConfig {
    uint8_t control;          /* attr 3: 0 static, 2 DHCP */
    uint8_t ip[4];            /* attr 5 */
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t dns1[4];
    uint8_t dns2[4];
    char domain[49];
    char host_name[65];       /* attr 6 */
} EipInterfaceConfig;

/* What a port knows about one network interface. Fields a port cannot fill
 * stay zero. */
typedef struct EipNetifInfo {
    uint8_t ip[4];
    uint8_t netmask[4];
    uint8_t gateway[4];
    uint8_t mac[6];
    uint8_t dns1[4];
    uint8_t dns2[4];
    uint8_t dhcp;             /* 1 when the address came from DHCP */
    uint8_t link_known;       /* 1 when the link fields below are valid */
    uint8_t link_up;
    uint8_t full_duplex;
    uint8_t autonegotiated;
    uint32_t speed_mbps;
    char host_name[65];
} EipNetifInfo;

typedef struct EipAdapterConfig {
    /* Identity object. vendor_id 0 keeps the built-in test identity
     * (vendor 65000, "EIP Test Adapter"). Use your own ODVA vendor id. */
    uint16_t vendor_id;
    uint16_t device_type;     /* 12 = communications adapter, 43 = generic device */
    uint16_t product_code;
    uint8_t revision_major;
    uint8_t revision_minor;
    const char *product_name; /* up to 31 characters */
    uint32_t serial_number;   /* 0 derives it from the interface MAC */

    /* Assemblies. assembly_count 0 keeps the test set:
     * 100 consumed (8 bytes), 101 produced (8), 102 produced (4), 150 config (4). */
    const EipAssemblyConfig *assemblies;
    uint8_t assembly_count;

    /* Minimum Class 1 inactivity timeout. 0 uses RPI x multiplier only,
     * though never less than two clock ticks of the port. */
    uint32_t timeout_floor_ms;

    /* Which port interface the adapter reports (0 is the first). */
    uint8_t netif_index;

    /* Optional. Link speed/duplex for the Ethernet Link object when the port
     * cannot read them. Return 1 while the link is up. */
    int (*link_info)(void *ctx, uint32_t *speed_mbps, int *full_duplex, int *autonegotiated);
    /* Optional. A scanner set TCP/IP Interface attr 3, 5 or 6. Called from
     * the adapter context about 150 ms after the reply went out; persist and
     * apply the new settings. 0 makes those attributes read-only. */
    void (*apply_interface)(void *ctx, const EipInterfaceConfig *config);
    /* Optional. Identity Reset service: type 0 restart, 1 factory defaults.
     * Called about 300 ms after the reply. 0 refuses the service. */
    void (*reset)(void *ctx, uint8_t type);
    void *callback_ctx;
    const char *domain;       /* initial domain name for TCP/IP attr 5, may be 0 */
    /* Optional. Asked before a TCP/IP Interface Set (what = EIP_CHANGE_INTERFACE)
     * or an Identity Reset (EIP_CHANGE_RESET) is accepted, so the application
     * can refuse it, for example while machinery is moving. Return 0 to allow,
     * or the CIP general status to answer with (0x10, device state conflict,
     * is the usual one); a refused request never reaches apply_interface or
     * reset. Runs in the adapter context with eip_hal_lock held: keep it short
     * and do not call eip_adapter_* from it. 0 allows every change. */
    uint8_t (*allow_change)(void *ctx, uint8_t what);
} EipAdapterConfig;

#define EIP_CHANGE_INTERFACE 1u
#define EIP_CHANGE_RESET     2u

/* An assembly as reported by eip_adapter_snapshot. */
typedef struct EipAssemblyView {
    uint16_t instance;
    uint8_t direction;
    uint16_t size;
    char name[16];
} EipAssemblyView;

/* A copy of the adapter's state, for status pages and diagnostics. */
typedef struct EipAdapterView {
    uint8_t running;          /* the adapter has been started */
    uint8_t listening;        /* its sockets are open */
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint8_t revision_major;
    uint8_t revision_minor;
    uint32_t serial_number;
    char name[32];
    uint8_t ip[4];
    uint8_t io_open;          /* a Class 1 connection is open */
    uint8_t run;              /* run/idle header says run (or the connection is modeless) */
    uint8_t multicast;        /* T->O goes to a multicast group */
    uint16_t o2t_instance;
    uint16_t t2o_instance;
    uint32_t rpi_us;          /* T->O interval of the open connection */
    uint32_t timeout_ms;      /* inactivity timeout of the open connection */
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t opens;           /* successful Forward_Opens */
    uint32_t timeouts;        /* connections closed by the inactivity watchdog */
    uint32_t rx_dropped_seq;  /* duplicate or older sequence number */
    uint32_t rx_dropped_source; /* O->T from an unexpected address */
    uint32_t rx_dropped_bad;  /* wrong connection id, short, malformed */
    uint32_t rx_idle;         /* accepted packets in idle (data set to zero) */
    uint32_t udp_replies_dropped; /* UDP 44818 requests not answered (rate limit) */
    uint8_t originator_ip[4]; /* where T->O goes: the originator, or the multicast group */
    uint8_t peer_ip[4];       /* the originator itself (the address its Forward_Open came from) */
    uint8_t tcp_clients;      /* open explicit-messaging sessions */
    uint16_t assembly_count;
    EipAssemblyView assemblies[EIP_ADAPTER_MAX_ASSEMBLIES];
} EipAdapterView;

/* The built-in adapter instance. For more than one adapter, or to own the
 * storage yourself, use the EipServer API in eip_server.h.
 *
 * eip_adapter_start copies what it needs from config (config may be 0 for
 * the test identity) and listens on TCP/UDP 44818 and UDP 2222.
 * - RTOS ports (NetBurner, ESP-IDF, POSIX) start a task that runs it.
 * - Poll-mode ports (ClearCore / bare-metal lwIP) start nothing: call
 *   eip_adapter_poll() from the main loop, at least every few ms. */
int eip_adapter_start(const EipAdapterConfig *config);
/* One non-blocking pass. Returns ms until the adapter next needs a call. */
uint32_t eip_adapter_poll(void);
/* The calls below return EIP_ERR_STATE until eip_adapter_start has succeeded. */
int eip_adapter_snapshot(EipAdapterView *view);
/* Copy an assembly's data. data_len is capacity in, byte count out, and a
 * capacity under the assembly size is EIP_ERR_TOO_BIG. A consumed assembly
 * reads as zeros after a timeout or close and while the scanner is idle, and
 * as its initial data before the first connection. An unknown instance is
 * EIP_ERR_PATH. */
int eip_adapter_read(uint16_t instance, uint8_t *data, uint16_t *data_len);
/* Replace an assembly's data, usually a produced one; the next T->O packet
 * carries it. data_len must equal the assembly size. */
int eip_adapter_write(uint16_t instance, const uint8_t *data, uint16_t data_len);

#ifdef __cplusplus
}
#endif

#endif /* EIP_H_ */
