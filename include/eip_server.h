/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Adapter server: the adapter core (eip_adapter.h) plus its sockets.
 *
 * Most applications use the built-in instance through eip_adapter_start()
 * in eip.h. Use this API to own the storage (an EipServer is roughly
 * EIP_MAX_PACKET x (EIP_ADAPTER_MAX_CLIENTS + 3) bytes, about 11 KB with the
 * defaults) or to drive the loop yourself:
 *
 *     static EipServer server;
 *     eip_server_init(&server, &config);
 *     for (;;) {
 *         eip_server_service(&server, 5);   // RTOS task: waits up to 5 ms
 *     }
 *
 *     // bare metal: call eip_server_poll(&server) from the main loop
 *
 * eip_server_init / poll / service / close run in one context (the adapter
 * task or the main loop). read / write / snapshot may be called from any
 * task; they take eip_hal_lock.
 */
#ifndef EIP_SERVER_H_
#define EIP_SERVER_H_

#include "eip.h"
#include "eip_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One explicit-messaging TCP connection. */
typedef struct EipServerClient {
    eip_sock_t sock;
    uint32_t session;             /* session registered on this connection; 0 before that */
    uint16_t got;                 /* bytes of a partial packet held in buf */
    uint8_t ip[4];
    uint32_t last_rx_ms;          /* for eviction when every slot is busy */
    uint8_t buf[EIP_MAX_PACKET];
} EipServerClient;

typedef struct EipServer {
    EipAdapter core;
    EipAdapterConfig config;      /* callbacks and options copied at init */
    eip_sock_t tcp;
    eip_sock_t udp_id;            /* UDP 44818 */
    eip_sock_t udp_io;            /* UDP 2222 */
    EipServerClient clients[EIP_ADAPTER_MAX_CLIENTS];
    uint8_t rsp[EIP_MAX_PACKET];
    uint8_t dgram[EIP_MAX_PACKET];
    uint8_t initialized;
    uint8_t serial_from_mac;      /* serial_number 0: take it from the MAC */
    uint32_t last_open_ms;        /* last attempt to open the sockets */
    uint32_t last_netif_ms;       /* last refresh of the interface values */
    uint32_t reply_tokens;        /* token bucket for UDP 44818 replies */
    uint32_t reply_last_ms;
    uint32_t udp_replies_dropped;
    EipInterfaceConfig pending_interface; /* waiting for apply_interface */
    uint8_t pending;
    uint32_t pending_due_ms;
    int16_t reset_type;           /* -1 none */
    uint32_t reset_due_ms;
} EipServer;

/* Set up the adapter and try to open its sockets. config may be 0 for the
 * test identity and assemblies. Sockets that fail to open (no link yet) are
 * retried once a second by poll, so EIP_OK does not mean "listening": see
 * EipAdapterView.listening. */
int eip_server_init(EipServer *server, const EipAdapterConfig *config);

/* One non-blocking pass: open sockets if needed, refresh the interface
 * values, take Class 1 input, answer UDP 44818, serve and accept TCP clients,
 * check timeouts, send due T->O packets, run deferred callbacks.
 * Returns ms until the next T->O packet is due, capped at
 * EIP_ADAPTER_IDLE_WAIT_MS. */
uint32_t eip_server_poll(EipServer *server);

/* Wait for socket activity up to min(max_wait_ms, next T->O deadline) via
 * eip_hal_wait, then poll. The body of an adapter task. */
void eip_server_service(EipServer *server, uint32_t max_wait_ms);

/* Close every socket. The server may be initialized again afterwards. Does
 * nothing on a server that is not initialized. */
void eip_server_close(EipServer *server);

/* The same as eip_adapter_snapshot / read / write in eip.h, for this instance. */
int eip_server_snapshot(EipServer *server, EipAdapterView *view);
int eip_server_read(EipServer *server, uint16_t instance, uint8_t *data, uint16_t *data_len);
int eip_server_write(EipServer *server, uint16_t instance, const uint8_t *data, uint16_t data_len);

/* The instance behind eip_adapter_start() / eip_adapter_poll(). */
EipServer *eip_adapter_default_server(void);

#ifdef __cplusplus
}
#endif

#endif /* EIP_SERVER_H_ */
