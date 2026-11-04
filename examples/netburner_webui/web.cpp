/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* A browser test page for QuickEIP on NetBurner. Application code, not part of the stack.
 *
 * Add this file and htmldata.cpp (the generated index.html, app.js and style.css) to an NNDK
 * application that calls StartHttp() and eip_adapter_start(). There is no main or makefile here.
 * The page POSTs JSON to /api/cmd, and CmdHandler at the bottom dispatches on its "cmd" member:
 *
 *   scan                    broadcast ListIdentity
 *   connect, disconnect     open or close the one scanner session
 *   symbols                 list controller tags, up to 48
 *   tag-read, tag-write     Logix tag access
 *   asm-read, asm-write     assembly data over explicit messaging
 *   io-cycle                one Class 1 open, produce, consume, close
 *   adapter, adapter-write  status and assembly data of this device's own adapter
 *
 * Replies are JSON objects with "ok", plus "error" on failure. All but the adapter commands
 * end with a "session" object so the page can show the true state.
 *
 * Threads: NNDK serves HTTP from one task, so everything here runs there, one request at a time,
 * and the statics below need no lock. Scanner calls block that task until they finish or time
 * out (a scan about 3 s, a session request up to 5 s), and no other page is served meanwhile. The
 * adapter runs in its own task; the eip_adapter_* calls are safe from any task. */
#include <predef.h>

#include "eip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <fdprintf.h>
#include <http.h>
#include <httppost.h>
#include <iosys.h>
#include <json_lexer.h>
#include <nbrtos.h>

/* Scanner side: one session, one Class 1 connection, and the target the page is pointed at. */
static EipSession g_session;
static EipIo g_io;
static EipDevice g_devices[16];
static EipSymbol g_symbols[48];
static char g_ip[40];
static char g_kind[16];
static int16_t g_slot = -1;
static int g_open;
static int g_busy; /* a command is running, see CmdHandler */

/* The reply being built. g_overflow latches when it fills, and SendJson then sends an error. */
static char g_json[16384];
static int g_used;
static int g_overflow;
/* Tag and assembly data scratch, shared by the commands. */
static uint8_t g_raw[256];

/* The "kind" strings the page uses for eip_device_kind() values. */
static const char *KindName(int kind)
{
    if (kind == EIP_KIND_MICRO800) {
        return "micro800";
    }
    if (kind == EIP_KIND_COMPACTLOGIX) {
        return "compactlogix";
    }
    return "assembly";
}

static int KindFromName(const char *name)
{
    if (name != 0 && strcmp(name, "micro800") == 0) {
        return EIP_KIND_MICRO800;
    }
    if (name != 0 && strcmp(name, "compactlogix") == 0) {
        return EIP_KIND_COMPACTLOGIX;
    }
    return EIP_KIND_ASSEMBLY;
}

/* ---- JSON reply --------------------------------------------------------- */

static void JsonReset(void)
{
    g_used = 0;
    g_overflow = 0;
    g_json[0] = '\0';
}

static void JsonAdd(const char *text)
{
    size_t n;
    if (text == 0 || g_overflow) {
        return;
    }
    n = strlen(text);
    if ((size_t)g_used + n >= sizeof g_json) {
        g_overflow = 1;
        return;
    }
    memcpy(g_json + g_used, text, n);
    g_used += (int)n;
    g_json[g_used] = '\0';
}

/* One formatted piece, cut silently at 191 characters and not escaped: send text that can come
 * from outside through JsonEscaped. */
static void JsonFmt(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = '\0';
    JsonAdd(buf);
}

/* A quoted JSON string. Escapes ", \ and control characters; other bytes pass through. */
static void JsonEscaped(const char *text)
{
    JsonAdd("\"");
    if (text == 0) {
        JsonAdd("\"");
        return;
    }
    for (; *text != '\0'; text++) {
        unsigned char c = (unsigned char)*text;
        char tmp[8];
        if (c == '"' || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            tmp[2] = '\0';
            JsonAdd(tmp);
        } else if (c < 0x20u) {
            sniprintf(tmp, sizeof tmp, "\\u%04x", c);
            JsonAdd(tmp);
        } else {
            tmp[0] = (char)c;
            tmp[1] = '\0';
            JsonAdd(tmp);
        }
    }
    JsonAdd("\"");
}

/* Bytes as a quoted string, "AA BB CC". */
static void JsonHex(const uint8_t *data, int len)
{
    int i;
    JsonAdd("\"");
    for (i = 0; i < len; i++) {
        char tmp[4];
        if (i != 0) {
            JsonAdd(" ");
        }
        sniprintf(tmp, sizeof tmp, "%02X", data[i]);
        JsonAdd(tmp);
    }
    JsonAdd("\"");
}

/* The "session" member. It starts with a comma, so the caller has already written a member. */
static void JsonSession(void)
{
    JsonAdd(",\"session\":{\"open\":");
    JsonAdd(g_open ? "true" : "false");
    JsonAdd(",\"ip\":");
    JsonEscaped(g_open ? g_ip : "");
    JsonAdd(",\"kind\":");
    JsonEscaped(g_open ? g_kind : "");
    JsonFmt(",\"slot\":%d}", (int)g_slot);
}

/* Writes the whole HTTP response by hand: status line, headers, body. */
static void SendJson(int sock)
{
    if (g_overflow) {
        writestring(sock, "HTTP/1.0 500 Internal Server Error\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\n\r\n");
        writestring(sock, "{\"ok\":false,\"error\":\"response too large\"}");
        return;
    }
    writestring(sock, "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\n\r\n");
    writestring(sock, g_json);
}

/* Build and send an error reply. rc 0 is a local check with no stack status to report. "cip" is
 * the session's last CIP status, which is only fresh when rc is EIP_ERR_CIP. */
static void Fail(int sock, const char *message, int rc)
{
    JsonReset();
    JsonAdd("{\"ok\":false,\"error\":");
    JsonEscaped(message);
    if (rc != 0) {
        JsonFmt(",\"code\":%d,\"status\":", rc);
        JsonEscaped(eip_status_text(rc));
        JsonFmt(",\"cip\":\"0x%02X\"", eip_last_cip_status(&g_session));
    }
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

static int HexNibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Hex text to bytes. Whitespace, ':' and ',' are skipped wherever they appear. Returns -1 for any
 * other character, an odd digit count, or more than cap bytes. */
static int ParseHex(const char *text, uint8_t *out, int cap, int *out_len)
{
    int n = 0;
    int hi = -1;
    if (text == 0) {
        return -1;
    }
    for (; *text != '\0'; text++) {
        int v;
        if (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n' || *text == ':' || *text == ',') {
            continue;
        }
        v = HexNibble(*text);
        if (v < 0) {
            return -1;
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) {
                return -1;
            }
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) {
        return -1;
    }
    *out_len = n;
    return 0;
}

/* ---- the scanner target ------------------------------------------------- */

/* 1 when the session is usable. Otherwise sends the error reply and returns 0. */
static int NeedOpen(int sock)
{
    if (g_open && eip_session_is_open(&g_session)) {
        return 1;
    }
    Fail(sock, "not connected", 0);
    return 0;
}

static void CloseTarget(void)
{
    if (g_io.is_open) {
        eip_io_close(&g_session, &g_io);
    }
    memset(&g_io, 0, sizeof g_io);
    if (eip_session_is_open(&g_session)) {
        eip_session_close(&g_session);
    }
    g_open = 0;
    g_ip[0] = '\0';
    g_kind[0] = '\0';
    g_slot = -1;
}

/* CloseTarget clears g_ip, so the address has to be copied back after it.
 * Replaces any open target. Returns 1, or sends the error reply and returns 0. kind is only kept
 * for the page: the session itself depends on the slot. */
static int OpenTarget(int sock, const char *ip, int kind, int16_t slot)
{
    EipOpenConfig config;
    char saved[40];
    int rc;
    uint8_t raw[8];
    uint16_t raw_len;
    uint8_t cip_status = 0;

    if (ip == 0 || ip[0] == '\0') {
        Fail(sock, "ip is required", 0);
        return 0;
    }
    if (slot < -1 || slot > 255) {
        Fail(sock, "slot must be -1 through 255", 0);
        return 0;
    }
    strncpy(saved, ip, sizeof saved - 1);
    saved[sizeof saved - 1] = '\0';
    CloseTarget();
    strncpy(g_ip, saved, sizeof g_ip - 1);
    g_ip[sizeof g_ip - 1] = '\0';

    /* The vendor id and serial identify this originator in a Forward_Open. Vendor 1 is Rockwell's:
     * use your own. The io-cycle command replaces the vendor id per request. */
    memset(&config, 0, sizeof config);
    config.ip = g_ip;
    config.slot = slot;
    config.vendor_id = 1;
    config.serial_number = 0x10001;
    config.timeout_ms = 5000;
    rc = eip_session_open(&g_session, &config);
    if (rc != EIP_OK) {
        g_ip[0] = '\0';
        Fail(sock, "connect failed", rc);
        return 0;
    }
    g_open = 1;
    g_slot = slot;
    strncpy(g_kind, KindName(kind), sizeof g_kind - 1);
    g_kind[sizeof g_kind - 1] = '\0';

    /* One read of the Identity vendor id. The result is ignored: the connect stands either way. */
    raw_len = sizeof raw;
    rc = eip_get_attribute(&g_session, 1, 1, 1, raw, &raw_len, &cip_status);
    (void)rc;
    (void)cip_status;
    return 1;
}

/* Compares address and slot only, so a new kind on the same target keeps the old g_kind. */
static int SameTarget(const char *ip, int16_t slot)
{
    return g_open && eip_session_is_open(&g_session) && strcmp(g_ip, ip) == 0 &&
           g_slot == slot;
}

/* ---- request fields ----------------------------------------------------- */

/* String member into out, cut to fit. 0, with out empty, when missing, not a string or empty. */
static int CopyField(ParsedJsonDataSet &json, const char *name, char *out, int out_len)
{
    JsonRef field = json(name);
    if (!field.Valid() || !field.IsString()) {
        out[0] = '\0';
        return 0;
    }
    if (field.CopyCurrentString(out, out_len) <= 0) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* Number member. Missing or not a number gives fallback and returns 0. The value goes through
 * int, so one above INT_MAX does not survive. */
static int NumberField(ParsedJsonDataSet &json, const char *name, long *out, long fallback)
{
    JsonRef field = json(name);
    if (!field.Valid() || !field.IsNumber()) {
        *out = fallback;
        return 0;
    }
    *out = (long)(int)field;
    return 1;
}

/* ---- scanner commands --------------------------------------------------- */

/* scan: up to 16 devices, each with its eip_device_kind(). Blocks about 3 s. */
static void CmdScan(int sock)
{
    uint16_t count = (uint16_t)(sizeof g_devices / sizeof g_devices[0]);
    uint16_t i;
    int rc = eip_scan_devices(g_devices, &count, 0);
    if (rc != EIP_OK) {
        Fail(sock, "scan failed", rc);
        return;
    }
    JsonReset();
    JsonAdd("{\"ok\":true,\"summary\":");
    JsonFmt("\"found %u\"", count);
    JsonFmt(",\"count\":%u,\"devices\":[", count);
    for (i = 0; i < count; i++) {
        int kind = eip_device_kind(g_devices[i].vendor_id, g_devices[i].name);
        if (i != 0) {
            JsonAdd(",");
        }
        JsonAdd("{\"ip\":");
        JsonFmt("\"%u.%u.%u.%u\"", g_devices[i].ip[0], g_devices[i].ip[1], g_devices[i].ip[2],
                g_devices[i].ip[3]);
        JsonAdd(",\"name\":");
        JsonEscaped(g_devices[i].name);
        JsonAdd(",\"kind\":");
        JsonEscaped(KindName(kind));
        JsonFmt(",\"vendor\":%u,\"type\":%u,\"product\":%u,\"major\":%u,\"minor\":%u,\"serial\":%lu}",
                g_devices[i].vendor_id, g_devices[i].device_type, g_devices[i].product_code,
                g_devices[i].revision_major, g_devices[i].revision_minor,
                (unsigned long)g_devices[i].serial_number);
    }
    JsonAdd("]");
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

/* ip, kind and slot from a request. Returns 0 when ip is missing. kind defaults to assembly. The
 * slot is used for kind compactlogix only; every other kind is forced to -1 (direct). */
static int TargetFromJson(ParsedJsonDataSet &json, char *ip, int ip_len, int *kind, int16_t *slot)
{
    char kind_name[16];
    long slot_value = -1;

    if (!CopyField(json, "ip", ip, ip_len)) {
        return 0;
    }
    if (!CopyField(json, "kind", kind_name, (int)sizeof kind_name)) {
        strcpy(kind_name, "assembly");
    }
    *kind = KindFromName(kind_name);
    NumberField(json, "slot", &slot_value, -1);
    if (*kind != EIP_KIND_COMPACTLOGIX || slot_value < -1 || slot_value > 255) {
        slot_value = -1;
    }
    *slot = (int16_t)slot_value;
    return 1;
}

static void CmdConnect(int sock, ParsedJsonDataSet &json)
{
    char ip[40];
    int kind = EIP_KIND_ASSEMBLY;
    int16_t slot = -1;

    if (!TargetFromJson(json, ip, (int)sizeof ip, &kind, &slot)) {
        Fail(sock, "ip is required", 0);
        return;
    }
    if (!OpenTarget(sock, ip, kind, slot)) {
        return;
    }
    JsonReset();
    JsonAdd("{\"ok\":true,\"summary\":");
    JsonEscaped("connected");
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

/* symbols: an "ip" in the request switches target first, otherwise the open session is used.
 * "ok" is true whenever some symbols came back, even with an error; "truncated" means g_symbols
 * filled before the list ended. */
static void CmdSymbols(int sock, ParsedJsonDataSet &json)
{
    char ip[40];
    int kind = EIP_KIND_ASSEMBLY;
    int16_t slot = -1;
    uint16_t count = (uint16_t)(sizeof g_symbols / sizeof g_symbols[0]);
    uint16_t i;
    int rc;

    if (TargetFromJson(json, ip, (int)sizeof ip, &kind, &slot)) {
        if (!SameTarget(ip, slot) && !OpenTarget(sock, ip, kind, slot)) {
            return;
        }
    } else if (!NeedOpen(sock)) {
        return;
    }
    rc = eip_list_symbols(&g_session, g_symbols, &count);
    JsonReset();
    JsonAdd("{\"ok\":");
    JsonAdd((rc == EIP_OK || count > 0) ? "true" : "false");
    JsonAdd(",\"summary\":");
    JsonFmt("\"%u symbols\"", count);
    if (rc != EIP_OK) {
        JsonAdd(",\"error\":");
        JsonEscaped(eip_status_text(rc));
        JsonFmt(",\"code\":%d", rc);
    }
    JsonFmt(",\"truncated\":%s,\"symbols\":[", (rc == EIP_ERR_TOO_BIG) ? "true" : "false");
    for (i = 0; i < count; i++) {
        if (i != 0) {
            JsonAdd(",");
        }
        JsonAdd("{\"name\":");
        JsonEscaped(g_symbols[i].name);
        JsonFmt(",\"type\":%u,\"size\":%u,\"dim0\":%lu,\"dim1\":%lu,\"dim2\":%lu,\"array\":%s}",
                g_symbols[i].symbol_type, g_symbols[i].element_size, (unsigned long)g_symbols[i].dim0,
                (unsigned long)g_symbols[i].dim1, (unsigned long)g_symbols[i].dim2,
                g_symbols[i].is_array ? "true" : "false");
    }
    JsonAdd("]");
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

/* tag-read: 1 to 32 elements, returned as the CIP type and raw hex. */
static void CmdTagRead(int sock, ParsedJsonDataSet &json)
{
    char tag[96];
    long elements = 1;
    uint16_t raw_len = sizeof g_raw;
    uint16_t cip_type = 0;
    uint8_t cip_status = 0;
    int rc;

    if (!NeedOpen(sock)) {
        return;
    }
    if (!CopyField(json, "tag", tag, (int)sizeof tag)) {
        Fail(sock, "tag is required", 0);
        return;
    }
    NumberField(json, "elements", &elements, 1);
    if (elements < 1 || elements > 32) {
        Fail(sock, "elements must be 1 through 32", 0);
        return;
    }
    rc = eip_tag_read(&g_session, tag, (uint16_t)elements, g_raw, &raw_len, &cip_type, &cip_status);
    if (rc != EIP_OK) {
        Fail(sock, "tag read failed", rc);
        return;
    }
    JsonReset();
    JsonAdd("{\"ok\":true,\"tag\":");
    JsonEscaped(tag);
    JsonFmt(",\"type\":%u,\"bytes\":%u,\"hex\":", cip_type, raw_len);
    JsonHex(g_raw, raw_len);
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

/* Encode an integer for tag-write into g_raw. -1 for a type it does not know. REAL returns -1 as
 * well, but CmdTagWrite handles REAL itself and never gets here with it. */
static int StoreNumber(const char *type_name, long number, uint16_t *cip_type, int *len)
{
    if (strcmp(type_name, "BOOL") == 0) {
        g_raw[0] = (number != 0) ? 1u : 0u;
        *cip_type = EIP_BOOL;
        *len = 1;
        return 0;
    }
    if (strcmp(type_name, "SINT") == 0 || strcmp(type_name, "USINT") == 0) {
        g_raw[0] = (uint8_t)number;
        *cip_type = (strcmp(type_name, "USINT") == 0) ? (uint16_t)EIP_USINT : (uint16_t)EIP_SINT;
        *len = 1;
        return 0;
    }
    if (strcmp(type_name, "INT") == 0 || strcmp(type_name, "UINT") == 0) {
        eip_store_le16(g_raw, (uint16_t)number);
        *cip_type = (strcmp(type_name, "UINT") == 0) ? (uint16_t)EIP_UINT : (uint16_t)EIP_INT;
        *len = 2;
        return 0;
    }
    if (strcmp(type_name, "DINT") == 0 || strcmp(type_name, "UDINT") == 0) {
        eip_store_le32(g_raw, (uint32_t)number);
        *cip_type = (strcmp(type_name, "UDINT") == 0) ? (uint16_t)EIP_UDINT : (uint16_t)EIP_DINT;
        *len = 4;
        return 0;
    }
    if (strcmp(type_name, "REAL") == 0) {
        return -1;
    }
    return -1;
}

/* tag-write: one element. "type" selects the encoding: "hex" takes raw bytes from "hex", REAL and
 * the integer types take "number". */
static void CmdTagWrite(int sock, ParsedJsonDataSet &json)
{
    char tag[96];
    char type_name[16];
    char hex[256];
    uint16_t cip_type = 0;
    uint8_t cip_status = 0;
    int len = 0;
    int rc;
    long number = 0;

    if (!NeedOpen(sock)) {
        return;
    }
    if (!CopyField(json, "tag", tag, (int)sizeof tag)) {
        Fail(sock, "tag is required", 0);
        return;
    }
    if (!CopyField(json, "type", type_name, (int)sizeof type_name)) {
        Fail(sock, "type is required", 0);
        return;
    }
    if (strcmp(type_name, "hex") == 0) {
        if (!CopyField(json, "hex", hex, (int)sizeof hex) || ParseHex(hex, g_raw, (int)sizeof g_raw, &len) != 0 ||
            len == 0) {
            Fail(sock, "hex value is invalid", 0);
            return;
        }
        /* The CIP type sent comes from the length alone. */
        if (len == 1) {
            cip_type = EIP_SINT;
        } else if (len == 2) {
            cip_type = EIP_INT;
        } else if (len == 4) {
            cip_type = EIP_DINT;
        } else if (len == 8) {
            cip_type = EIP_LINT;
        } else {
            Fail(sock, "hex write must be 1, 2, 4, or 8 bytes", 0);
            return;
        }
    } else if (strcmp(type_name, "STRING") == 0 || strcmp(type_name, "LREAL") == 0) {
        Fail(sock, "write this type as hex", 0);
        return;
    } else if (strcmp(type_name, "REAL") == 0) {
        JsonRef field = json("number");
        float value;
        uint32_t bits;
        if (!field.Valid() || !field.IsNumber()) {
            Fail(sock, "number is required", 0);
            return;
        }
        value = (float)(double)field;
        memcpy(&bits, &value, sizeof bits);
        eip_store_le32(g_raw, bits);
        cip_type = EIP_REAL;
        len = 4;
    } else {
        if (!NumberField(json, "number", &number, 0)) {
            Fail(sock, "number is required", 0);
            return;
        }
        if (StoreNumber(type_name, number, &cip_type, &len) != 0) {
            Fail(sock, "type must be BOOL, SINT, INT, DINT, USINT, UINT, UDINT, REAL, or hex", 0);
            return;
        }
    }
    rc = eip_tag_write(&g_session, tag, cip_type, 1, g_raw, (uint16_t)len, &cip_status);
    if (rc != EIP_OK) {
        Fail(sock, "tag write failed", rc);
        return;
    }
    JsonReset();
    JsonAdd("{\"ok\":true,\"summary\":\"tag written\",\"hex\":");
    JsonHex(g_raw, len);
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

/* asm-read: class 4 attribute 3 (data). "bytes" is the read capacity, so an assembly larger than
 * it fails. Attribute 4 (size) is tried first and reported as "size" when the target answers. */
static void CmdAsmRead(int sock, ParsedJsonDataSet &json)
{
    long instance = 0;
    long bytes = 32;
    uint16_t raw_len;
    uint16_t size_len = 4;
    uint8_t size_raw[4];
    uint8_t cip_status = 0;
    int rc;
    int reported_size = -1;

    if (!NeedOpen(sock)) {
        return;
    }
    NumberField(json, "instance", &instance, 0);
    NumberField(json, "bytes", &bytes, 32);
    if (instance < 1 || instance > 65535 || bytes < 1 || bytes > (long)sizeof g_raw) {
        Fail(sock, "instance or length is out of range", 0);
        return;
    }
    rc = eip_get_attribute(&g_session, 4, (uint16_t)instance, 4, size_raw, &size_len, &cip_status);
    if (rc == EIP_OK && size_len >= 2u) {
        reported_size = (int)eip_load_le16(size_raw);
    }
    raw_len = (uint16_t)bytes;
    rc = eip_get_attribute(&g_session, 4, (uint16_t)instance, 3, g_raw, &raw_len, &cip_status);
    if (rc != EIP_OK) {
        Fail(sock, "assembly read failed", rc);
        return;
    }
    JsonReset();
    JsonFmt("{\"ok\":true,\"instance\":%ld,\"bytes\":%u", instance, raw_len);
    if (reported_size >= 0) {
        JsonFmt(",\"size\":%d", reported_size);
    }
    JsonAdd(",\"hex\":");
    JsonHex(g_raw, raw_len);
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

/* asm-write: Set_Attribute_Single on class 4 attribute 3. */
static void CmdAsmWrite(int sock, ParsedJsonDataSet &json)
{
    long instance = 0;
    char hex[600];
    int len = 0;
    uint8_t cip_status = 0;
    int rc;

    if (!NeedOpen(sock)) {
        return;
    }
    NumberField(json, "instance", &instance, 0);
    if (instance < 1 || instance > 65535) {
        Fail(sock, "instance is out of range", 0);
        return;
    }
    if (!CopyField(json, "hex", hex, (int)sizeof hex) || ParseHex(hex, g_raw, (int)sizeof g_raw, &len) != 0 ||
        len == 0) {
        Fail(sock, "hex value is invalid", 0);
        return;
    }
    rc = eip_set_attribute(&g_session, 4, (uint16_t)instance, 3, g_raw, (uint16_t)len, &cip_status);
    if (rc != EIP_OK) {
        Fail(sock, "assembly write failed", rc);
        return;
    }
    JsonReset();
    JsonFmt("{\"ok\":true,\"summary\":\"assembly %ld written\",\"bytes\":%d", instance, len);
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

/* ---- this device's adapter ---------------------------------------------- */

static const char *DirectionName(uint8_t direction)
{
    if (direction == EIP_ASM_CONSUMED) {
        return "consumed";
    }
    if (direction == EIP_ASM_PRODUCED) {
        return "produced";
    }
    return "config";
}

/* Adapter status and each assembly's data into g_json. Leaves the outer object open: the caller
 * adds the closing brace. An assembly larger than g_raw shows empty hex. "originator" is
 * view.originator_ip, where T->O goes, so it holds the group on a multicast connection. */
static int WriteAdapterJson(const char *summary)
{
    EipAdapterView view;
    int rc;
    uint16_t i;

    rc = eip_adapter_snapshot(&view);
    if (rc != EIP_OK) {
        return rc;
    }
    JsonReset();
    JsonAdd("{\"ok\":true,\"summary\":");
    JsonEscaped(summary);
    JsonAdd(",\"listening\":");
    JsonAdd(view.listening ? "true" : "false");
    JsonFmt(",\"identity\":{\"vendor\":%u,\"type\":%u,\"product\":%u,\"rev\":\"%u.%u\",\"serial\":%lu,\"name\":",
            view.vendor_id, view.device_type, view.product_code, view.revision_major, view.revision_minor,
            (unsigned long)view.serial_number);
    JsonEscaped(view.name);
    JsonAdd("},\"io\":{\"open\":");
    JsonAdd(view.io_open ? "true" : "false");
    JsonAdd(",\"run\":");
    JsonAdd(view.run ? "true" : "false");
    JsonFmt(",\"output\":%u,\"input\":%u,\"rx\":%lu,\"tx\":%lu,\"originator\":\"%u.%u.%u.%u\"",
            view.o2t_instance, view.t2o_instance, (unsigned long)view.rx_packets,
            (unsigned long)view.tx_packets, view.originator_ip[0], view.originator_ip[1],
            view.originator_ip[2], view.originator_ip[3]);
    JsonAdd("},\"assemblies\":[");
    for (i = 0; i < view.assembly_count; i++) {
        uint16_t raw_len = (uint16_t)sizeof g_raw;
        if (i != 0) {
            JsonAdd(",");
        }
        JsonFmt("{\"instance\":%u,\"direction\":", view.assemblies[i].instance);
        JsonEscaped(DirectionName(view.assemblies[i].direction));
        JsonFmt(",\"size\":%u,\"name\":", view.assemblies[i].size);
        JsonEscaped(view.assemblies[i].name);
        JsonAdd(",\"hex\":");
        if (eip_adapter_read(view.assemblies[i].instance, g_raw, &raw_len) != EIP_OK) {
            raw_len = 0;
        }
        JsonHex(g_raw, (int)raw_len);
        JsonAdd("}");
    }
    JsonAdd("]");
    return EIP_OK;
}

static void CmdAdapter(int sock)
{
    EipAdapterView view;

    if (eip_adapter_snapshot(&view) != EIP_OK) {
        Fail(sock, "adapter is not running", 0);
        return;
    }
    if (WriteAdapterJson(view.listening ? "adapter listening" : "adapter starting") != EIP_OK) {
        Fail(sock, "adapter is not running", 0);
        return;
    }
    JsonAdd("}");
    SendJson(sock);
}

/* adapter-write: set an assembly in this device's adapter. The length must equal the assembly
 * size. hex[] holds 79 characters and CopyField cuts longer text, so this cannot fill a large
 * assembly: 39 bytes at most, or 26 in the spaced form JsonHex writes. */
static void CmdAdapterWrite(int sock, ParsedJsonDataSet &json)
{
    long instance = 0;
    char hex[80];
    int len = 0;
    int rc;

    NumberField(json, "instance", &instance, 0);
    if (instance < 1 || instance > 65535) {
        Fail(sock, "instance is out of range", 0);
        return;
    }
    if (!CopyField(json, "hex", hex, (int)sizeof hex) || ParseHex(hex, g_raw, (int)sizeof g_raw, &len) != 0) {
        Fail(sock, "hex value is invalid", 0);
        return;
    }
    rc = eip_adapter_write((uint16_t)instance, g_raw, (uint16_t)len);
    if (rc == EIP_ERR_PATH) {
        Fail(sock, "no such assembly", 0);
        return;
    }
    if (rc != EIP_OK) {
        Fail(sock, "hex length must match the assembly size", 0);
        return;
    }
    if (WriteAdapterJson("assembly written") != EIP_OK) {
        Fail(sock, "adapter is not running", 0);
        return;
    }
    JsonAdd("}");
    SendJson(sock);
}

/* io-cycle: one Class 1 exchange as an originator. Forward_Open, one O->T packet, up to 1 s for
 * one T->O packet, Forward_Close. It shows that a target accepts the parameters; it is not a
 * running connection. Instance defaults are the test adapter's 150 / 100 / 101. Both directions
 * use the same RPI. */
static void CmdIoCycle(int sock, ParsedJsonDataSet &json)
{
    EipIoConfig config;
    long config_inst = 150;
    long output_inst = 100;
    long input_inst = 101;
    long out_bytes = 4;
    long in_bytes = 4;
    long rpi_ms = 20;
    long vendor = 1;
    char hex[400];
    int len = 0;
    uint8_t produced[128];
    uint8_t consumed[128];
    uint16_t consumed_len;
    int rc;
    int run;

    if (!NeedOpen(sock)) {
        return;
    }
    NumberField(json, "config", &config_inst, 150);
    NumberField(json, "output", &output_inst, 100);
    NumberField(json, "input", &input_inst, 101);
    NumberField(json, "outBytes", &out_bytes, 4);
    NumberField(json, "inBytes", &in_bytes, 4);
    NumberField(json, "rpiMs", &rpi_ms, 20);
    NumberField(json, "vendor", &vendor, 1);
    if (config_inst < 1 || output_inst < 1 || input_inst < 1 || out_bytes < 0 || in_bytes < 0 ||
        out_bytes > (long)sizeof produced || in_bytes > (long)sizeof consumed || rpi_ms < 1 || vendor < 1 ||
        vendor > 65535) {
        Fail(sock, "Class 1 field is out of range", 0);
        return;
    }
    memset(produced, 0, sizeof produced);
    if (out_bytes > 0) {
        if (!CopyField(json, "hex", hex, (int)sizeof hex) || ParseHex(hex, produced, (int)sizeof produced, &len) != 0 ||
            len != (int)out_bytes) {
            Fail(sock, "produce hex length must match out bytes", 0);
            return;
        }
    }
    if (g_io.is_open) {
        eip_io_close(&g_session, &g_io);
        memset(&g_io, 0, sizeof g_io);
    }
    g_session.vendor_id = (uint16_t)vendor; /* eip_io_open takes the vendor id from the session */
    memset(&config, 0, sizeof config);
    config.config_instance = (uint16_t)config_inst;
    config.output_instance = (uint16_t)output_inst;
    config.input_instance = (uint16_t)input_inst;
    config.output_bytes = (uint16_t)out_bytes;
    config.input_bytes = (uint16_t)in_bytes;
    config.o2t_rpi_us = (uint32_t)rpi_ms * 1000u;
    config.t2o_rpi_us = config.o2t_rpi_us;
    config.run_idle = json("runIdle").PermissiveCurrentBool() ? 1u : 0u;
    config.timeout_multiplier = 2;
    run = json("run").Valid() ? (json("run").PermissiveCurrentBool() ? 1 : 0) : 1;

    rc = eip_io_open(&g_session, &g_io, &config);
    if (rc != EIP_OK) {
        Fail(sock, "Class 1 open failed", rc);
        return;
    }
    rc = eip_io_produce(&g_io, produced, (uint16_t)out_bytes, run);
    if (rc != EIP_OK) {
        eip_io_close(&g_session, &g_io);
        memset(&g_io, 0, sizeof g_io);
        Fail(sock, "Class 1 produce failed", rc);
        return;
    }
    consumed_len = (uint16_t)in_bytes;
    rc = eip_io_consume(&g_io, consumed, &consumed_len, 1000);
    eip_io_close(&g_session, &g_io);
    memset(&g_io, 0, sizeof g_io);
    if (rc != EIP_OK) {
        Fail(sock, "Class 1 consume failed", rc);
        return;
    }
    JsonReset();
    JsonAdd("{\"ok\":true,\"summary\":\"Class 1 cycle finished\",\"hex\":");
    JsonHex(consumed, consumed_len);
    JsonSession();
    JsonAdd("}");
    SendJson(sock);
}

/* ---- HTTP entry point --------------------------------------------------- */

/* POST /api/cmd. NNDK parses the body and calls this in its HTTP task. g_busy refuses a command
 * while another runs, but it is a plain flag, not a lock: the test and set are not atomic, and the
 * "busy" reply itself rewrites g_json. With the single HTTP task it is never seen set. */
static void CmdHandler(int sock, ParsedJsonDataSet &json)
{
    char cmd[24];

    if (g_busy) {
        Fail(sock, "busy", 0);
        return;
    }
    g_busy = 1;
    if (!json.CheckLint()) {
        Fail(sock, "invalid JSON", 0);
        g_busy = 0;
        return;
    }
    if (!CopyField(json, "cmd", cmd, (int)sizeof cmd)) {
        Fail(sock, "cmd is required", 0);
        g_busy = 0;
        return;
    }
    iprintf("EIP %s\r\n", cmd);
    if (strcmp(cmd, "scan") == 0) {
        CmdScan(sock);
    } else if (strcmp(cmd, "connect") == 0) {
        CmdConnect(sock, json);
    } else if (strcmp(cmd, "disconnect") == 0) {
        CloseTarget();
        JsonReset();
        JsonAdd("{\"ok\":true,\"summary\":\"disconnected\"");
        JsonSession();
        JsonAdd("}");
        SendJson(sock);
    } else if (strcmp(cmd, "symbols") == 0) {
        CmdSymbols(sock, json);
    } else if (strcmp(cmd, "tag-read") == 0) {
        CmdTagRead(sock, json);
    } else if (strcmp(cmd, "tag-write") == 0) {
        CmdTagWrite(sock, json);
    } else if (strcmp(cmd, "asm-read") == 0) {
        CmdAsmRead(sock, json);
    } else if (strcmp(cmd, "asm-write") == 0) {
        CmdAsmWrite(sock, json);
    } else if (strcmp(cmd, "io-cycle") == 0) {
        CmdIoCycle(sock, json);
    } else if (strcmp(cmd, "adapter") == 0) {
        CmdAdapter(sock);
    } else if (strcmp(cmd, "adapter-write") == 0) {
        CmdAdapterWrite(sock, json);
    } else {
        Fail(sock, "unknown cmd", 0);
    }
    g_busy = 0;
}

/* The HTTP matcher skips one leading slash on the request, then compares
 * the rest to this name. A leading slash here never matches. */
static JsonPostCallbackHandler gCmdHandler("api/cmd", CmdHandler);
