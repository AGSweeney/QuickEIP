/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* Internal helpers shared by the scanner source files. Not public API. */
#ifndef EIP_INTERNAL_H_
#define EIP_INTERNAL_H_

#include "eip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* cip is a complete CIP message (service, path size, path, data), at most 480 bytes. It is sent in
 * SendRRData, wrapped for the session's slot when slot is 0 or higher.
 * extra is extra_items encoded CPF items sent after the CIP item (0, 0, 0 for none).
 * response_len 0 discards the reply data. Otherwise response must be set, and *response_len gets
 * the size of the reply data, without the CIP reply header.
 * rr_body is the whole SendRRData reply body, there for items beyond the CIP reply.
 * rr_body stays valid until the next call on this session.
 * Returns EIP_OK for general status 0 and for 0x06 (partial), so callers that page read
 * session->cip_status to tell them apart. Any other status is EIP_ERR_CIP. */
int eip_send_cip_message(EipSession *session, const uint8_t *cip, uint16_t cip_len,
                         const uint8_t *extra, uint16_t extra_len, uint16_t extra_items,
                         uint8_t *response, uint16_t response_max, uint16_t *response_len,
                         const uint8_t **rr_body, uint16_t *rr_len);

/* eip_send_cip_message that first builds the CIP message from service, path and request. */
int eip_send_cip_ex(EipSession *session, uint8_t service, const uint8_t *path, uint16_t path_len,
                    const uint8_t *request, uint16_t request_len, const uint8_t *extra,
                    uint16_t extra_len, uint16_t extra_items, uint8_t *response,
                    uint16_t response_max, uint16_t *response_len, const uint8_t **rr_body,
                    uint16_t *rr_len);

/* Session timeout in ms, never under 100. This feeds timeout fields sent on the wire. The socket
 * reads in eip_session.c use session->timeout_ms directly. */
uint32_t eip_timeout_ms(const EipSession *session);

#ifdef __cplusplus
}
#endif

#endif /* EIP_INTERNAL_H_ */
