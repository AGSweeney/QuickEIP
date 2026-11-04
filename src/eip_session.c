/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Scanner side: encapsulation session, explicit messaging, tags, symbols.
 * Portable C99; all socket and clock access goes through eip_hal.h.
 *
 * Packet layouts live in eip_codec.c. Class 1 I/O (eip_io.c) sends its Forward_Open through
 * eip_send_cip_message here. Every request is one blocking exchange on the session's TCP socket:
 * one send, then reads until its reply arrives.
 *
 * The three session buffers:
 *   tx       the outgoing encapsulation packet
 *   rx       the incoming packet; before a send it also stages the SendRRData body
 *   scratch  the CIP message being sent, then a symbol page in eip_list_symbols
 *
 * Most errors leave the session open, and the caller decides when to close and reopen. The
 * exception is a broken packet stream (see RecvEncap), which closes the session here: with no way
 * to find the next packet boundary, nothing later could be trusted. */
#include "eip_codec.h"
#include "eip_hal.h"
#include "eip_internal.h"

#include <string.h>

/* ---- encapsulation exchange ---- */

/* sock_owned is tested because a zeroed struct has sock 0, which looks like a valid handle. */
static int SessionOpen(const EipSession *session)
{
    return session != 0 && session->sock_owned && session->sock >= 0 && session->handle != 0;
}

/* Read exactly length bytes or fail. timeout_ms covers the whole read, and the elapsed time is a
 * difference so it survives eip_hal_millis wrapping. A recv of 0 only means nothing arrived yet.
 * *received reports how many bytes were taken, which on a failure tells a quiet socket from a
 * packet cut short. */
static int RecvFull(eip_sock_t sock, uint8_t *buf, uint16_t length, uint32_t timeout_ms,
                    uint16_t *received)
{
    uint32_t start = eip_hal_millis();
    uint16_t got = 0;

    *received = 0;
    while (got < length) {
        uint32_t elapsed = eip_hal_millis() - start;
        int n;

        if (elapsed >= timeout_ms) {
            return EIP_ERR_TIMEOUT;
        }
        n = eip_hal_tcp_recv(sock, buf + got, (uint16_t)(length - got), timeout_ms - elapsed);
        if (n < 0) {
            return EIP_ERR_IO;
        }
        got = (uint16_t)(got + (uint16_t)n);
        *received = got;
    }
    return EIP_OK;
}

/* Read one encapsulation packet (header plus body) into session->rx. timeout_ms bounds the wait
 * for the header. Once a header has arrived the body gets the full session timeout.
 *
 * A timeout with nothing read leaves the stream intact, and the session stays open. A packet cut
 * short or too big for rx leaves part of it unread with no way to find the next boundary, so the
 * session closes itself and later calls return EIP_ERR_STATE. */
static int RecvEncap(EipSession *session, EipEncapView *view, uint32_t timeout_ms)
{
    uint16_t body_len;
    uint16_t received = 0;
    int rc = RecvFull(session->sock, session->rx, 24, timeout_ms, &received);

    if (rc != EIP_OK) {
        if (received != 0) {
            eip_session_close(session);
        }
        return rc;
    }
    body_len = eip_load_le16(session->rx + 2);
    /* A peer must not be able to advertise more than session->rx holds. */
    if (body_len > (uint16_t)(EIP_MAX_PACKET - 24)) {
        eip_session_close(session);
        return EIP_ERR_TOO_BIG;
    }
    rc = RecvFull(session->sock, session->rx + 24, body_len, session->timeout_ms, &received);
    if (rc != EIP_OK) {
        eip_session_close(session);
        return rc;
    }
    return eip_parse_encap(session->rx, (uint16_t)(24u + body_len), view);
}

/* How far back a sender context may be and still count as a late reply. A session gives up on one
 * request per timeout, so this only has to cover a run of timeouts. */
enum { kLateReplyWindow = 64 };

/* 1 when a reply answers an earlier request of this session. Contexts are the sequence number in
 * the first 4 bytes, then zeros, and start at 1. Anything else, including a device that does not
 * echo the context, is not a late reply, and fails the check in Exchange. */
static int IsLateReply(const EipSession *session, const uint8_t *context)
{
    uint32_t seen = eip_load_le32(context);
    uint32_t behind = session->context_seq - seen;

    return seen != 0 && eip_load_le32(context + 4) == 0 && behind != 0 && behind <= kLateReplyWindow;
}

/* Send one encapsulation request and read its reply. The reply must echo the sender context, carry
 * status 0, and repeat the command. All three failures are EIP_ERR_PROTOCOL; eip_last_encap_status
 * holds the peer's status. body may be session->rx, because it is copied to tx before the read.
 *
 * A reply that arrives after its request timed out is discarded here, inside this request's
 * timeout, so one slow answer does not leave the session a reply behind. */
static int Exchange(EipSession *session, uint16_t command, const uint8_t *body, uint16_t body_len,
                    EipEncapView *view)
{
    uint8_t context[8];
    uint16_t packet_len = 0;
    uint32_t start;
    int rc;

    /* A fresh context per request lets the reads below recognise a stale reply. */
    memset(context, 0, sizeof context);
    session->context_seq++;
    eip_store_le32(context, session->context_seq);
    rc = eip_build_encap(session->tx, EIP_MAX_PACKET, command, session->handle, context, body,
                         body_len, &packet_len);
    if (rc != EIP_OK) {
        return rc;
    }
    if (eip_hal_tcp_send(session->sock, session->tx, packet_len) != EIP_OK) {
        return EIP_ERR_IO;
    }
    start = eip_hal_millis();
    for (;;) {
        uint32_t elapsed = eip_hal_millis() - start;

        if (elapsed >= session->timeout_ms) {
            return EIP_ERR_TIMEOUT;
        }
        rc = RecvEncap(session, view, session->timeout_ms - elapsed);
        if (rc != EIP_OK) {
            return rc;
        }
        if (!IsLateReply(session, view->context)) {
            break;
        }
    }
    session->encap_status = view->status;
    if (memcmp(view->context, context, 8) != 0) {
        return EIP_ERR_PROTOCOL;
    }
    if (view->status != 0) {
        return EIP_ERR_PROTOCOL;
    }
    if (view->command != command) {
        return EIP_ERR_PROTOCOL;
    }
    return EIP_OK;
}

uint32_t eip_timeout_ms(const EipSession *session)
{
    uint32_t ms = session->timeout_ms;
    if (ms < 100u) {
        ms = 100u;
    }
    return ms;
}

/* ---- session ---- */

/* Connect and send RegisterSession. The memset below drops any earlier state without closing it,
 * so opening a session that is still open leaks its socket. Registration does not go through
 * Exchange: there is no handle yet, and the reply's context is not checked. */
int eip_session_open(EipSession *session, const EipOpenConfig *config)
{
    uint8_t body[4];
    uint8_t context[8];
    uint16_t packet_len = 0;
    EipEncapView view;
    int rc;

    if (session == 0 || config == 0 || config->ip == 0 || config->ip[0] == '\0') {
        return EIP_ERR_ARG;
    }
    if (config->slot < -1 || config->slot > 255) {
        return EIP_ERR_ARG;
    }
    memset(session, 0, sizeof *session);
    session->sock = EIP_SOCK_INVALID;
    session->slot = config->slot;
    session->vendor_id = config->vendor_id;
    session->serial_number = (config->serial_number == 0) ? 1u : config->serial_number;
    session->next_conn_serial = 1;
    session->timeout_ms = (config->timeout_ms == 0) ? 5000u : config->timeout_ms;
    session->tcp_port = (config->tcp_port == 0) ? (uint16_t)EIP_TCP_PORT : config->tcp_port;

    if (eip_parse_ipv4(config->ip, session->ip) != EIP_OK ||
        (session->ip[0] == 0 && session->ip[1] == 0 && session->ip[2] == 0 && session->ip[3] == 0)) {
        return EIP_ERR_ARG;
    }
    session->sock = eip_hal_tcp_connect(session->ip, session->tcp_port, session->timeout_ms);
    if (session->sock < 0) {
        session->sock = EIP_SOCK_INVALID;
        return EIP_ERR_CONNECT;
    }
    session->sock_owned = 1;

    body[0] = 0x01; /* protocol version 1 */
    body[1] = 0x00;
    body[2] = 0x00; /* options */
    body[3] = 0x00;
    memset(context, 0, sizeof context);
    session->context_seq = 1;
    eip_store_le32(context, session->context_seq);
    rc = eip_build_encap(session->tx, EIP_MAX_PACKET, EIP_CMD_REGISTER, 0, context, body, 4,
                         &packet_len);
    if (rc != EIP_OK) {
        eip_session_close(session);
        return rc;
    }
    if (eip_hal_tcp_send(session->sock, session->tx, packet_len) != EIP_OK) {
        eip_session_close(session);
        return EIP_ERR_IO;
    }
    rc = RecvEncap(session, &view, session->timeout_ms);
    if (rc != EIP_OK && rc != EIP_ERR_PROTOCOL) {
        eip_session_close(session);
        return rc;
    }
    if (rc != EIP_OK || view.status != 0 || view.command != EIP_CMD_REGISTER || view.length < 4u ||
        view.session == 0) {
        /* eip_session_close leaves encap_status alone, so the caller can still read it. */
        session->encap_status = (rc == EIP_OK) ? view.status : 0;
        eip_session_close(session);
        return EIP_ERR_PROTOCOL;
    }
    session->handle = view.session;
    session->encap_status = 0;
    return EIP_OK;
}

/* Safe on a zeroed or already closed session. UnRegisterSession gets no reply, so it is sent best
 * effort and the socket closes straight after. It is skipped when registration never finished. */
void eip_session_close(EipSession *session)
{
    uint8_t context[8];
    uint16_t packet_len = 0;

    if (session == 0) {
        return;
    }
    if (session->sock_owned && session->sock >= 0 && session->handle != 0) {
        memset(context, 0, sizeof context);
        if (eip_build_encap(session->tx, EIP_MAX_PACKET, EIP_CMD_UNREGISTER, session->handle, context,
                            0, 0, &packet_len) == EIP_OK) {
            (void)eip_hal_tcp_send(session->sock, session->tx, packet_len);
        }
    }
    if (session->sock_owned && session->sock >= 0) {
        eip_hal_tcp_close(session->sock);
    }
    session->sock = EIP_SOCK_INVALID;
    session->sock_owned = 0;
    session->handle = 0;
}

/* An out-of-range slot is ignored and the old value stays. */
void eip_session_set_slot(EipSession *session, int16_t slot)
{
    if (session != 0 && slot >= -1 && slot <= 255) {
        session->slot = slot;
    }
}

int eip_session_is_open(const EipSession *session)
{
    return SessionOpen(session);
}

uint8_t eip_last_cip_status(const EipSession *session)
{
    return (session == 0) ? 0 : session->cip_status;
}

uint16_t eip_last_cip_extended(const EipSession *session)
{
    return (session == 0) ? 0 : session->cip_extended;
}

uint32_t eip_last_encap_status(const EipSession *session)
{
    return (session == 0) ? 0 : session->encap_status;
}

/* ---- explicit messaging ---- */

static void RememberCip(EipSession *session, uint8_t status, uint16_t extended, uint8_t *cip_status,
                        uint16_t *extended_status)
{
    session->cip_status = status;
    session->cip_extended = extended;
    if (cip_status != 0) {
        *cip_status = status;
    }
    if (extended_status != 0) {
        *extended_status = extended;
    }
}

/* The one path every explicit message takes. The contract is in eip_internal.h.
 *
 * Buffer use: cip_in is copied to scratch + 544 (at most 480 bytes), and the Unconnected Send
 * wrapper is built in scratch[0..540). That needs EIP_MAX_PACKET to be at least 1024, which
 * nothing enforces. The SendRRData body is staged in rx, free until the reply arrives, and
 * Exchange copies it to tx.
 *
 * The CIP status is recorded only when a CIP reply is parsed. A socket, encapsulation or framing
 * failure returns early and leaves the previous status in session->cip_status. The reply is
 * checked for bit 0x80 but not matched to the service that was sent. */
int eip_send_cip_message(EipSession *session, const uint8_t *cip_in, uint16_t cip_in_len,
                         const uint8_t *extra, uint16_t extra_len, uint16_t extra_items,
                         uint8_t *response, uint16_t response_max, uint16_t *response_len,
                         const uint8_t **rr_body, uint16_t *rr_len)
{
    uint8_t tick = 0;
    uint8_t ticks = 0;
    uint16_t cip_len = 0;
    uint16_t rr_body_len = 0;
    uint16_t timeout_sec;
    const uint8_t *cip;
    const uint8_t *cpf = 0;
    uint16_t cpf_len = 0;
    const uint8_t *unconnected = 0;
    uint16_t unconnected_len = 0;
    EipEncapView view;
    int rc;

    if (!SessionOpen(session)) {
        return EIP_ERR_STATE;
    }
    if (session->slot > 255 || cip_in == 0 || cip_in_len == 0 || cip_in_len > 480u) {
        return EIP_ERR_ARG;
    }
    memcpy(session->scratch + 544, cip_in, cip_in_len);
    cip = session->scratch + 544;
    cip_len = cip_in_len;
    if (session->slot >= 0) {
        eip_tick_pair(eip_timeout_ms(session), &tick, &ticks);
        rc = eip_wrap_unconnected(session->scratch, 540, cip, cip_len, (uint8_t)session->slot, tick,
                                  ticks, &cip_len);
        if (rc != EIP_OK) {
            return rc;
        }
        cip = session->scratch;
    }
    timeout_sec = (uint16_t)(eip_timeout_ms(session) / 1000u);
    if (timeout_sec < 1u) {
        timeout_sec = 1u;
    }
    rc = eip_build_rr_body(session->rx, EIP_MAX_PACKET, timeout_sec, cip, cip_len, extra, extra_len,
                           extra_items, &rr_body_len);
    if (rc != EIP_OK) {
        return rc;
    }
    rc = Exchange(session, EIP_CMD_SEND_RR, session->rx, rr_body_len, &view);
    if (rc != EIP_OK) {
        return rc;
    }
    /* The reply body opens with interface handle (4) and timeout (2), then the CPF item count. */
    if (view.length < 8u) {
        return EIP_ERR_PROTOCOL;
    }
    cpf = view.body + 6;
    cpf_len = (uint16_t)(view.length - 6u);
    rc = eip_cpf_find(cpf, cpf_len, EIP_ITEM_UNCONNECTED, &unconnected, &unconnected_len);
    if (rc != EIP_OK) {
        return rc;
    }
    /* One layer, routed or not. On success a router removes the Unconnected Send layer, so the
     * target's own reply arrives here. A router that cannot deliver answers for itself with
     * service 0xD2 and a failing status. Do not tell the cases apart by service code: 0xD2 is
     * also the success reply of Read Tag Fragmented (0x52). */
    {
        uint8_t rsp_service = 0;
        uint8_t rsp_status = 0;
        uint16_t rsp_ext = 0;
        const uint8_t *rsp_data = 0;
        uint16_t rsp_data_len = 0;

        rc = eip_parse_cip_rsp(unconnected, unconnected_len, &rsp_service, &rsp_status, &rsp_ext,
                               &rsp_data, &rsp_data_len);
        if (rc != EIP_OK) {
            return rc;
        }
        if ((rsp_service & 0x80u) == 0) {
            return EIP_ERR_PROTOCOL;
        }
        RememberCip(session, rsp_status, rsp_ext, 0, 0);
        /* Reply data is copied for any status, not only success. A failing status beats data too
         * big for response, so a router's failure is not hidden by its remaining-path bytes. */
        if (response_len != 0) {
            if (response == 0) {
                return EIP_ERR_ARG;
            }
            if (rsp_data_len > response_max) {
                *response_len = 0;
                return (rsp_status != 0 && rsp_status != EIP_CIP_PARTIAL) ? EIP_ERR_CIP
                                                                          : EIP_ERR_TOO_BIG;
            }
            if (rsp_data_len != 0) {
                memcpy(response, rsp_data, rsp_data_len);
            }
            *response_len = rsp_data_len;
        }
        if (rr_body != 0) {
            *rr_body = view.body;
        }
        if (rr_len != 0) {
            *rr_len = view.length;
        }
        if (rsp_status != 0 && rsp_status != EIP_CIP_PARTIAL) {
            return EIP_ERR_CIP;
        }
        return EIP_OK;
    }
}

int eip_send_cip_ex(EipSession *session, uint8_t service, const uint8_t *path, uint16_t path_len,
                    const uint8_t *request, uint16_t request_len, const uint8_t *extra,
                    uint16_t extra_len, uint16_t extra_items, uint8_t *response,
                    uint16_t response_max, uint16_t *response_len, const uint8_t **rr_body,
                    uint16_t *rr_len)
{
    uint8_t built[480];
    uint16_t built_len = 0;
    int rc = eip_build_cip_req(built, sizeof built, service, path, path_len, request, request_len,
                               &built_len);
    if (rc != EIP_OK) {
        return rc;
    }
    return eip_send_cip_message(session, built, built_len, extra, extra_len, extra_items, response,
                                response_max, response_len, rr_body, rr_len);
}

/* ---- symbols ---- */

/* Walk the symbol object one page at a time, at most 256 pages, into symbols. Each reply goes to
 * session->scratch. next_instance is 16 bits because the request path is: a last instance of
 * 0xFFFF or above truncates and the walk restarts low, where duplicates are dropped.
 *
 * Returns EIP_OK when the walk ended by itself, EIP_ERR_TOO_BIG when the array filled or the page
 * limit hit with the walk still going, and the failing code otherwise. An array that fills on the
 * final symbol also returns EIP_ERR_TOO_BIG, as there was no way to know it was the last. */
int eip_list_symbols(EipSession *session, EipSymbol *symbols, uint16_t *symbols_len)
{
    uint16_t cap;
    uint16_t count = 0;
    uint16_t next_instance = 0;
    int page = 0;
    int more = 0;
    int rc = EIP_OK;

    if (session == 0 || symbols == 0 || symbols_len == 0 || *symbols_len == 0) {
        return EIP_ERR_ARG;
    }
    if (!SessionOpen(session)) {
        return EIP_ERR_STATE;
    }
    cap = *symbols_len;
    *symbols_len = 0;

    /* A Micro800 answers success on a full page and expects the next call
     * to start at the following instance. Partial (0x06) means the same. */
    for (page = 0; page < 256 && count < cap; page++) {
        uint8_t request[32];
        uint16_t request_len = 0;
        uint16_t page_len = 0;
        uint16_t parsed = 0;
        uint32_t last_instance = next_instance;
        uint8_t status;

        rc = eip_build_symbol_list(request, sizeof request, &request_len, next_instance);
        if (rc != EIP_OK) {
            break;
        }
        rc = eip_send_cip_message(session, request, request_len, 0, 0, 0, session->scratch, EIP_MAX_PACKET,
                                  &page_len, 0, 0);
        status = session->cip_status;
        if (rc != EIP_OK) {
            /* Asking past the last instance is how the walk ends on some
             * controllers. Keep the symbols already stored. */
            if (rc == EIP_ERR_CIP && count > 0) {
                rc = EIP_OK;
                more = 0;
            }
            break;
        }
        rc = eip_parse_symbol_page(session->scratch, page_len, symbols, cap, &count, &parsed, &last_instance);
        if (rc != EIP_OK) {
            break;
        }
        if (status == EIP_CIP_PARTIAL && parsed > 0) {
            more = 1;
            next_instance = (uint16_t)(last_instance + 1u);
            continue;
        }
        /* The instance test stops a device that repeats a page from looping here. */
        if (status == 0 && parsed > 0 && last_instance >= next_instance) {
            more = 1;
            next_instance = (uint16_t)(last_instance + 1u);
            continue;
        }
        more = 0;
        if (status == 0) {
            rc = EIP_OK;
            break;
        }
        rc = (count > 0) ? EIP_OK : EIP_ERR_CIP;
        break;
    }

    *symbols_len = count;
    if (rc == EIP_OK && more && (count >= cap || page >= 256)) {
        return EIP_ERR_TOO_BIG;
    }
    return rc;
}

/* ---- generic CIP and attributes ---- */

/* The status outputs are copied from the session on every return, so after a failure with no CIP
 * reply they hold the previous exchange's status and *response_len is unchanged. Check the return
 * code first. A null response_len means no reply data is wanted. */
int eip_cip(EipSession *session, uint8_t service, const uint8_t *path, uint16_t path_len,
            const uint8_t *request, uint16_t request_len, uint8_t *response, uint16_t *response_len,
            uint8_t *cip_status, uint16_t *extended_status)
{
    uint16_t capacity = (response_len == 0) ? 0 : *response_len;
    int rc = eip_send_cip_ex(session, service, path, path_len, request, request_len, 0, 0, 0, response,
                             capacity, response_len, 0, 0);
    if (cip_status != 0 && session != 0) {
        *cip_status = session->cip_status;
    }
    if (extended_status != 0 && session != 0) {
        *extended_status = session->cip_extended;
    }
    return rc;
}

/* Get_Attribute_Single on class, instance, attribute. path[12] holds all three in 16-bit form. */
int eip_get_attribute(EipSession *session, uint16_t class_code, uint16_t instance, uint16_t attribute,
                      uint8_t *data, uint16_t *data_len, uint8_t *cip_status)
{
    uint8_t path[12];
    uint16_t used = 0;
    int rc;

    if (data == 0 || data_len == 0) {
        return EIP_ERR_ARG;
    }
    rc = eip_put_logical(path, sizeof path, &used, 0x20, class_code);
    if (rc != EIP_OK) {
        return rc;
    }
    rc = eip_put_logical(path, sizeof path, &used, 0x24, instance);
    if (rc != EIP_OK) {
        return rc;
    }
    rc = eip_put_logical(path, sizeof path, &used, 0x30, attribute);
    if (rc != EIP_OK) {
        return rc;
    }
    return eip_cip(session, EIP_CIP_GET_ATTRIBUTE, path, used, 0, 0, data, data_len, cip_status, 0);
}

int eip_set_attribute(EipSession *session, uint16_t class_code, uint16_t instance, uint16_t attribute,
                      const uint8_t *data, uint16_t data_len, uint8_t *cip_status)
{
    uint8_t path[12];
    uint16_t used = 0;
    int rc;

    if (data == 0 || data_len == 0) {
        return EIP_ERR_ARG;
    }
    rc = eip_put_logical(path, sizeof path, &used, 0x20, class_code);
    if (rc != EIP_OK) {
        return rc;
    }
    rc = eip_put_logical(path, sizeof path, &used, 0x24, instance);
    if (rc != EIP_OK) {
        return rc;
    }
    rc = eip_put_logical(path, sizeof path, &used, 0x30, attribute);
    if (rc != EIP_OK) {
        return rc;
    }
    return eip_cip(session, EIP_CIP_SET_ATTRIBUTE, path, used, data, data_len, 0, 0, cip_status, 0);
}

/* ---- tags ---- */

/* 128 is the size of the callers' path buffers. eip_tag_write's chunk sizing relies on it. */
static int BuildTagPath(const char *tag, uint8_t *path, uint16_t *path_len)
{
    return eip_build_tag_path(tag, path, 128, path_len);
}

/* Read Tag (0x4C) sends the element count, and a reply is a 16-bit type, then data. Status 0x06
 * means more follows, so the read goes on with Read Tag Fragmented (0x52), which adds a 32-bit
 * byte offset, until a reply has status 0. Fragment replies repeat the type prefix.
 *
 * Bytes are copied as they arrive, with no decoding by type. Exactly two bytes are removed from
 * each reply: a longer type prefix stays in data and counts toward the offset. *data_len is only
 * updated on success, and data may be partly written on failure. */
int eip_tag_read(EipSession *session, const char *tag, uint16_t elements, uint8_t *data,
                 uint16_t *data_len, uint16_t *cip_type, uint8_t *cip_status)
{
    uint8_t path[128];
    uint16_t path_len = 0;
    uint16_t capacity;
    uint16_t stored = 0;
    uint32_t offset = 0;
    uint16_t type = 0;
    uint8_t service = EIP_CIP_READ_TAG;
    int rc;

    if (data == 0 || data_len == 0 || elements == 0) {
        return EIP_ERR_ARG;
    }
    rc = BuildTagPath(tag, path, &path_len);
    if (rc != EIP_OK) {
        return rc;
    }
    capacity = *data_len;
    for (;;) {
        uint8_t request[6];
        uint8_t response[512];
        uint16_t request_len;
        uint16_t response_len = sizeof response;
        uint16_t payload_len;
        uint8_t status;

        eip_store_le16(request, elements);
        if (service == EIP_CIP_READ_TAG) {
            request_len = 2;
        } else {
            eip_store_le32(request + 2, offset);
            request_len = 6;
        }
        rc = eip_cip(session, service, path, path_len, request, request_len, response, &response_len,
                     cip_status, 0);
        status = (session == 0) ? 0 : session->cip_status;
        if (rc != EIP_OK) {
            return rc;
        }
        if (status != 0 && status != EIP_CIP_PARTIAL) {
            return EIP_ERR_CIP;
        }
        if (response_len < 2u) {
            return EIP_ERR_PROTOCOL;
        }
        if (offset == 0) {
            type = eip_load_le16(response);
        }
        payload_len = (uint16_t)(response_len - 2u);
        if ((uint32_t)stored + payload_len > capacity) {
            return EIP_ERR_TOO_BIG;
        }
        if (payload_len != 0) {
            memcpy(data + stored, response + 2, payload_len);
        }
        /* "More follows" with no data would ask for the same offset for ever. */
        if (status == EIP_CIP_PARTIAL && payload_len == 0) {
            return EIP_ERR_PROTOCOL;
        }
        stored = (uint16_t)(stored + payload_len);
        offset += payload_len;
        if (status == 0) {
            *data_len = stored;
            if (cip_type != 0) {
                *cip_type = type;
            }
            return EIP_OK;
        }
        service = EIP_CIP_READ_FRAG;
        if (offset > 1000000u) {
            return EIP_ERR_PROTOCOL;
        }
    }
}

/* Write Tag (0x4D) data is the 16-bit type, the element count, then the bytes. Data over one chunk
 * goes as Write Tag Fragmented (0x53) with a 32-bit byte offset after the count, and every
 * fragment repeats the type and the total element count. data must already be in wire order.
 *
 * Fragments go one at a time and the first failure stops the write, so a failed fragmented
 * write can leave the tag partly written. Chunks are cut by bytes, not on element boundaries.
 * data_len 0 still sends one Write Tag with no data. */
int eip_tag_write(EipSession *session, const char *tag, uint16_t cip_type, uint16_t elements,
                  const uint8_t *data, uint16_t data_len, uint8_t *cip_status)
{
    uint8_t path[128];
    uint16_t path_len = 0;
    uint32_t offset = 0;
    uint16_t chunk;
    int fragmented;
    int rc;

    if (elements == 0 || (data_len != 0 && data == 0)) {
        return EIP_ERR_ARG;
    }
    rc = BuildTagPath(tag, path, &path_len);
    if (rc != EIP_OK) {
        return rc;
    }
    /* Size a chunk so the CIP message, 2 + path + 8 + chunk, totals 470: under the 480-byte limit
     * of eip_send_cip_message. With a path of at most 128 bytes the check below cannot fire. */
    chunk = (uint16_t)(470u - 2u - path_len);
    if (chunk < 16u) {
        return EIP_ERR_TOO_BIG;
    }
    chunk = (uint16_t)(chunk - 8u);
    if (chunk > 400u) {
        chunk = 400u;
    }
    fragmented = data_len > chunk;
    while (offset < data_len || (data_len == 0 && offset == 0)) {
        uint8_t request[8 + 400];
        uint16_t piece = (uint16_t)(data_len - offset);
        uint16_t request_len;
        uint8_t service;

        if (piece > chunk) {
            piece = chunk;
        }
        eip_store_le16(request, cip_type);
        eip_store_le16(request + 2, elements);
        if (!fragmented) {
            service = EIP_CIP_WRITE_TAG;
            if (piece != 0) {
                memcpy(request + 4, data + offset, piece);
            }
            request_len = (uint16_t)(4u + piece);
        } else {
            service = EIP_CIP_WRITE_FRAG;
            eip_store_le32(request + 4, offset);
            if (piece != 0) {
                memcpy(request + 8, data + offset, piece);
            }
            request_len = (uint16_t)(8u + piece);
        }
        rc = eip_cip(session, service, path, path_len, request, request_len, 0, 0, cip_status, 0);
        if (rc != EIP_OK) {
            return rc;
        }
        if (data_len == 0) {
            break;
        }
        offset += piece;
        if (!fragmented) {
            break;
        }
    }
    return EIP_OK;
}
