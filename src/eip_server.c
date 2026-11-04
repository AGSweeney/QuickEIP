/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Adapter server: owns the adapter's sockets and feeds the sans-I/O core.
 * Portable C99; generalized from the ESP-IDF and NetBurner adapter tasks.
 *
 * eip_adapter.c holds the protocol and never touches a socket or a clock.
 * This file does the I/O around it: it opens the sockets, hands each packet
 * to the core with a tick count, and sends what comes back. One tick is one
 * millisecond of eip_hal_millis(). eip_adapter_default.c holds the built-in
 * instance. The port's runner, or the application in poll mode, drives it.
 *
 * Contexts. init, poll, service, close and every static function here run in
 * one context (the adapter task or the main loop). Sockets, client slots,
 * rsp and dgram belong to that context alone and need no lock. Only the core
 * is shared with other tasks, through snapshot, read and write, so
 * eip_hal_lock wraps calls into the core and is released before any socket
 * call.
 *
 * Sections: deferred application callbacks, interface values, sockets and
 * per-pass work, then the public API. eip_server_poll shows the pass order.
 */
#include "eip_server.h"

#include "eip_codec.h"
#include "eip_hal.h"

#include <string.h>

enum { kMaxDrain = 16 }; /* datagrams/accepts handled per socket per pass */

static int IpIsZero(const uint8_t ip[4])
{
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

/* ---- deferred application callbacks ------------------------------------ */

/* The core calls OnSetInterface and OnReset while it builds the reply, so
 * they only record the request. RunDeferred acts on it in a later pass.
 *
 * They are installed only when the matching application callback exists, so
 * the "not settable" and "not supported" returns below are a backstop.
 *
 * cfg is the whole interface configuration, built from the core's values
 * with the one attribute changed, so attribute is not needed. The core keeps
 * accepted values and RefreshNetif leaves them alone while one is pending, so
 * Sets of several attributes inside the delay merge: each replaces
 * pending_interface, restarts the delay and leads to one apply_interface. */
static uint8_t OnSetInterface(void *ctx, uint16_t attribute, const EipInterfaceConfig *cfg)
{
    EipServer *server = (EipServer *)ctx;

    (void)attribute;
    if (server->config.apply_interface == 0) {
        return 0x0E; /* attribute not settable */
    }
    if (server->config.allow_change != 0) {
        uint8_t status = server->config.allow_change(server->config.callback_ctx, EIP_CHANGE_INTERFACE);
        if (status != 0) {
            return status;
        }
    }
    /* Called under eip_hal_lock from inside the core. Apply after the reply
     * has gone out, so the scanner sees the success before the address moves. */
    server->pending_interface = *cfg;
    server->pending_due_ms = eip_hal_millis() + 150u;
    server->pending = 1;
    return 0;
}

static uint8_t OnReset(void *ctx, uint8_t type)
{
    EipServer *server = (EipServer *)ctx;

    if (server->config.reset == 0) {
        return 0x08; /* service not supported */
    }
    if (server->config.allow_change != 0) {
        uint8_t status = server->config.allow_change(server->config.callback_ctx, EIP_CHANGE_RESET);
        if (status != 0) {
            return status;
        }
    }
    /* Called under eip_hal_lock, like OnSetInterface. The longer delay gives
     * the reply time to reach the scanner before the device goes down. */
    server->reset_type = (int16_t)type;
    server->reset_due_ms = eip_hal_millis() + 300u;
    return 0;
}

/* Runs the application callbacks in the adapter context with no lock held, so
 * they may call eip_adapter_* and may take a while. T->O packets stop until
 * they return. The flags are read unlocked because only this context writes
 * them. Due times compare by signed difference, which stays correct across
 * the 2^32 wrap of eip_hal_millis. */
static void RunDeferred(EipServer *server)
{
    uint32_t now = eip_hal_millis();

    if (server->pending && (int32_t)(now - server->pending_due_ms) >= 0) {
        EipInterfaceConfig copy;
        eip_hal_lock();
        copy = server->pending_interface;
        server->pending = 0;
        eip_hal_unlock();
        server->config.apply_interface(server->config.callback_ctx, &copy);
    }
    if (server->reset_type >= 0 && (int32_t)(now - server->reset_due_ms) >= 0) {
        uint8_t type = (uint8_t)server->reset_type;
        server->reset_type = -1;
        server->config.reset(server->config.callback_ctx, type);
    }
}

/* ---- interface values --------------------------------------------------- */

/* Copy the port's view of the interface into the core, every 100 ms from
 * poll. The network stack is the source of truth: once a pending Set has been
 * applied, the next refresh replaces the IP settings, control method, DNS and
 * host name with whatever the port reports, so a value the port does not
 * report back (a host name, on a port that leaves it empty) does not stick.
 * The domain is the exception: it is passed as 0 here, which keeps the core's
 * value.
 *
 * eip_hal_netif_get and link_info run before the lock is taken: they may be
 * slow, and link_info is application code. Link values come from link_info if
 * set, else from the port if it knows them, else "up at 100 Mbit/s full
 * duplex while there is an address". */
static void RefreshNetif(EipServer *server)
{
    EipNetifInfo info;
    uint32_t speed = 0;
    int up = 0;
    int full_duplex = 0;
    int autoneg = 0;

    if (server->pending) {
        return; /* do not overwrite a just-set configuration with the old values */
    }
    memset(&info, 0, sizeof info);
    if (eip_hal_netif_get(server->config.netif_index, &info) != EIP_OK) {
        return;
    }
    if (server->config.link_info != 0) {
        up = server->config.link_info(server->config.callback_ctx, &speed, &full_duplex, &autoneg);
    } else if (info.link_known) {
        up = info.link_up;
        speed = info.speed_mbps;
        full_duplex = info.full_duplex;
        autoneg = info.autonegotiated;
    } else {
        up = !IpIsZero(info.ip);
        speed = up ? 100u : 0u;
        full_duplex = up;
        autoneg = up;
    }
    eip_hal_lock();
    eip_adapter_set_ip(&server->core, info.ip);
    if (!IpIsZero(info.netmask)) {
        eip_adapter_set_netmask(&server->core, info.netmask);
    }
    eip_adapter_set_interface(&server->core, info.mac, info.gateway, info.dhcp ? 2u : 0u,
                              info.host_name);
    eip_adapter_set_link(&server->core, up, speed, full_duplex, autoneg);
    eip_adapter_set_dns(&server->core, info.dns1, info.dns2, 0);
    if (server->serial_from_mac && server->core.serial_number <= 1u &&
        (info.mac[2] | info.mac[3] | info.mac[4] | info.mac[5]) != 0) {
        /* The MAC was not known at init (interface still coming up). The core
         * turns serial 0 into 1, hence the <= 1 test. */
        server->core.serial_number = ((uint32_t)info.mac[2] << 24) | ((uint32_t)info.mac[3] << 16) |
                                     ((uint32_t)info.mac[4] << 8) | (uint32_t)info.mac[5];
    }
    eip_hal_unlock();
}

/* ---- sockets ------------------------------------------------------------ */

static int AllOpen(const EipServer *server)
{
    return server->tcp >= 0 && server->udp_id >= 0 && server->udp_io >= 0;
}

/* Open whatever is missing and keep what is already open; poll calls this
 * once a second until all three exist. A socket that opened and later fails
 * is not detected or reopened: receive errors just end that socket's drain. */
static void OpenSockets(EipServer *server)
{
    if (server->tcp < 0) {
        server->tcp = eip_hal_tcp_listen(EIP_TCP_PORT, EIP_ADAPTER_MAX_CLIENTS);
    }
    if (server->udp_id < 0) {
        server->udp_id = eip_hal_udp_open(EIP_TCP_PORT, EIP_UDP_BROADCAST);
    }
    if (server->udp_io < 0) {
        server->udp_io = eip_hal_udp_open(EIP_UDP_PORT, EIP_UDP_MULTICAST_TX);
    }
    eip_hal_lock();
    server->core.listening = AllOpen(server) ? 1u : 0u;
    eip_hal_unlock();
}

/* Frees the slot only. The core is not told: a Class 1 connection does not
 * depend on the TCP session that opened it, and ends by Forward_Close or by
 * its own inactivity timeout. */
static void DropClient(EipServerClient *client)
{
    if (client->sock >= 0) {
        eip_hal_tcp_close(client->sock);
    }
    client->sock = EIP_SOCK_INVALID;
    client->session = 0;
    client->got = 0;
}

static void AcceptClients(EipServer *server)
{
    int round;

    for (round = 0; round < kMaxDrain; round++) {
        uint8_t peer[4];
        eip_sock_t sock = eip_hal_tcp_accept(server->tcp, peer);
        int slot;

        if (sock < 0) {
            return;
        }
        for (slot = 0; slot < EIP_ADAPTER_MAX_CLIENTS && server->clients[slot].sock >= 0; slot++) {
        }
        if (slot >= EIP_ADAPTER_MAX_CLIENTS) {
            /* Every slot busy. A peer that vanished (cable pulled, PLC reset)
             * must not lock scanners out: evict the session silent longest,
             * once it has been silent EIP_ADAPTER_EVICT_MS. Never the session
             * of the current I/O owner, which stays quiet while Class 1 runs.
             * The owner is matched by IP address, so every session from that
             * address is exempt. If nothing qualifies the newcomer is closed.
             * Age is an unsigned difference, safe across the clock wrap. */
            uint8_t owner[4];
            uint8_t io_open;
            uint32_t now = eip_hal_millis();
            int oldest = -1;
            int i;

            eip_hal_lock();
            io_open = server->core.io_open;
            memcpy(owner, server->core.o2t_source_ip, 4);
            eip_hal_unlock();
            for (i = 0; i < EIP_ADAPTER_MAX_CLIENTS; i++) {
                if (io_open && memcmp(server->clients[i].ip, owner, 4) == 0) {
                    continue;
                }
                if (oldest < 0 ||
                    (int32_t)(server->clients[i].last_rx_ms - server->clients[oldest].last_rx_ms) < 0) {
                    oldest = i;
                }
            }
            if (oldest >= 0 && (now - server->clients[oldest].last_rx_ms) >= EIP_ADAPTER_EVICT_MS) {
                DropClient(&server->clients[oldest]);
                slot = oldest;
            } else {
                eip_hal_tcp_close(sock);
                continue;
            }
        }
        server->clients[slot].sock = sock;
        server->clients[slot].session = 0;
        server->clients[slot].got = 0;
        server->clients[slot].last_rx_ms = eip_hal_millis();
        memcpy(server->clients[slot].ip, peer, 4);
    }
}

/* Read what the socket has and answer every complete encapsulation packet in
 * the client's buffer. TCP gives no packet boundaries, so a partial packet
 * stays in buf for the next pass. Any receive error, malformed packet or
 * failed reply drops the client. An empty slot skips the loop.
 *
 * eip_hal_tcp_send can wait for the port's send budget (a few hundred ms), so
 * a client that has stopped reading can delay this whole pass, T->O included. */
static void ServiceClient(EipServer *server, EipServerClient *client)
{
    int round;

    for (round = 0; round < kMaxDrain && client->sock >= 0; round++) {
        uint16_t space = (uint16_t)(sizeof client->buf - client->got);
        int n;

        if (space == 0) {
            DropClient(client); /* a packet larger than the buffer */
            return;
        }
        n = eip_hal_tcp_recv(client->sock, client->buf + client->got, space, 0);
        if (n < 0) {
            DropClient(client);
            return;
        }
        if (n == 0) {
            return;
        }
        client->last_rx_ms = eip_hal_millis();
        client->got = (uint16_t)(client->got + (uint16_t)n);
        while (client->sock >= 0 && client->got >= 24u) {
            /* 24-byte encapsulation header; its length field is at offset 2. */
            uint32_t need = 24u + eip_load_le16(client->buf + 2);
            uint16_t rsp_len = 0;
            int rc;

            if (need > sizeof client->buf) {
                DropClient(client);
                return;
            }
            if (client->got < need) {
                break;
            }
            eip_hal_lock();
            rc = eip_adapter_on_encap(&server->core, &client->session, client->ip, client->buf,
                                      (uint16_t)need, eip_hal_millis(), server->rsp, sizeof server->rsp,
                                      &rsp_len);
            eip_hal_unlock();
            if (rc != EIP_OK) {
                DropClient(client);
                return;
            }
            if (rsp_len > 0 && eip_hal_tcp_send(client->sock, server->rsp, rsp_len) != EIP_OK) {
                DropClient(client);
                return;
            }
            if (client->got > need) {
                memmove(client->buf, client->buf + need, client->got - need);
            }
            client->got = (uint16_t)(client->got - need);
        }
    }
}

/* Token bucket for UDP 44818 replies. One token per EIP_ADAPTER_UDP_REPLY_MS,
 * capped at EIP_ADAPTER_UDP_REPLY_BURST. reply_last_ms moves only by whole
 * token periods, so the remainder of a partial period is not lost. The
 * unsigned difference is safe across the clock wrap.
 *
 * The check comes before the request is parsed, so a token is spent per
 * request of header size or more, whether or not it earns a reply, and
 * udp_replies_dropped counts requests refused here. */
static int ReplyAllowed(EipServer *server)
{
    uint32_t now = eip_hal_millis();
    uint32_t add = (now - server->reply_last_ms) / EIP_ADAPTER_UDP_REPLY_MS;

    if (add != 0) {
        server->reply_tokens = (server->reply_tokens + add > EIP_ADAPTER_UDP_REPLY_BURST)
                                   ? EIP_ADAPTER_UDP_REPLY_BURST
                                   : server->reply_tokens + add;
        server->reply_last_ms += add * EIP_ADAPTER_UDP_REPLY_MS;
    }
    if (server->reply_tokens == 0) {
        server->udp_replies_dropped++;
        return 0;
    }
    server->reply_tokens--;
    return 1;
}

/* UDP 44818: the session-free commands (ListIdentity and the like). session
 * starts at 0 for every datagram and is thrown away, so SendRRData is refused
 * here and a RegisterSession over UDP gives a handle nothing can use. */
static void PollIdentity(EipServer *server)
{
    int round;

    for (round = 0; round < kMaxDrain && server->udp_id >= 0; round++) {
        uint8_t peer[4];
        uint16_t peer_port = 0;
        uint32_t session = 0;
        uint16_t rsp_len = 0;
        int rc;
        int n = eip_hal_udp_recvfrom(server->udp_id, server->dgram, sizeof server->dgram, peer, &peer_port, 0);

        if (n <= 0) {
            return;
        }
        if (n < 24 || !ReplyAllowed(server)) {
            continue;
        }
        eip_hal_lock();
        rc = eip_adapter_on_encap(&server->core, &session, peer, server->dgram, (uint16_t)n, eip_hal_millis(),
                                  server->rsp, sizeof server->rsp, &rsp_len);
        eip_hal_unlock();
        if (rc == EIP_OK && rsp_len > 0) {
            (void)eip_hal_udp_sendto(server->udp_id, peer, peer_port, server->rsp, rsp_len);
        }
    }
}

/* UDP 2222: Class 1 O->T packets. The core filters by length, connection,
 * source and sequence and keeps its own drop counters, so its result is not
 * needed. */
static void PollIo(EipServer *server)
{
    int round;

    for (round = 0; round < kMaxDrain && server->udp_io >= 0; round++) {
        uint8_t peer[4];
        int n = eip_hal_udp_recvfrom(server->udp_io, server->dgram, sizeof server->dgram, peer, 0, 0);

        if (n <= 0) {
            return;
        }
        eip_hal_lock();
        (void)eip_adapter_take_input_from(&server->core, server->dgram, (uint16_t)n, eip_hal_millis(), peer);
        eip_hal_unlock();
    }
}

/* Check the inactivity timeout, then send at most one T->O packet per pass.
 * The packet is built under the lock, which fixes the assembly data it
 * carries, and sent after the lock is released. If passes fall behind the
 * RPI, the core restarts its schedule from now and does not send a burst. */
static void Produce(EipServer *server)
{
    uint16_t len = 0;
    uint8_t ip[4];
    uint16_t port = 0;
    uint32_t now = eip_hal_millis();
    int rc = EIP_ERR_STATE;

    eip_hal_lock();
    eip_adapter_check_timeout(&server->core, now);
    if (eip_adapter_io_due(&server->core, now)) {
        /* server->rsp is free here: client replies are sent before this runs. */
        rc = eip_adapter_build_output(&server->core, now, server->rsp, sizeof server->rsp, &len, ip, &port);
    }
    eip_hal_unlock();
    if (rc == EIP_OK && server->udp_io >= 0 && port != 0) {
        (void)eip_hal_udp_sendto(server->udp_io, ip, port, server->rsp, len);
    }
}

/* Ms until the next T->O packet, capped at EIP_ADAPTER_IDLE_WAIT_MS. The cap
 * also bounds how late timeouts and deferred callbacks run, as they have no
 * deadline here. Ticks are ms, so next_tx_tick compares directly with the
 * clock. The signed difference is safe across the wrap; d <= 0 means overdue. */
static uint32_t NextDue(EipServer *server)
{
    uint32_t wait = EIP_ADAPTER_IDLE_WAIT_MS;

    eip_hal_lock();
    if (server->core.io_open && !server->core.t2o_null) {
        int32_t d = (int32_t)(server->core.next_tx_tick - eip_hal_millis());
        wait = (d <= 0) ? 0u : (((uint32_t)d < wait) ? (uint32_t)d : wait);
    }
    eip_hal_unlock();
    return wait;
}

/* ---- public ------------------------------------------------------------- */

/* An error return leaves no sockets open and initialized at 0. An interface
 * that is not up yet is not an error: RefreshNetif fills in the address, and
 * the MAC-derived serial, later on. */
int eip_server_init(EipServer *server, const EipAdapterConfig *config)
{
    EipNetifInfo info;
    uint32_t serial;
    uint32_t floor_ms;
    int i;

    if (server == 0) {
        return EIP_ERR_ARG;
    }
    memset(server, 0, sizeof *server);
    server->tcp = EIP_SOCK_INVALID;
    server->udp_id = EIP_SOCK_INVALID;
    server->udp_io = EIP_SOCK_INVALID;
    for (i = 0; i < EIP_ADAPTER_MAX_CLIENTS; i++) {
        server->clients[i].sock = EIP_SOCK_INVALID;
    }
    if (config != 0) {
        server->config = *config;
    }
    server->reset_type = -1;
    server->reply_tokens = EIP_ADAPTER_UDP_REPLY_BURST;
    server->reply_last_ms = eip_hal_millis();

    memset(&info, 0, sizeof info);
    (void)eip_hal_netif_get(server->config.netif_index, &info);
    serial = server->config.serial_number;
    if (serial == 0) {
        server->serial_from_mac = 1;
        serial = ((uint32_t)info.mac[2] << 24) | ((uint32_t)info.mac[3] << 16) | ((uint32_t)info.mac[4] << 8) |
                 (uint32_t)info.mac[5];
    }
    eip_adapter_init(&server->core, info.ip, serial);
    eip_adapter_set_clock(&server->core, 1000u); /* ticks are eip_hal_millis() */

    /* Replaces the core's 2000 ms default. A timeout under two clock steps
     * could expire between two readings of a coarse clock. */
    floor_ms = 2u * eip_hal_clock_resolution_ms();
    if (server->config.timeout_floor_ms > floor_ms) {
        floor_ms = server->config.timeout_floor_ms;
    }
    eip_adapter_set_timeout_floor_ms(&server->core, floor_ms);

    if (server->config.vendor_id != 0) {
        eip_adapter_set_identity(&server->core, server->config.vendor_id, server->config.device_type,
                                 server->config.product_code, server->config.revision_major,
                                 server->config.revision_minor, server->config.product_name);
    }
    if (server->config.assembly_count != 0) {
        if (server->config.assemblies == 0) {
            return EIP_ERR_ARG;
        }
        eip_adapter_clear_assemblies(&server->core);
        for (i = 0; i < server->config.assembly_count; i++) {
            const EipAssemblyConfig *a = &server->config.assemblies[i];
            int rc = eip_adapter_add_assembly(&server->core, a->instance, a->direction, a->size, a->name,
                                              a->initial);
            if (rc != EIP_OK) {
                return rc;
            }
            if (a->implicit_only) {
                (void)eip_adapter_set_implicit_only(&server->core, a->instance, 1);
            }
        }
    }
    eip_adapter_set_callbacks(&server->core, server->config.apply_interface ? OnSetInterface : 0,
                              server->config.reset ? OnReset : 0, server);
    if (server->config.domain != 0) {
        eip_adapter_set_dns(&server->core, 0, 0, server->config.domain);
    }
    server->config.assemblies = 0; /* not kept: the caller's array may go away */
    server->config.product_name = 0;
    server->config.domain = 0;

    RefreshNetif(server);
    OpenSockets(server);
    server->last_open_ms = eip_hal_millis();
    server->last_netif_ms = server->last_open_ms;
    server->initialized = 1;
    return EIP_OK;
}

/* One pass. The order matters:
 *   - Class 1 input comes before the timeout check in Produce, so packets
 *     queued during a stall (a blocking scanner call on bare metal) refresh
 *     the inactivity timer before it is judged.
 *   - Clients are served before new ones are accepted, so eviction sees
 *     fresh last_rx_ms values and slots freed by this pass's drops.
 *   - Produce follows the client replies because it reuses server->rsp.
 *   - RunDeferred is last: its callbacks may be slow or move the address, and
 *     everything this pass owed the network has gone out by then.
 * The interval checks use unsigned differences, safe across the clock wrap. */
uint32_t eip_server_poll(EipServer *server)
{
    uint32_t now;
    int i;

    if (server == 0 || !server->initialized) {
        return EIP_ADAPTER_IDLE_WAIT_MS;
    }
    now = eip_hal_millis();
    if (!AllOpen(server) && (now - server->last_open_ms) >= 1000u) {
        OpenSockets(server);
        server->last_open_ms = now;
    }
    if ((now - server->last_netif_ms) >= 100u) {
        RefreshNetif(server);
        server->last_netif_ms = now;
    }
    PollIo(server);
    PollIdentity(server);
    for (i = 0; i < EIP_ADAPTER_MAX_CLIENTS; i++) {
        ServiceClient(server, &server->clients[i]);
    }
    if (server->tcp >= 0) {
        AcceptClients(server);
    }
    Produce(server);
    RunDeferred(server);
    return NextDue(server);
}

void eip_server_service(EipServer *server, uint32_t max_wait_ms)
{
    eip_sock_t socks[3 + EIP_ADAPTER_MAX_CLIENTS];
    uint8_t count = 0;
    uint32_t wait;
    int i;

    if (server == 0 || !server->initialized) {
        eip_hal_yield();
        return;
    }
    wait = NextDue(server);
    if (wait > max_wait_ms) {
        wait = max_wait_ms;
    }
    /* Unopened sockets and free client slots go in as invalid handles, which
     * eip_hal_wait skips. An overdue T->O packet skips the wait altogether. */
    socks[count++] = server->tcp;
    socks[count++] = server->udp_id;
    socks[count++] = server->udp_io;
    for (i = 0; i < EIP_ADAPTER_MAX_CLIENTS; i++) {
        socks[count++] = server->clients[i].sock;
    }
    if (wait > 0) {
        (void)eip_hal_wait(socks, count, wait);
    }
    (void)eip_server_poll(server);
}

/* Adapter context only: nothing stops a running adapter task first. Does not
 * check initialized, so the server must have been through eip_server_init: in
 * a zeroed EipServer the handles read as 0, which is a valid handle on some
 * ports. An open Class 1 connection is not closed in the core; a new init
 * clears it. */
void eip_server_close(EipServer *server)
{
    int i;

    /* A zeroed server has handles of 0, which are valid: closing them would close someone else's. */
    if (server == 0 || !server->initialized) {
        return;
    }
    for (i = 0; i < EIP_ADAPTER_MAX_CLIENTS; i++) {
        DropClient(&server->clients[i]);
    }
    if (server->tcp >= 0) {
        eip_hal_tcp_close(server->tcp);
    }
    if (server->udp_id >= 0) {
        eip_hal_udp_close(server->udp_id);
    }
    if (server->udp_io >= 0) {
        eip_hal_udp_close(server->udp_io);
    }
    server->tcp = EIP_SOCK_INVALID;
    server->udp_id = EIP_SOCK_INVALID;
    server->udp_io = EIP_SOCK_INVALID;
    eip_hal_lock();
    server->core.listening = 0;
    server->initialized = 0;
    eip_hal_unlock();
}

/* snapshot, read and write may run in any task. They touch the core only
 * under the lock. The two things read here that the adapter context writes
 * without the lock, the client handles and udp_replies_dropped, are
 * diagnostics, so a value one pass old does no harm. */
int eip_server_snapshot(EipServer *server, EipAdapterView *view)
{
    const EipAdapter *a;
    uint8_t i;

    if (server == 0 || view == 0) {
        return EIP_ERR_ARG;
    }
    if (!server->initialized) {
        return EIP_ERR_STATE;
    }
    memset(view, 0, sizeof *view);
    eip_hal_lock();
    a = &server->core;
    view->running = 1;
    view->listening = a->listening;
    view->vendor_id = a->vendor_id;
    view->device_type = a->device_type;
    view->product_code = a->product_code;
    view->revision_major = a->revision_major;
    view->revision_minor = a->revision_minor;
    view->serial_number = a->serial_number;
    memcpy(view->name, a->product_name, sizeof view->name);
    view->name[sizeof view->name - 1u] = '\0';
    memcpy(view->ip, a->ip, 4);
    view->io_open = a->io_open;
    view->run = a->run;
    view->multicast = a->t2o_multicast;
    view->o2t_instance = a->o2t_instance;
    view->t2o_instance = a->t2o_instance;
    /* One tick is 1 ms, so the RPI reads back in whole milliseconds. */
    view->rpi_us = a->io_open ? a->interval_ticks * 1000u : 0u;
    view->timeout_ms = a->io_open ? a->timeout_ticks : 0u;
    view->rx_packets = a->rx_packets;
    view->tx_packets = a->tx_packets;
    view->opens = a->opens;
    view->timeouts = a->timeouts;
    view->rx_dropped_seq = a->rx_dropped_seq;
    view->rx_dropped_source = a->rx_dropped_source;
    view->rx_dropped_bad = a->rx_dropped_bad;
    view->rx_idle = a->rx_idle;
    view->udp_replies_dropped = server->udp_replies_dropped;
    memcpy(view->originator_ip, a->originator_ip, 4);
    memcpy(view->peer_ip, a->o2t_source_ip, 4);
    for (i = 0; i < EIP_ADAPTER_MAX_CLIENTS; i++) {
        if (server->clients[i].sock >= 0) {
            view->tcp_clients++;
        }
    }
    view->assembly_count = a->asm_count;
    for (i = 0; i < a->asm_count && i < EIP_ADAPTER_MAX_ASSEMBLIES; i++) {
        view->assemblies[i].instance = a->asms[i].instance;
        view->assemblies[i].direction = a->asms[i].direction;
        view->assemblies[i].size = a->asms[i].size;
        memcpy(view->assemblies[i].name, a->asms[i].name, sizeof view->assemblies[i].name);
    }
    eip_hal_unlock();
    return EIP_OK;
}

int eip_server_read(EipServer *server, uint16_t instance, uint8_t *data, uint16_t *data_len)
{
    int rc;

    if (server == 0) {
        return EIP_ERR_ARG;
    }
    if (!server->initialized) {
        return EIP_ERR_STATE;
    }
    eip_hal_lock();
    rc = eip_adapter_copy(&server->core, instance, data, data_len);
    eip_hal_unlock();
    return rc;
}

int eip_server_write(EipServer *server, uint16_t instance, const uint8_t *data, uint16_t data_len)
{
    int rc;

    if (server == 0) {
        return EIP_ERR_ARG;
    }
    if (!server->initialized) {
        return EIP_ERR_STATE;
    }
    eip_hal_lock();
    rc = eip_adapter_store(&server->core, instance, data, data_len);
    eip_hal_unlock();
    return rc;
}
