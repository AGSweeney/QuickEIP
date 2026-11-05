# QuickEIP

A small EtherNet/IP stack for embedded targets, in portable C99. One code base
works as a **scanner** (talks to PLCs and other devices) and as an **adapter**
(looks like an I/O device to a PLC). It runs on NetBurner, ESP32, Teknic
ClearCore, desktop Linux and Windows.

- No dynamic allocation in the protocol code. All sizes are set at build time.
- No OS calls in the protocol code. A port of 19 functions supplies the clock,
  a lock, interface information and non-blocking sockets.
- The adapter core is sans-I/O: it takes packets and a tick count and returns
  packets, so it can be tested without a network.

Version 2.0.0. MIT licensed.

## Contents

- [What it does](#what-it-does)
- [Quick start: adapter](#quick-start-adapter)
- [Quick start: scanner](#quick-start-scanner)
- [Platforms](#platforms)
- [Architecture](#architecture)
- [Limitations](#limitations)
- [Memory and configuration](#memory-and-configuration)
- [Host build and tests](#host-build-and-tests)
- [eiptool](#eiptool)
- [Examples](#examples)
- [Repository layout](#repository-layout)
- [Documentation](#documentation)

## What it does

### Scanner (originator)

| Feature | Details |
|---|---|
| Sessions | Encapsulation session on TCP 44818. All explicit messages use SendRRData (unconnected). |
| Explicit messages | Any service with `eip_cip()`, plus Get and Set Attribute Single helpers. Requests up to 480 bytes. |
| Routing | Direct, or one hop across a ControlLogix backplane with Unconnected Send (`slot` 0–255). Tested against scripted replies, not yet on a real ControlLogix. |
| Logix tags | Read and write by name, for example `Program:Main.Speed` or `Data[2,3].Value`. Large transfers are fragmented automatically. Data stays as raw little-endian bytes. |
| Symbol list | Tag list from the Symbol object, on Micro800 and Logix. |
| Class 1 I/O | Forward_Open as an originator, point-to-point, with an optional run/idle header. Assemblies up to 600 bytes. The application paces the packets. |
| Discovery | ListIdentity to one address, or a broadcast scan on every interface. |

### Adapter (target)

| CIP object | Supported |
|---|---|
| Identity (0x01) | Attributes 1–8, Get_Attributes_All. Reset (types 0 and 1) when the application provides a callback. |
| Message Router (0x02) | Multiple Service Packet. |
| Assembly (0x04) | Up to 8 instances by default, from a shared data pool. Get and Set of the data attribute, and size. |
| Connection Manager (0x06) | Forward_Open, Large_Forward_Open, Forward_Close. |
| TCP/IP Interface (0xF5) | Read-only by default. Attributes 3, 5 and 6 become settable when the application provides a callback. |
| Ethernet Link (0xF6) | Three instances, read-only: the port in use, a second port that always reports down, and an internal port. |

The adapter also provides:

- ListIdentity and ListServices on TCP and UDP 44818, with a rate limit on UDP
  replies, plus ListInterfaces, RegisterSession and SendRRData on TCP.
- Class 1 I/O on UDP 2222, with point-to-point or multicast T→O and optional
  run/idle on O→T.
- An inactivity watchdog, sequence and source address checks, and a defined
  safe state: consumed data reads as zeros after a timeout or close and in idle.
- An `allow_change` callback that can refuse an IP change or a reset, for
  example while machinery is moving.

## Quick start: adapter

```c
#include <string.h>
#include "eip.h"

static const EipAssemblyConfig assemblies[] = {
    /* instance, direction,  size, name, initial data, implicit_only */
    {150, EIP_ASM_CONSUMED, 32, "Outputs", 0, 0},   /* PLC -> device */
    {100, EIP_ASM_PRODUCED, 32, "Inputs",  0, 0},   /* device -> PLC */
    {151, EIP_ASM_CONFIG,    0, "Config",  0, 0},   /* config point of the connection path */
};

int start_adapter(void)
{
    EipAdapterConfig cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.vendor_id = 65000;           /* use your own ODVA vendor id */
    cfg.device_type = 43;            /* generic device */
    cfg.product_code = 1;
    cfg.revision_major = 1;
    cfg.revision_minor = 1;          /* an EDS needs 1 or higher */
    cfg.product_name = "My Device";
    cfg.assemblies = assemblies;
    cfg.assembly_count = 3;
    return eip_adapter_start(&cfg);  /* copies what it needs from cfg */
}

/* Call from your application loop. */
void exchange_data(void)
{
    uint8_t outputs[32];
    uint8_t inputs[32] = {0};
    uint16_t len = sizeof outputs;

    eip_adapter_read(150, outputs, &len);   /* what the PLC sent; zeros after a timeout or in idle */
    /* ... use outputs, fill inputs ... */
    eip_adapter_write(100, inputs, sizeof inputs);   /* what the PLC receives */
    /* bare-metal ports (ClearCore) must also call eip_adapter_poll() on every pass */
}
```

The sizes in your EDS file must match these assemblies, or the PLC's
Forward_Open is refused. A sample is in `examples/eds`, and
[INTEGRATION.md §3.6](docs/INTEGRATION.md#36-connecting-a-plc) covers
connecting a PLC.

## Quick start: scanner

```c
#include <stdio.h>
#include "eip.h"

static EipSession session;   /* about 4.7 KB: keep it off the stack */

int read_tag(void)
{
    EipOpenConfig cfg = {0};
    uint8_t data[4];
    uint16_t len = sizeof data;
    uint16_t type = 0;
    int rc;

    cfg.ip = "192.168.1.10";
    cfg.slot = -1;   /* -1 sends straight to the device. A zeroed config means slot 0! */

    rc = eip_session_open(&session, &cfg);
    if (rc != EIP_OK) {
        printf("open failed: %s\n", eip_status_text(rc));
        return rc;
    }
    rc = eip_tag_read(&session, "MyDint", 1, data, &len, &type, 0);
    if (rc == EIP_OK && type == EIP_DINT) {
        printf("MyDint = %d\n", (int)(int32_t)eip_load_le32(data));
    } else if (rc == EIP_ERR_CIP) {
        printf("CIP status 0x%02X\n", eip_last_cip_status(&session));
    }
    eip_session_close(&session);
    return rc;
}
```

Use `slot = -1` for CompactLogix, Micro800 and any device reached directly.
Use the slot number for a controller behind a ControlLogix Ethernet module.
Scanner calls block, so on an RTOS give them their own task.

## Platforms

| Platform | Port | Adapter runs in | Build | Verified |
|---|---|---|---|---|
| NetBurner NNDK 3.x | `ports/netburner` | its own task | NNDK makefile | Builds (NNDK 3.5.8, MODM7AE70). Not yet run on hardware. |
| ESP-IDF 5.x (ESP32) | `ports/esp-idf` + `ports/bsd_sockets` | its own task | IDF component | Builds (IDF 5.5.1, esp32). Not yet run on hardware. |
| Teknic ClearCore | `ports/clearcore` + `ports/lwip_raw` | `eip_adapter_poll()` in the main loop | Microchip Studio | Builds and links (Enhanced ClearCore Library, arm-gcc 6.3). The adapter has run on hardware with a Micro850 PLC. |
| Linux, WSL, macOS | `ports/posix` + `ports/bsd_sockets` | a thread | CMake | Unit and loopback tests pass, also under ASan and UBSan (gcc 13.3, Ubuntu 24.04 on WSL2). |
| Windows 7 and later | `ports/windows` | a thread, or `eip_adapter_poll()` with `EIP_WIN_POLL_MODE` | CMake | Unit, port-contract and loopback tests pass (Visual Studio 2022, x64 and Win32). Explicit scanner calls checked against a Micro850. Adapter not yet tested from a real PLC. |

Step-by-step setup for each platform is in
[docs/INTEGRATION.md](docs/INTEGRATION.md). To add a platform, see
[docs/PORTING.md](docs/PORTING.md): most ports reuse `ports/bsd_sockets` or
`ports/lwip_raw` and only supply a short platform file.

## Architecture

```
                         your application
             │ eip.h (scanner, built-in adapter)  │ eip_server.h (own the adapter instance)
             ▼                                    ▼
 ┌─ src/  portable C99, no OS calls ──────────────────────────────┐
 │  scanner                        adapter                        │
 │  eip_session  tags, messages    eip_adapter_default  built-in  │
 │  eip_io       Class 1           eip_server           sockets   │
 │  eip_scan     discovery         eip_adapter          sans-I/O  │
 │                                                                │
 │  eip_codec    packet encoding and parsing, used by both sides  │
 └───────────────────────────────┬────────────────────────────────┘
                                 │ eip_hal.h (19 functions)
      ┌──────────────┬───────────┴───┬────────────────┬───────────┐
  netburner       esp-idf          posix          clearcore    windows
                     └─ bsd_sockets ─┘             └ lwip_raw
```

Applications that want their own event loop can use `eip_adapter.h`, the
sans-I/O core, directly.

The adapter's core runs in one context: a task on RTOS ports, or
`eip_adapter_poll()` on bare metal. `eip_adapter_read`, `eip_adapter_write` and
`eip_adapter_snapshot` are safe from any task. One task at a time may use a
given `EipSession` or `EipIo`.

## Limitations

Check these against your application before you commit to QuickEIP.

**Adapter**

- One Class 1 connection at a time. A second originator is refused, even when
  T→O is multicast, so no listen-only sharing.
- No Class 3 connections, so explicit messages are unconnected only.
- No Unconnected Send, so the adapter can't route.
- The T→O connection uses cyclic production. A change-of-state request is
  accepted but produces on the RPI. Application-triggered connections are
  refused.
- A connection with a null O→T has no inactivity watchdog, and I/O isn't tied
  to the TCP session. It lasts until a Forward_Close or until the same
  originator opens it again.
- Each explicit reply carries at most 508 bytes of assembly data, and a Class 1
  assembly is limited to about 500 bytes.

**Scanner**

- Only one routing hop. Explicit messages can be routed, but use `slot = -1`
  for Class 1: a routed Forward_Open hasn't been tested.
- Tag data is not decoded. Elementary types work. `eip_tag_write` has no form
  for a structure, and `eip_tag_read` returns a structure with its header bytes
  in the data, so UDTs and STRINGs aren't supported as such.
- Bit access by name (`Tag.3`) isn't supported.
- A packet cut short or too large closes the session, because the stream can't
  be resynchronized. After a socket error or repeated timeouts, close the session
  and open it again: `eip_session_is_open` doesn't detect a lost peer.

## Memory and configuration

With the default settings:

| Item | RAM |
|---|---|
| Built-in adapter | about 11 KB static, plus a 1.5 KB shared reply buffer |
| Each `EipSession` | about 4.7 KB |
| Each `EipIo` | about 100 bytes, plus a 1.5 KB stack buffer during produce and consume |

Sizes are `#ifndef` defaults in [include/eip_config.h](include/eip_config.h).
The main ones are `EIP_MAX_PACKET` (1536, don't go below 1024),
`EIP_ADAPTER_MAX_ASSEMBLIES` (8), `EIP_ADAPTER_DATA_POOL` (512) and
`EIP_ADAPTER_MAX_CLIENTS` (4). Override them with `-D`, or list them in a header
and build with `-DEIP_CONFIG_FILE=\"my_eip_config.h\"`.

> **Every file that includes `eip.h` must see the same values**, in C and C++
> alike. These settings change struct sizes, so a mismatch corrupts memory.

All settings are in
[INTEGRATION.md §2](docs/INTEGRATION.md#2-build-time-configuration).

## Host build and tests

Needs CMake 3.13 or later, a C99 compiler and a Unix-like host.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

| Target | What it is |
|---|---|
| `quickeip_core` | Portable core only. Link your own `eip_hal_*` port. |
| `quickeip` | Core plus the host port (POSIX or Windows). |
| `eiptool` | Command-line tool (`-DQUICKEIP_BUILD_TOOLS=OFF` to skip). |
| `test_core`, `test_session`, `test_hal`, `test_loopback` | Tests (`-DQUICKEIP_BUILD_TESTS=OFF` to skip). |

| Test | Covers | Needs |
|---|---|---|
| `test_core` | The codec and the adapter core, byte for byte, including Forward_Open captures from real devices. | Nothing: no sockets or clock. |
| `test_session` | The scanner session against a scripted device through a fake HAL: routed requests and router failures, late, cut-short and oversize replies. | Nothing: no sockets or clock. |
| `test_hal` | The port contract, called directly over this host's loopback: busy ports fail, long datagrams truncate, ICMP errors aren't fatal, timeouts hold, the clock really steps at its reported resolution. | Free TCP and UDP ports in the 478xx range. |
| `test_loopback` | Scanner against adapter through the host port: discovery, explicit messages, Class 1 both ways, idle, the watchdog, reopen. | A non-loopback interface, and TCP and UDP 44818 and UDP 2222 free. |

## eiptool

A command-line scanner and test adapter, built on the host port.

```
eiptool scan
eiptool identity <ip>
eiptool tags <ip> [slot]
eiptool read <ip> <tag> [slot]
eiptool write <ip> <tag> <dint|int|sint|real|bool> <value> [slot]
eiptool get <ip> <class> <instance> <attribute> [slot]
eiptool adapter
```

`slot` defaults to -1. Numbers may be decimal or `0x` hex. `adapter` runs the
built-in test adapter until you stop it, which is handy as a target for a new
port. Set `EIP_NETIF=<name>` to choose the interface the adapter reports and
the scan uses first.

```sh
./build/eiptool adapter &
./build/eiptool get 127.0.0.1 4 101 3    # test assembly: 11 22 33 44 55 66 77 88
```

Explicit messages work on `127.0.0.1`, but Class 1 needs the address the
adapter reports.

## Examples

| Directory | Shows |
|---|---|
| `examples/netburner_adapter` | An adapter on NetBurner, and an optional scanner tag read. |
| `examples/esp_idf_adapter` | An adapter as an ESP-IDF project. |
| `examples/clearcore_adapter` | A poll-mode adapter driving ClearCore I/O. |
| `examples/posix_eiptool` | `eiptool`, using most of the scanner API. |
| `examples/eds` | A sample EDS file matching the quick start. |
| `examples/netburner_webui` | A browser test page. Application code, not part of the stack. It has no `main` or makefile, so add it to an NNDK application. |

## Repository layout

```
include/   eip.h         public API: scanner and built-in adapter
           eip_config.h  build-time sizes
           eip_hal.h     port contract
           eip_server.h  adapter instances you own
           eip_adapter.h sans-I/O adapter core
src/       portable protocol code
ports/     one directory per platform, plus the shared socket layers
examples/  see above
tests/     test_core, test_session, test_hal, test_loopback
docs/      INTEGRATION.md, PORTING.md, MIGRATION.md
```

The repository root is also an ESP-IDF component (`CMakeLists.txt`,
`idf_component.yml`).

## Documentation

| Document | Read it to |
|---|---|
| [docs/INTEGRATION.md](docs/INTEGRATION.md) | Add QuickEIP to a project: configuration, the adapter and scanner APIs, connecting a PLC, setup per platform, troubleshooting. |
| [docs/PORTING.md](docs/PORTING.md) | Write a port for a new platform, and check it. |
| [docs/MIGRATION.md](docs/MIGRATION.md) | Upgrade from QuickEIP 1.x (NetBurner). |

The API reference is the comments in `include/`, starting with `eip.h`, plus
each port's own header (`ports/esp-idf/eip_esp_idf.h`,
`ports/clearcore/eip_clearcore.h`).

Using a coding agent? Point it at [AGENTS.md](AGENTS.md).

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2026 Adam G. Sweeney.
