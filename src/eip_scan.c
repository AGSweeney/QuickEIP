/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* ListIdentity: unicast query and broadcast scan. Does not need a session.
 * Requests go by UDP to port 44818 (EIP_TCP_PORT, the same number). eip_codec.c parses replies. */
#include "eip_codec.h"
#include "eip_hal.h"

#include <string.h>

/* Each interface adds its own address and its directed broadcast, and the global broadcast adds
 * one more: 4 x 2 + 1 = 9 targets at most. */
enum { kMaxTargets = 10, kMaxInterfaces = 4 };

static int IpIsZero(const uint8_t ip[4])
{
    return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

/* Sends one request, with no resend. Datagrams from other addresses or under 24 bytes are skipped.
 * The first other one decides the result: a malformed reply is EIP_ERR_PROTOCOL, not a retry. */
int eip_list_identity(const char *ip, EipIdentity *identity, uint32_t timeout_ms)
{
    uint8_t remote[4];
    uint8_t context[8];
    uint8_t packet[512];
    uint16_t packet_len = 0;
    uint32_t start;
    eip_sock_t sock;
    EipEncapView view;
    int rc;

    if (ip == 0 || identity == 0) {
        return EIP_ERR_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = 2000u;
    }
    if (eip_parse_ipv4(ip, remote) != EIP_OK || IpIsZero(remote)) {
        return EIP_ERR_ARG;
    }
    sock = eip_hal_udp_open(0, 0);
    if (sock < 0) {
        return EIP_ERR_IO;
    }
    /* An arbitrary value. Replies are matched by source address, not by context. */
    memset(context, 0, sizeof context);
    context[0] = 0x11;
    rc = eip_build_encap(packet, sizeof packet, EIP_CMD_LIST_IDENTITY, 0, context, 0, 0, &packet_len);
    if (rc == EIP_OK && eip_hal_udp_sendto(sock, remote, EIP_TCP_PORT, packet, packet_len) != EIP_OK) {
        rc = EIP_ERR_IO;
    }
    start = eip_hal_millis();
    while (rc == EIP_OK) {
        uint32_t elapsed = eip_hal_millis() - start;
        uint8_t from[4];
        int n;

        if (elapsed >= timeout_ms) {
            rc = EIP_ERR_TIMEOUT;
            break;
        }
        n = eip_hal_udp_recvfrom(sock, packet, sizeof packet, from, 0, timeout_ms - elapsed);
        if (n < 0) {
            rc = EIP_ERR_IO;
            break;
        }
        if (n == 0) {
            continue;
        }
        if (memcmp(from, remote, 4) != 0 || n < 24) {
            continue; /* someone else's datagram */
        }
        if (eip_parse_encap(packet, (uint16_t)n, &view) != EIP_OK ||
            view.command != EIP_CMD_LIST_IDENTITY || view.status != 0) {
            rc = EIP_ERR_PROTOCOL;
            break;
        }
        rc = eip_parse_identity(view.body, view.length, identity);
        break;
    }
    eip_hal_udp_close(sock);
    return rc;
}

static void AddTarget(uint8_t targets[][4], int *count, const uint8_t candidate[4])
{
    int i;

    if (*count >= kMaxTargets || IpIsZero(candidate)) {
        return;
    }
    for (i = 0; i < *count; i++) {
        if (memcmp(targets[i], candidate, 4) == 0) {
            return;
        }
    }
    memcpy(targets[(*count)++], candidate, 4);
}

/* Fill targets and return the count. Stops at the first interface index the port rejects and skips
 * interfaces with no address yet. The global broadcast is always last, so the count is at least 1
 * even with no usable interface. */
static int BroadcastTargets(uint8_t targets[][4])
{
    static const uint8_t global[4] = {255, 255, 255, 255};
    int count = 0;
    uint8_t index;

    for (index = 0; index < kMaxInterfaces; index++) {
        EipNetifInfo info;
        uint8_t directed[4];
        int b;

        memset(&info, 0, sizeof info);
        if (eip_hal_netif_get(index, &info) != EIP_OK) {
            break;
        }
        if (IpIsZero(info.ip) || IpIsZero(info.netmask)) {
            continue;
        }
        /* Some stacks (NetBurner) do not deliver a broadcast back to the
         * sender, so a unicast to our own address finds a local adapter. */
        AddTarget(targets, &count, info.ip);
        for (b = 0; b < 4; b++) {
            directed[b] = (uint8_t)((info.ip[b] & info.netmask[b]) | (uint8_t)~info.netmask[b]);
        }
        AddTarget(targets, &count, directed);
    }
    AddTarget(targets, &count, global);
    return count;
}

/* Returns 1 when at least one target took the request. One failed send does not stop the rest. */
static int SendListIdentity(eip_sock_t sock, uint8_t targets[][4], int target_count,
                            const uint8_t *request, uint16_t request_len)
{
    int any = 0;
    int i;

    for (i = 0; i < target_count; i++) {
        if (eip_hal_udp_sendto(sock, targets[i], EIP_TCP_PORT, request, request_len) == EIP_OK) {
            any = 1;
        }
    }
    return any;
}

/* One socket sends to every target and collects the replies, one device per source address. The
 * loop polls in steps of at most 100 ms and ends at the first of: timeout_ms, the array filling,
 * or 500 ms of quiet after a reply. A full array is not an error, and further devices go unseen.
 * Requests repeat every second, which in practice matters only until the first reply, since the
 * quiet rule then ends the scan. A receive error counts as no data and the scan runs on. */
int eip_scan_devices(EipDevice *devices, uint16_t *devices_len, uint32_t timeout_ms)
{
    uint8_t targets[kMaxTargets][4];
    uint8_t request[32];
    uint8_t packet[512];
    uint16_t request_len = 0;
    uint8_t context[8];
    int target_count;
    eip_sock_t sock;
    uint16_t found = 0;
    uint16_t cap;
    uint32_t quiet = 500u;
    uint32_t scan_start;
    uint32_t last_send;
    uint32_t last_reply = 0;
    int have_reply = 0;
    int rc;

    if (devices == 0 || devices_len == 0 || *devices_len == 0) {
        return EIP_ERR_ARG;
    }
    cap = *devices_len;
    *devices_len = 0;
    if (timeout_ms == 0) {
        timeout_ms = 3000u;
    }

    sock = eip_hal_udp_open(EIP_SCAN_PORT, EIP_UDP_BROADCAST);
    if (sock < 0) {
        sock = eip_hal_udp_open(0, EIP_UDP_BROADCAST); /* 44819 busy: any port works */
    }
    if (sock < 0) {
        return EIP_ERR_IO;
    }
    memset(context, 0, sizeof context);
    rc = eip_build_encap(request, sizeof request, EIP_CMD_LIST_IDENTITY, 0, context, 0, 0, &request_len);
    target_count = BroadcastTargets(targets);
    if (rc != EIP_OK || target_count <= 0 ||
        !SendListIdentity(sock, targets, target_count, request, request_len)) {
        eip_hal_udp_close(sock);
        return (rc != EIP_OK) ? rc : EIP_ERR_IO;
    }

    scan_start = eip_hal_millis();
    last_send = scan_start;
    while ((eip_hal_millis() - scan_start) < timeout_ms && found < cap) {
        uint32_t now = eip_hal_millis();
        uint32_t elapsed = now - scan_start;
        uint32_t remaining = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0;
        uint32_t wait = (remaining < 100u) ? remaining : 100u;
        uint8_t from[4];
        EipEncapView view;
        EipIdentity identity;
        EipDevice *device;
        int received;
        int i;
        int duplicate = 0;

        if ((now - last_send) >= 1000u) {
            SendListIdentity(sock, targets, target_count, request, request_len);
            last_send = now;
        }
        if (have_reply && (now - last_reply) >= quiet) {
            break;
        }
        if (wait == 0) {
            break;
        }
        received = eip_hal_udp_recvfrom(sock, packet, sizeof packet, from, 0, wait);
        if (received < 0) {
            eip_hal_yield(); /* an error returns at once: don't spin until the timeout */
        }
        if (received <= 0) {
            continue;
        }
        /* Header (24), item count (2) and one item header (4) are the least a reply can hold. */
        if (received < 30) {
            continue;
        }
        /* Set before validation: any datagram of this size holds off the quiet exit, even a
         * duplicate or one that fails to parse below. */
        last_reply = eip_hal_millis();
        have_reply = 1;
        if (eip_parse_encap(packet, (uint16_t)received, &view) != EIP_OK ||
            view.command != EIP_CMD_LIST_IDENTITY || view.status != 0) {
            continue;
        }
        if (eip_parse_identity(view.body, view.length, &identity) != EIP_OK) {
            continue;
        }
        for (i = 0; i < (int)found; i++) {
            if (memcmp(devices[i].ip, from, 4) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        device = &devices[found];
        memset(device, 0, sizeof *device);
        memcpy(device->ip, from, 4);
        device->vendor_id = identity.vendor_id;
        device->device_type = identity.device_type;
        device->product_code = identity.product_code;
        device->revision_major = identity.revision_major;
        device->revision_minor = identity.revision_minor;
        device->serial_number = identity.serial_number;
        memcpy(device->name, identity.name, sizeof device->name);
        /* Classified on the name as received, before sanitizing can change it. */
        device->micro800 = (uint8_t)eip_name_is_micro800(identity.vendor_id, device->name);
        eip_sanitize_product_name(device->name);
        found++;
    }

    eip_hal_udp_close(sock);
    *devices_len = found;
    return EIP_OK;
}
