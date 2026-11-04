/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
#ifndef EIP_ADAPTER_H_
#define EIP_ADAPTER_H_

#include "eip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sans-I/O adapter core. It never touches a socket or a clock: the
 * caller hands it packets plus a tick count and sends whatever it returns.
 * eip_server.c is the standard caller; use this header directly only for an
 * event loop of your own. EipInterfaceConfig is defined in eip.h.
 *
 * The core keeps one shared reply buffer, so calls on every EipAdapter in
 * a program must be serialized (the EipServer layer holds eip_hal_lock). */

/* Called (with the adapter state already validated) when a client sets F5 attr 3, 5 or 6.
 * Return a CIP general status: 0 accepts. The adapter's own values are updated on success; the caller
 * applies them to the network stack (after the reply is sent) and persists them.
 * cfg is the whole configuration: current values with the new attribute merged in. */
typedef uint8_t (*EipSetInterfaceFn)(void *ctx, uint16_t attribute, const EipInterfaceConfig *cfg);
/* Identity Reset (service 0x05) type 0 = restart, 1 = factory defaults. Return a CIP general status. */
typedef uint8_t (*EipResetFn)(void *ctx, uint8_t type);

/* Protocol state for one adapter. One task may call the eip_adapter_*
 * functions on a given object at a time. */

typedef struct EipAsm {
    uint16_t instance;
    uint8_t direction;      /* EIP_ASM_CONSUMED / PRODUCED / CONFIG */
    uint16_t size;
    uint8_t *data;          /* points into EipAdapter.pool */
    char name[16];
    uint8_t implicit_only;  /* connection point only: explicit Get/Set answer 0x16 (real FEN20 assembly 110) */
} EipAsm;

typedef struct EipAdapter {
    EipAsm asms[EIP_ADAPTER_MAX_ASSEMBLIES];
    uint8_t asm_count;
    uint16_t pool_used;
    uint8_t pool[EIP_ADAPTER_DATA_POOL];
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint8_t revision_major;
    uint8_t revision_minor;
    uint16_t identity_status;
    uint32_t serial_number;
    char product_name[32];
    uint8_t ip[4];
    uint8_t state;              /* Identity attr 8; 3 = operational */
    uint32_t ticks_per_second;
    uint32_t next_session;      /* next RegisterSession handle; never 0 */
    /* The one Class 1 connection. The connection fields mean something only while io_open is 1. */
    uint8_t io_open;
    uint8_t run;                /* run/idle says run, or the connection has no O->T header */
    uint8_t o2t_header;         /* O->T carries the 4-byte run/idle header */
    uint8_t t2o_header;         /* T->O carries it too, if the Forward_Open's size asked for it */
    uint8_t o2t_null;           /* this direction is null: no packets, no assembly */
    uint8_t t2o_null;
    uint8_t listening;          /* set by EipServer: its sockets are open */
    uint16_t o2t_instance;
    uint16_t t2o_instance;
    uint16_t o2t_bytes;         /* assembly bytes, without sequence count or header */
    uint16_t t2o_bytes;
    uint32_t o2t_id;
    uint32_t t2o_id;
    uint16_t conn_serial;       /* with the next two, the triad Forward_Close must match */
    uint16_t originator_vendor;
    uint32_t originator_serial;
    uint8_t originator_ip[4];   /* T->O destination: the originator, or the multicast group */
    uint16_t originator_port;   /* T->O destination port */
    uint32_t interval_ticks;    /* T->O RPI, rounded up to ticks */
    uint32_t timeout_ticks;     /* inactivity timeout; see AcceptOpen */
    uint32_t last_rx_tick;      /* last accepted O->T packet */
    uint32_t next_tx_tick;      /* when the next T->O packet is due */
    uint32_t seq32;             /* T->O sequence numbers */
    uint16_t seq16;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t next_t2o_id;       /* next connection id the adapter picks, for either direction */
    /* Added for the ESP32 port. All have backward-compatible defaults. */
    uint8_t netmask[4];         /* for the CIP multicast address; 255.255.255.0 by default */
    uint8_t t2o_multicast;      /* the open connection produces to a multicast group */
    uint32_t timeout_floor_ms;  /* minimum inactivity timeout; 2000 in the bare core, and EipServer replaces it */
    uint32_t timeouts;          /* connections closed by the inactivity watchdog */
    uint32_t opens;             /* successful Forward_Opens */
    /* TCP/IP Interface (0xF5) and Ethernet Link (0xF6) object values. Zero until set. */
    uint8_t mac[6];
    uint8_t gateway[4];
    uint8_t ip_config_control;  /* F5 attr 3: 0 static, 1 BOOTP, 2 DHCP */
    uint32_t ip_config_capability; /* F5 attr 2 */
    char host_name[65];
    uint8_t dns1[4];
    uint8_t dns2[4];
    char domain[49];
    EipSetInterfaceFn set_interface;
    EipResetFn reset;
    void *callback_ctx;
    /* Class-1 input hardening (review 2026-10-31) */
    uint8_t o2t_source_ip[4];   /* originator that opened the connection: O->T must come from here */
    uint32_t o2t_last_seq32;    /* last accepted encapsulation sequence number */
    uint8_t o2t_seq_valid;
    uint16_t o2t_last_seq16;    /* 16-bit CIP count, used when a packet has no Sequenced Address item */
    uint8_t o2t_seq16_valid;
    uint32_t rx_dropped_seq;   /* duplicate or older sequence number */
    uint32_t rx_dropped_source; /* from an unexpected address */
    uint32_t rx_dropped_bad;    /* wrong connection id, short or malformed */
    uint32_t rx_idle;           /* accepted packets with the run/idle header in idle */
    uint32_t link_speed_mbps;   /* F6 attr 1 */
    uint32_t link_flags;        /* F6 attr 2: bit0 link, bit1 full duplex, bits 2-4 negotiation status */
} EipAdapter;

void eip_adapter_init(EipAdapter *adapter, const uint8_t ip[4], uint32_t serial_number);
void eip_adapter_set_ip(EipAdapter *adapter, const uint8_t ip[4]);
void eip_adapter_set_clock(EipAdapter *adapter, uint32_t ticks_per_second);

/* Identity object values (also used by ListIdentity). name is truncated to 31 chars. */
void eip_adapter_set_identity(EipAdapter *adapter, uint16_t vendor_id, uint16_t device_type,
                              uint16_t product_code, uint8_t revision_major, uint8_t revision_minor,
                              const char *product_name);
/* Replace the default test assemblies (100/101/102/150). Call before any connection opens. */
void eip_adapter_clear_assemblies(EipAdapter *adapter);
int eip_adapter_add_assembly(EipAdapter *adapter, uint16_t instance, uint8_t direction, uint16_t size,
                             const char *name, const uint8_t *initial);
/* Hide an assembly from explicit messaging; Forward_Open and class-1 I/O still use it. */
int eip_adapter_set_implicit_only(EipAdapter *adapter, uint16_t instance, int implicit_only);
void eip_adapter_set_netmask(EipAdapter *adapter, const uint8_t netmask[4]);
/* 0 disables the floor, so the timeout is exactly O->T RPI x 4 << multiplier (rounded up to ticks,
 * capped at 0x00FFFFFF ticks). */
void eip_adapter_set_timeout_floor_ms(EipAdapter *adapter, uint32_t floor_ms);
/* Values reported by the TCP/IP Interface object (0xF5). config_control: 0 static, 2 DHCP. */
void eip_adapter_set_interface(EipAdapter *adapter, const uint8_t mac[6], const uint8_t gateway[4],
                               uint8_t config_control, const char *host_name);
/* Enable Set_Attribute_Single on F5 attrs 3/5/6 (capability bit 4 "configuration settable") and/or the
 * Identity Reset service. Either callback may be 0. */
void eip_adapter_set_callbacks(EipAdapter *adapter, EipSetInterfaceFn set_interface, EipResetFn reset,
                               void *ctx);
/* DNS servers and domain reported in F5 attr 5. */
void eip_adapter_set_dns(EipAdapter *adapter, const uint8_t dns1[4], const uint8_t dns2[4], const char *domain);
/* Values reported by the Ethernet Link object (0xF6). */
void eip_adapter_set_link(EipAdapter *adapter, int link_up, uint32_t speed_mbps, int full_duplex,
                          int autonegotiated);
/* CIP Vol 2 3-5.3: 239.192.1.0 + ((host_id - 1) & 0x3FF) * 32. */
void eip_adapter_multicast_address(const uint8_t ip[4], const uint8_t netmask[4], uint8_t out[4]);

/* *response_len 0 means the encapsulation command has no reply.
 * EIP_ERR_PROTOCOL means the bytes are not one encapsulation packet. Any other error means no reply
 * could be built (EIP_ERR_TOO_BIG: it did not fit). request must hold exactly one packet. */
int eip_adapter_on_encap(EipAdapter *adapter, uint32_t *conn_session, const uint8_t peer_ip[4],
                         const uint8_t *request, uint16_t request_len, uint32_t now_tick,
                         uint8_t *response, uint16_t response_cap, uint16_t *response_len);

/* Class-1 O->T input. A packet is accepted only if the connection id matches, it comes from the originator
 * that opened the connection (src_ip; 0 skips that check), its 32-bit sequence number is newer than the last
 * accepted one, and its header/length are right; only then is the inactivity timer refreshed. In idle (run/idle
 * header bit 0 = 0) the consumed assembly goes to its idle state (all zero) instead of taking the packet's data.
 * On close or timeout the consumed assembly is cleared as well.
 * A packet with no Sequenced Address item is judged by its 16-bit count instead: an older count is
 * dropped, and an unchanged one refreshes the timer but keeps the old data. */
int eip_adapter_take_input_from(EipAdapter *adapter, const uint8_t *packet, uint16_t packet_len,
                                uint32_t now_tick, const uint8_t src_ip[4]);
/* Same without the source check (kept for existing callers). */
int eip_adapter_take_input(EipAdapter *adapter, const uint8_t *packet, uint16_t packet_len,
                           uint32_t now_tick);
int eip_adapter_io_due(const EipAdapter *adapter, uint32_t now_tick);
int eip_adapter_build_output(EipAdapter *adapter, uint32_t now_tick, uint8_t *packet, uint16_t cap,
                             uint16_t *packet_len, uint8_t ip[4], uint16_t *port);
void eip_adapter_check_timeout(EipAdapter *adapter, uint32_t now_tick);

int eip_adapter_copy(const EipAdapter *adapter, uint16_t instance, uint8_t *data, uint16_t *data_len);
int eip_adapter_store(EipAdapter *adapter, uint16_t instance, const uint8_t *data, uint16_t data_len);

#ifdef __cplusplus
}
#endif

#endif /* EIP_ADAPTER_H_ */
