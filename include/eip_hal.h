/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* QuickEIP port contract.
 *
 * A port implements every function below, once, for one platform. The
 * library links against them directly (no function table). Ports that ship
 * with QuickEIP:
 *
 *   ports/netburner    NetBurner NNDK 3.x (NBRTOS)
 *   ports/esp-idf      ESP-IDF 5.x (FreeRTOS + lwIP sockets)
 *   ports/posix        Linux, macOS, WSL (desktop tools and tests)
 *   ports/windows      Windows 7 and later (Winsock 2; desktop tools and tests)
 *   ports/clearcore    Teknic ClearCore (bare metal, lwIP raw API, polled)
 *
 * ports/bsd_sockets and ports/lwip_raw hold the socket half of the contract
 * and are shared by the platform ports above. See docs/PORTING.md.
 *
 * Rules every implementation follows:
 *   - Socket handles are small non-negative ints; EIP_SOCK_INVALID is -1.
 *   - IPv4 addresses are 4 bytes in network order: {192,168,1,10}.
 *   - Ports are host-order integers.
 *   - timeout_ms 0 means "do not wait": return what is ready now.
 *   - A blocking wait must keep the network stack running. A bare-metal
 *     port calls its stack poll (eip_hal_yield) inside every wait loop.
 *   - Return codes are EIP_OK / EIP_ERR_* from eip.h unless stated.
 */
#ifndef EIP_HAL_H_
#define EIP_HAL_H_

#include "eip.h"

#ifdef __cplusplus
extern "C" {
#endif

struct EipServer;

/* ---- time and scheduling ------------------------------------------------ */

/* Free-running millisecond counter. Wraps at 2^32; callers use differences. */
uint32_t eip_hal_millis(void);

/* Granularity of eip_hal_millis, in ms (1 on most ports, 50 on a 20 Hz
 * NetBurner tick). The adapter never sets a Class 1 timeout under two of
 * these. */
uint32_t eip_hal_clock_resolution_ms(void);

/* Let other work run for a moment: sleep one tick on an RTOS, run the
 * network stack's poll on bare metal. Called inside every blocking wait. */
void eip_hal_yield(void);

/* Guard adapter state shared by the adapter context and application tasks.
 * Not nested. A no-op on single-threaded ports. Never held across a
 * socket call. */
void eip_hal_lock(void);
void eip_hal_unlock(void);

/* ---- network interfaces ------------------------------------------------- */

/* Fill info for interface `index` (0 = the primary one). Return EIP_OK, or
 * EIP_ERR_ARG when there is no such interface. An interface without an
 * address yet still returns EIP_OK with ip 0.0.0.0. Zero what is unknown. */
int eip_hal_netif_get(uint8_t index, EipNetifInfo *info);

/* ---- TCP ---------------------------------------------------------------- */

/* Connect, waiting up to timeout_ms. EIP_SOCK_INVALID on failure. */
eip_sock_t eip_hal_tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);

/* Listen on all interfaces. EIP_SOCK_INVALID on failure. */
eip_sock_t eip_hal_tcp_listen(uint16_t port, uint8_t backlog);

/* Never blocks. A connected socket, or EIP_SOCK_INVALID when none is
 * waiting. peer_ip receives the client address. */
eip_sock_t eip_hal_tcp_accept(eip_sock_t listener, uint8_t peer_ip[4]);

/* Queue all len bytes for sending. EIP_OK, or EIP_ERR_IO when the
 * connection failed or the bytes could not all be queued within a short
 * bounded time (a few hundred ms). */
int eip_hal_tcp_send(eip_sock_t sock, const uint8_t *data, uint16_t len);

/* Receive up to cap bytes, waiting up to timeout_ms for the first byte.
 * Returns the byte count (> 0), 0 when nothing arrived in time, or
 * EIP_ERR_IO when the peer closed or the connection failed. */
int eip_hal_tcp_recv(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint32_t timeout_ms);

/* The local address this connection uses (for the T->O sockaddr item). */
int eip_hal_tcp_local_ip(eip_sock_t sock, uint8_t ip[4]);

void eip_hal_tcp_close(eip_sock_t sock);

/* ---- UDP ---------------------------------------------------------------- */

enum {
    EIP_UDP_BROADCAST = 0x01,   /* may send to broadcast addresses */
    EIP_UDP_MULTICAST_TX = 0x02 /* may send to multicast groups (TTL 1, primary interface) */
};

/* Bind a UDP socket that can both send and receive. local_port 0 picks
 * any free port. EIP_SOCK_INVALID when the port is taken or on failure. */
eip_sock_t eip_hal_udp_open(uint16_t local_port, uint8_t flags);

/* Send one datagram. EIP_OK or EIP_ERR_IO. */
int eip_hal_udp_sendto(eip_sock_t sock, const uint8_t ip[4], uint16_t port, const uint8_t *data,
                       uint16_t len);

/* Receive one datagram, waiting up to timeout_ms. Returns its length (> 0),
 * 0 when nothing arrived in time, or EIP_ERR_IO. A datagram longer than
 * cap is truncated to cap. ip and port receive the sender; either may be 0. */
int eip_hal_udp_recvfrom(eip_sock_t sock, uint8_t *buf, uint16_t cap, uint8_t ip[4],
                         uint16_t *port, uint32_t timeout_ms);

void eip_hal_udp_close(eip_sock_t sock);

/* ---- waiting ------------------------------------------------------------ */

/* Wait until at least one of the sockets may have data (or a pending
 * connection for a listener), or until timeout_ms passes. May return early.
 * Returns > 0 when something may be ready, 0 on timeout. A port without
 * select() may simply yield and return 1. Invalid handles are skipped. */
int eip_hal_wait(const eip_sock_t *socks, uint8_t count, uint32_t timeout_ms);

/* ---- adapter runner ----------------------------------------------------- */

/* Called once by eip_adapter_start() after the built-in server is set up.
 * RTOS ports start a task that loops eip_server_service(server, ...).
 * Poll-mode ports do nothing and return EIP_OK; the application then calls
 * eip_adapter_poll(). Returns EIP_OK or EIP_ERR_IO. */
int eip_hal_adapter_start_task(struct EipServer *server);

#ifdef __cplusplus
}
#endif

#endif /* EIP_HAL_H_ */
