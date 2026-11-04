# Migrating to QuickEIP 2.0

QuickEIP 2.0 replaces the NetBurner-only 1.x (`eip_session.cpp`,
`eip_io.cpp`, `eip_adapter_task.cpp`, plus the web UI) with a portable
library. What's new in the adapter:

- Configurable identity and assemblies.
- A shared assembly data pool, which removes the 16-byte-per-assembly limit.
- Multicast T→O.
- The TCP/IP Interface and Ethernet Link objects, settable IP configuration
  and Identity Reset.
- Input hardening.
- Sizes moved to `eip_config.h`.

1.x's fallback to the 16-bit sequence count stays, for originators that send
no Sequenced Address item. A repeated count keeps the connection alive
without replacing the data, and an older count is dropped.

The socket code is now one portable server (`src/eip_server.c`), shared by
every platform.

## From QuickEIP 1.x (NetBurner)

### Build

Replace the old makefile lines:

```make
# old
C_SRC   += eip/src/eip_codec.c eip/src/eip_adapter.c
CPP_SRC += eip/src/eip_session.cpp eip/src/eip_io.cpp eip/src/eip_adapter_task.cpp
```

with the list in [INTEGRATION.md §6](INTEGRATION.md#6-netburner-nndk).
`web.cpp` and `htmldata.cpp` are no longer part of the library. They're in
`examples/netburner_webui/` and use the new API.

### API changes

| 1.x | 2.0 | Notes |
|---|---|---|
| `EipOpenConfig.timeout_ticks` | `timeout_ms` | **All timeouts are milliseconds now.** `TICKS_PER_SECOND * 5` becomes `5000`. |
| `eip_list_identity(ip, id, ticks)` | `(ip, id, ms)` | |
| `eip_scan_devices(d, n, ticks)` | `(d, n, ms)` | 0 still selects the default (3 s). |
| `eip_io_consume(io, d, n, ticks)` | `(io, d, n, ms)` | Passing `TICKS_PER_SECOND` (20) would now mean 20 ms. |
| `session.tcp_fd > 0` | `eip_session_is_open(&session)` | The field is now `sock` (an `eip_sock_t`), and 0 is a valid handle. |
| `io.udp_fd` | `io.sock` | |
| `EipSession.timeout_ticks` | `timeout_ms` | |
| `eip_adapter_start(void)` | `eip_adapter_start(const EipAdapterConfig *)` | Pass `0` for the old behavior: test identity, assemblies 100/101/102/150. |
| `EIP_MAX_PACKET` (enum) | macro in `eip_config.h` | Same value. It can now be overridden. |
| `EIP_ADAPTER_ASSEMBLIES` (4) | `EIP_ADAPTER_MAX_ASSEMBLIES` (8) | The old name is still an alias. |
| `src/eip_port.h` | `src/eip_internal.h` | Internal header, renamed. |
| `src/eip_adapter.h` | `include/eip_adapter.h` | Now public (the sans-I/O core). |
| `EipAdapterView` | more fields | Added `ip`, `multicast`, `rpi_us`, `timeout_ms`, `opens`, `timeouts`, `rx_dropped_*`, `rx_idle`, `udp_replies_dropped`, `tcp_clients`. |

### Behavior changes to be aware of

- **Inactivity timeout** is now calculated from the **O→T** RPI (CIP
  Vol. 1: RPI × 4 << multiplier). 1.x used the T→O interval. The
  2-second minimum is gone. The minimum is now two clock ticks, which is
  100 ms at 20 Hz, or `timeout_floor_ms` if you set it higher. For 1.x
  behavior, set `cfg.timeout_floor_ms = 2000`.
- **Identity status** in the adapter now reads "configured" (bit 2):
  `0x0034` with no connection, `0x0065` run, `0x0075` idle.
- **Unknown assembly instance** answers CIP `0x16` (object does not
  exist), not `0x05`.
- **Consumed data on idle or timeout** goes to all zeros, as in 1.x.
  On idle, only the consumed point of the connection is cleared.
- The adapter accepts up to 4 TCP clients, not 2, and evicts a session
  that has been silent for 60 s when all slots are busy.
