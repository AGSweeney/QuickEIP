/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Class 1 I/O as an originator: Forward_Open over the session, then UDP.
 *
 * eip_session.c carries the Forward_Open and Forward_Close, eip_codec.c builds
 * every packet, and this file owns the UDP socket and the sequence counts.
 * Nothing here runs in the background: the application paces the O->T packets
 * by calling eip_io_produce once per RPI. The adapter side of the same
 * exchange is in eip_adapter.c. */
#include "eip_codec.h"
#include "eip_hal.h"
#include "eip_internal.h"

#include <string.h>

/* Bind the local T->O port. With prefer 0, try UDP 2223..2231 in order. */
static eip_sock_t OpenUdp(uint16_t prefer, uint16_t *bound_port)
{
    /* Port 2222 belongs to an adapter that may run on this same device, and
     * some stacks hand a second listener's packets to the newer socket. So
     * an automatic bind avoids 2222. An explicit local_port is kept. */
    uint16_t first = (prefer == 0) ? (uint16_t)(EIP_UDP_PORT + 1u) : prefer;
    int tries = (prefer == 0) ? 9 : 1;
    int index;

    for (index = 0; index < tries; index++) {
        uint16_t port = (uint16_t)(first + index);
        eip_sock_t sock = eip_hal_udp_open(port, 0);
        if (sock >= 0) {
            *bound_port = port;
            return sock;
        }
    }
    return EIP_SOCK_INVALID;
}

static void DropUdp(EipIo *io)
{
    if (io->sock_owned && io->sock >= 0) {
        eip_hal_udp_close(io->sock);
    }
    io->sock = EIP_SOCK_INVALID;
    io->sock_owned = 0;
}

int eip_io_open(EipSession *session, EipIo *io, const EipIoConfig *config)
{
    uint8_t path[64];
    uint16_t path_len = 0;
    uint8_t local_ip[4];
    uint8_t extra[40];
    uint16_t extra_len = 0;
    uint16_t one_len = 0;
    uint16_t local_port = 0;
    uint8_t request[256];
    uint16_t request_len = 0;
    uint8_t response[128];
    uint16_t response_len = sizeof response;
    const uint8_t *rr = 0;
    uint16_t rr_len = 0;
    EipForwardOpenInfo info;
    uint8_t tick = 0;
    uint8_t ticks = 0;
    uint32_t o2t_id;
    uint32_t rsp_o2t = 0;
    uint32_t rsp_t2o = 0;
    uint32_t rsp_o2t_api = 0;
    uint32_t rsp_t2o_api = 0;
    int rc;

    if (session == 0 || io == 0 || config == 0) {
        return EIP_ERR_ARG;
    }
    if (!eip_session_is_open(session)) {
        return EIP_ERR_STATE;
    }
    if (config->output_bytes > 600u || config->input_bytes > 600u) {
        return EIP_ERR_TOO_BIG;
    }
    memset(io, 0, sizeof *io);
    io->sock = EIP_SOCK_INVALID;

    rc = eip_build_assembly_path(path, sizeof path, &path_len, config->config_instance,
                                 config->output_instance, config->input_instance);
    if (rc != EIP_OK) {
        return rc;
    }
    if (path_len > sizeof io->path) {
        return EIP_ERR_TOO_BIG;
    }

    io->sock = OpenUdp(config->local_port, &local_port);
    if (io->sock < 0) {
        return EIP_ERR_IO;
    }
    io->sock_owned = 1;
    /* The T->O sockaddr item tells the target where to send, so it needs the
     * address this TCP connection really uses. */
    if (eip_hal_tcp_local_ip(session->sock, local_ip) != EIP_OK ||
        (local_ip[0] == 0 && local_ip[1] == 0 && local_ip[2] == 0 && local_ip[3] == 0)) {
        DropUdp(io);
        return EIP_ERR_STATE;
    }

    /* Two extra CPF items ride with the Forward_Open: where O->T goes and
     * where T->O should come back. */
    rc = eip_build_sockaddr_item(extra, sizeof extra, &one_len, EIP_ITEM_SOCK_O2T, session->ip,
                                 EIP_UDP_PORT);
    if (rc != EIP_OK) {
        DropUdp(io);
        return rc;
    }
    extra_len = one_len;
    rc = eip_build_sockaddr_item(extra + extra_len, (uint16_t)(sizeof extra - extra_len), &one_len,
                                 EIP_ITEM_SOCK_T2O, local_ip, local_port);
    if (rc != EIP_OK) {
        DropUdp(io);
        return rc;
    }
    extra_len = (uint16_t)(extra_len + one_len);

    eip_tick_pair(eip_timeout_ms(session), &tick, &ticks);
    /* Proposed O->T id, unique per connection serial. The target may replace
     * it in its reply, and it assigns the T->O id, which goes out as 0. */
    o2t_id = session->serial_number ^ (0x4F540000u + session->next_conn_serial);
    if (o2t_id == 0) {
        o2t_id = 1;
    }
    memset(&info, 0, sizeof info);
    info.priority_tick = tick;
    info.timeout_ticks = ticks;
    info.o2t_id = o2t_id;
    info.t2o_id = 0;
    info.connection_serial = session->next_conn_serial++;
    if (session->next_conn_serial == 0) {
        session->next_conn_serial = 1;
    }
    info.vendor_id = session->vendor_id;
    info.originator_serial = session->serial_number;
    info.timeout_multiplier =
        (config->timeout_multiplier > 7u) ? 7u : config->timeout_multiplier;
    info.o2t_rpi_us = (config->o2t_rpi_us == 0) ? 20000u : config->o2t_rpi_us;
    info.t2o_rpi_us = (config->t2o_rpi_us == 0) ? 20000u : config->t2o_rpi_us;
    /* Connection sizes on the wire include the 2-byte sequence count and,
     * O->T only, the 4-byte run/idle header. */
    info.o2t_params = eip_large_conn_params(
        (uint16_t)(2u + (config->run_idle ? 4u : 0u) + config->output_bytes), 0, 0, 2, 0);
    info.t2o_params = eip_large_conn_params((uint16_t)(2u + config->input_bytes), 0, 0, 2, 0);
    info.transport = 0x01;

    rc = eip_build_forward_open_request(request, sizeof request, &request_len, &info, path, path_len);
    if (rc != EIP_OK) {
        DropUdp(io);
        return rc;
    }
    rc = eip_send_cip_message(session, request, request_len, extra, extra_len, 2, response,
                              sizeof response, &response_len, &rr, &rr_len);
    if (rc != EIP_OK) {
        DropUdp(io);
        return rc;
    }
    /* The reply was a CIP success but might not parse. The target may hold
     * the connection in that case, so ask it to let go. */
    rc = eip_parse_forward_open_success(response, response_len, &rsp_o2t, &rsp_t2o, &rsp_o2t_api,
                                        &rsp_t2o_api);
    if (rc != EIP_OK) {
        uint8_t close_request[128];
        uint16_t close_len = 0;
        if (eip_build_forward_close_request(close_request, sizeof close_request, &close_len, tick, ticks,
                                            info.connection_serial, info.vendor_id, info.originator_serial,
                                            path, path_len) == EIP_OK) {
            (void)eip_send_cip_message(session, close_request, close_len, 0, 0, 0, 0, 0, 0, 0, 0);
        }
        DropUdp(io);
        return rc;
    }
    (void)rsp_o2t_api;
    (void)rsp_t2o_api;

    io->o2t_connection_id = (rsp_o2t == 0) ? o2t_id : rsp_o2t;
    io->t2o_connection_id = rsp_t2o;
    io->connection_serial = info.connection_serial;
    io->vendor_id = info.vendor_id;
    io->originator_serial = info.originator_serial;
    io->output_bytes = config->output_bytes;
    io->input_bytes = config->input_bytes;
    io->run_idle = config->run_idle ? 1u : 0u;
    io->remote_port = EIP_UDP_PORT;
    memcpy(io->remote_ip, session->ip, 4);
    memcpy(io->path, path, path_len);
    io->path_len = (uint8_t)path_len;
    /* The target may name another address or port for O->T in its reply.
     * Skip the 6 bytes of interface handle and timeout before the CPF list. */
    if (rr != 0 && rr_len > 6u) {
        const uint8_t *sock = 0;
        uint16_t sock_len = 0;
        uint8_t parsed_ip[4];
        uint16_t parsed_port = 0;
        if (eip_cpf_find(rr + 6, (uint16_t)(rr_len - 6u), EIP_ITEM_SOCK_O2T, &sock, &sock_len) ==
                EIP_OK &&
            eip_parse_sockaddr(sock, sock_len, parsed_ip, &parsed_port) == EIP_OK &&
            parsed_port != 0) {
            if (!(parsed_ip[0] == 0 && parsed_ip[1] == 0 && parsed_ip[2] == 0 && parsed_ip[3] == 0)) {
                memcpy(io->remote_ip, parsed_ip, 4);
            }
            io->remote_port = parsed_port;
        }
    }
    io->is_open = 1;
    return EIP_OK;
}

int eip_io_produce(EipIo *io, const uint8_t *data, uint16_t data_len, int run)
{
    uint8_t packet[EIP_MAX_PACKET];
    uint16_t packet_len = 0;
    int rc;

    if (io == 0 || io->is_open == 0 || !io->sock_owned) {
        return EIP_ERR_STATE;
    }
    if (data_len != io->output_bytes || (data_len != 0 && data == 0)) {
        return EIP_ERR_ARG;
    }
    /* Both counts advance on every packet and skip 0 on wrap. */
    io->sequence_32++;
    if (io->sequence_32 == 0) {
        io->sequence_32 = 1;
    }
    io->sequence_16++;
    if (io->sequence_16 == 0) {
        io->sequence_16 = 1;
    }
    rc = eip_build_io_packet(packet, sizeof packet, &packet_len, io->o2t_connection_id, io->sequence_32,
                             io->sequence_16, io->run_idle, run, data, data_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return (eip_hal_udp_sendto(io->sock, io->remote_ip, io->remote_port, packet, packet_len) == EIP_OK)
               ? EIP_OK
               : EIP_ERR_IO;
}

int eip_io_consume(EipIo *io, uint8_t *data, uint16_t *data_len, uint32_t timeout_ms)
{
    uint8_t packet[EIP_MAX_PACKET];
    uint32_t start;

    if (io == 0 || io->is_open == 0 || !io->sock_owned || data_len == 0) {
        return EIP_ERR_STATE;
    }
    if (io->input_bytes != 0 && (data == 0 || *data_len < io->input_bytes)) {
        return EIP_ERR_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = 1000u;
    }
    start = eip_hal_millis();
    for (;;) {
        uint32_t elapsed = eip_hal_millis() - start;
        uint32_t connection_id = 0;
        uint16_t sequence = 0;
        const uint8_t *payload = 0;
        uint16_t payload_len = 0;
        int n;
        int rc;

        if (elapsed >= timeout_ms) {
            return EIP_ERR_TIMEOUT;
        }
        n = eip_hal_udp_recvfrom(io->sock, packet, sizeof packet, 0, 0, timeout_ms - elapsed);
        if (n < 0) {
            return EIP_ERR_IO;
        }
        if (n == 0) {
            continue;
        }
        /* Junk and other connections are skipped, not errors, on a socket
         * anyone can send to. The sender address and sequence are not checked. */
        rc = eip_parse_io_packet(packet, (uint16_t)n, &connection_id, &sequence, &payload, &payload_len);
        if (rc != EIP_OK || connection_id != io->t2o_connection_id) {
            continue;
        }
        (void)sequence;
        if (payload_len < io->input_bytes) {
            return EIP_ERR_PROTOCOL;
        }
        if (io->input_bytes != 0) {
            memcpy(data, payload, io->input_bytes);
        }
        *data_len = io->input_bytes;
        return EIP_OK;
    }
}

int eip_io_close(EipSession *session, EipIo *io)
{
    uint8_t request[128];
    uint16_t request_len = 0;
    uint8_t tick = 0;
    uint8_t ticks = 0;
    int rc = EIP_OK;

    if (io == 0) {
        return EIP_ERR_ARG;
    }
    if (io->is_open && eip_session_is_open(session)) {
        eip_tick_pair(eip_timeout_ms(session), &tick, &ticks);
        rc = eip_build_forward_close_request(request, sizeof request, &request_len, tick, ticks,
                                             io->connection_serial, io->vendor_id, io->originator_serial,
                                             io->path, io->path_len);
        if (rc == EIP_OK) {
            rc = eip_send_cip_message(session, request, request_len, 0, 0, 0, 0, 0, 0, 0, 0);
        }
    }
    DropUdp(io);
    io->is_open = 0;
    return rc;
}
