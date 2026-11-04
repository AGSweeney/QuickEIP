/* MIT License. Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com> */
/* The built-in adapter instance behind eip_adapter_start() and friends.
 * Kept in its own file so a program that never calls them does not link
 * the static EipServer. Each call forwards to eip_server.c; only start also
 * involves the port, through eip_hal_adapter_start_task. */
#include "eip_hal.h"
#include "eip_server.h"

static EipServer g_server;
static volatile int g_started;

EipServer *eip_adapter_default_server(void)
{
    return &g_server;
}

/* The test of g_started is not atomic, so call this from one task. There is
 * no matching stop: an adapter task, once started, runs for good.
 *
 * g_started is set before the runner starts so the instance already reads as
 * started when a new task first runs, and is cleared again if that fails. */
int eip_adapter_start(const EipAdapterConfig *config)
{
    int rc;

    if (g_started) {
        return EIP_ERR_STATE;
    }
    rc = eip_server_init(&g_server, config);
    if (rc != EIP_OK) {
        return rc;
    }
    g_started = 1;
    rc = eip_hal_adapter_start_task(&g_server);
    if (rc != EIP_OK) {
        eip_server_close(&g_server);
        g_started = 0;
    }
    return rc;
}

/* For poll-mode ports. Nothing here checks for a running adapter task, so on
 * a port that has one this would be a second context on the same server. */
uint32_t eip_adapter_poll(void)
{
    if (!g_started) {
        return EIP_ADAPTER_IDLE_WAIT_MS;
    }
    return eip_server_poll(&g_server);
}

int eip_adapter_snapshot(EipAdapterView *view)
{
    return g_started ? eip_server_snapshot(&g_server, view) : EIP_ERR_STATE;
}

int eip_adapter_read(uint16_t instance, uint8_t *data, uint16_t *data_len)
{
    return g_started ? eip_server_read(&g_server, instance, data, data_len) : EIP_ERR_STATE;
}

int eip_adapter_write(uint16_t instance, const uint8_t *data, uint16_t data_len)
{
    return g_started ? eip_server_write(&g_server, instance, data, data_len) : EIP_ERR_STATE;
}
