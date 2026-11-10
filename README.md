# QuickEIP

QuickEIP is a small EtherNet/IP stack for embedded targets, in portable C99.
One code base has two functions:

- As a **scanner**, it sends requests to PLCs and other devices.
- As an **adapter**, a PLC sees it as an I/O device.

QuickEIP runs on NetBurner, ESP32, Teknic ClearCore, desktop Linux, and
Windows.

The stack has these properties:

- The protocol code does no dynamic allocation. You set all sizes at build
  time.
- The protocol code makes no OS calls. A port of 19 functions supplies the
  clock, a lock, interface information, and non-blocking sockets.
- The adapter core is sans-I/O. It takes packets and a tick count, and it
  returns packets. Thus, you can test it without a network.

> **WARNING:** QuickEIP does not implement CIP Safety. Do not use QuickEIP as
> a safety function. Use a certified safety system to protect persons and
> machinery.

Version 2.0.0. MIT license.

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

The table lists the functions of the scanner.

| Feature | Details |
|---|---|
| Sessions | An encapsulation session on TCP port 44818. All explicit messages use SendRRData (unconnected). |
| Explicit messages | `eip_cip()` sends any service. Helper functions do Get_Attribute_Single and Set_Attribute_Single. A request can be 480 bytes or fewer. |
| Routing | Direct, or one hop across a ControlLogix backplane with Unconnected Send (`slot` 0–255). Tested with scripted replies. Not yet tested with a ControlLogix. |
| Logix tags | Read and write by name, for example `Program:Main.Speed` or `Data[2,3].Value`. The scanner divides a large transfer into fragments. The data stays as raw little-endian bytes. |
| Symbol list | The tag list from the Symbol object, on Micro800 and Logix. |
| Class 1 I/O | Forward_Open as an originator, point-to-point, with an optional run/idle header. An assembly can be 600 bytes or fewer. Your application sets the packet rate. |
| Discovery | ListIdentity to one address, or a broadcast scan on each interface. |

### Adapter (target)

The table lists the CIP objects of the adapter.

| CIP object | Functions |
|---|---|
| Identity (`0x01`) | Attributes 1–8 and Get_Attributes_All. Reset (types 0 and 1), if your application supplies a callback. |
| Message Router (`0x02`) | Multiple Service Packet. |
| Assembly (`0x04`) | Eight instances or fewer by default, from a shared data pool. Get and Set of the data attribute, and Get of the size. |
| Connection Manager (`0x06`) | Forward_Open, Large_Forward_Open, and Forward_Close. |
| TCP/IP Interface (`0xF5`) | Read-only by default. If your application supplies a callback, a scanner can set attributes 3, 5, and 6. |
| Ethernet Link (`0xF6`) | Three read-only instances: the port in use, a second port that always reports link down, and an internal port. |

The adapter also has these functions:

- It answers ListIdentity and ListServices on TCP port 44818 and UDP port
  44818. A rate limit applies to the UDP replies.
- It answers ListInterfaces, RegisterSession, and SendRRData on TCP.
- It does Class 1 I/O on UDP port 2222. T→O can be point-to-point or
  multicast. O→T can have a run/idle header.
- It has an inactivity watchdog, and it checks the sequence number and the
  source address of each packet.
- It has a defined safe state for consumed data.
- It has an `allow_change` callback. Your application can use the callback to
  refuse an IP change or a reset.

> **WARNING:** A consumed assembly reads as all zeros after a timeout, after a
> close, and while the PLC is in idle. Make sure that all zeros is a safe
> state for your machinery.

> **WARNING:** A scanner can change the IP settings of the adapter or reset
> it while machinery moves. Use the `allow_change` callback to refuse these
> requests when a change is not safe.

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

The vendor ID 65000 is a placeholder. Use the vendor ID that ODVA gave you.

The sizes in your EDS file must agree with these assemblies. If they do not
agree, the adapter refuses the Forward_Open from the PLC. These documents
help you:

- `examples/eds` contains a sample EDS file.
- [INTEGRATION.md §3.6](docs/INTEGRATION.md#36-connecting-a-plc) tells you
  how to connect a PLC.
- On Windows, [EDS Builder](tools/eds_builder/README.md) writes the EDS file
  from your source files.

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

These rules apply:

- Set `slot = -1` for a CompactLogix, a Micro800, and each device that you
  connect to directly.
- Set `slot` to the slot number for a controller behind a ControlLogix
  Ethernet module.
- Scanner calls block. On an RTOS, call them from a task of their own.

## Platforms

The table gives the port, the build system, and the test status of each
platform.

| Platform | Port | Adapter runs in | Build | Test status |
|---|---|---|---|---|
| NetBurner NNDK 3.x | `ports/netburner` | A task of its own | NNDK makefile | Builds (NNDK 3.5.8, MODM7AE70). Not yet tested on hardware. |
| ESP-IDF 5.x (ESP32) | `ports/esp-idf` + `ports/bsd_sockets` | A task of its own | IDF component | Builds (IDF 5.5.1, esp32). Not yet tested on hardware. |
| Teknic ClearCore | `ports/clearcore` + `ports/lwip_raw` | `eip_adapter_poll()` in the main loop | Microchip Studio | Builds (Enhanced ClearCore Library, arm-gcc 6.3). The adapter runs on a ClearCore. Tested with a Micro850. |
| Linux, WSL, macOS | `ports/posix` + `ports/bsd_sockets` | A thread | CMake | Runs on Ubuntu 24.04 on WSL2 (gcc 13.3). The unit tests and the loopback tests pass, also with ASan and UBSan. Not yet tested with a PLC. |
| Windows 7 and later | `ports/windows` | A thread, or `eip_adapter_poll()` with `EIP_WIN_POLL_MODE` | CMake | Runs on Windows (Visual Studio 2022, x64 and Win32). The unit tests, the port-contract test, and the loopback tests pass. Explicit scanner calls tested with a Micro850. Adapter not yet tested with a PLC. |

> **WARNING:** Windows, Linux, and macOS are not real-time platforms. Packet
> timing can change without notice. Do not use the adapter on these platforms
> as a safety function. On Windows, also set a longer inactivity timeout (see
> [INTEGRATION.md §10](docs/INTEGRATION.md#10-windows)).

[docs/INTEGRATION.md](docs/INTEGRATION.md) gives the setup procedure for each
platform. To add a platform, see [docs/PORTING.md](docs/PORTING.md). Most
ports use `ports/bsd_sockets` or `ports/lwip_raw` and add only a short
platform file.

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

An application with an event loop of its own can use `eip_adapter.h`
directly. That header is the sans-I/O core.

These rules apply to tasks:

- The adapter core runs in one context. On an RTOS port, the context is a
  task. On bare metal, the context is `eip_adapter_poll()`.
- You can call `eip_adapter_read`, `eip_adapter_write`, and
  `eip_adapter_snapshot` from any task.
- Only one task at a time can use a given `EipSession` or `EipIo`.

## Limitations

Compare these limits with the needs of your application before you select
QuickEIP.

**Adapter**

- The adapter has one Class 1 connection at a time. It refuses a second
  originator, also when T→O is multicast. Thus, it has no listen-only
  connections.
- The adapter has no Class 3 connections. All explicit messages are
  unconnected.
- The adapter has no Unconnected Send. It cannot route a message.
- The T→O connection uses cyclic production. The adapter accepts a
  change-of-state request, but it produces at the requested packet interval
  (RPI). It refuses an application-triggered connection.
- A connection with a null O→T has no inactivity watchdog. The I/O does not
  depend on the TCP session. The connection stays open until a Forward_Close,
  or until the same originator opens it again.
- Each explicit reply contains 508 bytes of assembly data or fewer. A Class 1
  assembly can be approximately 500 bytes or fewer.

**Scanner**

- The scanner routes through one hop only. It can route explicit messages.
  For Class 1, set `slot = -1`. A routed Forward_Open is not yet tested.
- The scanner does not decode tag data. You can read and write elementary
  types.
- `eip_tag_write` has no form for a structure. `eip_tag_read` returns a
  structure with its header bytes in the data. Thus, the scanner has no
  special functions for UDTs and STRINGs.
- You cannot access a bit by name (`Tag.3`).
- A packet that is cut short or too large closes the session, because the
  scanner cannot find the start of the next packet.
- `eip_session_is_open` does not find a lost peer. After a socket error or
  repeated timeouts, close the session and open it again.

## Memory and configuration

The table gives the RAM use with the default settings.

| Item | RAM |
|---|---|
| Built-in adapter | Approximately 11 KB static, plus a shared reply buffer of 1.5 KB |
| Each `EipSession` | Approximately 4.7 KB |
| Each `EipIo` | Approximately 100 bytes, plus a stack buffer of 1.5 KB during produce and consume |

The sizes are `#ifndef` defaults in
[include/eip_config.h](include/eip_config.h). The table gives the most
important ones.

| Setting | Default | Remarks |
|---|---|---|
| `EIP_MAX_PACKET` | 1536 | Do not set a value less than 1024. |
| `EIP_ADAPTER_MAX_ASSEMBLIES` | 8 | |
| `EIP_ADAPTER_DATA_POOL` | 512 | |
| `EIP_ADAPTER_MAX_CLIENTS` | 4 | |

To change a setting, use `-D` on the compiler command line. As an
alternative, put the settings in a header and build with
`-DEIP_CONFIG_FILE=\"my_eip_config.h\"`.

> **CAUTION:** Each file that includes `eip.h` must see the same values, in C
> and in C++. These settings change the sizes of structures. Different values
> corrupt memory.

[INTEGRATION.md §2](docs/INTEGRATION.md#2-build-time-configuration) lists all
settings.

## Host build and tests

The build needs CMake 3.13 or later and a C99 compiler. Use Linux, macOS,
WSL, or Windows with Visual Studio.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The first table lists the build targets. The second table lists the tests.

| Target | Description |
|---|---|
| `quickeip_core` | The portable core only. Link a port (`eip_hal_*`) of your own. |
| `quickeip` | The core plus the host port (POSIX or Windows). |
| `eiptool` | The command-line tool. Set `-DQUICKEIP_BUILD_TOOLS=OFF` to omit it. |
| `test_core`, `test_session`, `test_hal`, `test_loopback` | The tests. Set `-DQUICKEIP_BUILD_TESTS=OFF` to omit them. |

| Test | Covers | Needs |
|---|---|---|
| `test_core` | The codec and the adapter core, byte for byte. It includes Forward_Open captures from real devices. | No sockets and no clock. |
| `test_session` | The scanner session with a scripted device through a fake HAL. It includes routed requests, router failures, and late, short, and oversize replies. | No sockets and no clock. |
| `test_hal` | The port contract, through direct calls on the loopback interface of the host. A busy port fails. A long datagram is truncated. An ICMP error is not fatal. Timeouts are correct. The clock steps at its reported resolution. | Free TCP ports and UDP ports in the 478xx range. |
| `test_loopback` | The scanner with the adapter through the host port: discovery, explicit messages, Class 1 in the two directions, idle, the watchdog, and a second open. | An interface that is not loopback. TCP port 44818, UDP port 44818, and UDP port 2222 must be free. |

## eiptool

`eiptool` is a command-line scanner and test adapter. It uses the host port.

```
eiptool scan
eiptool identity <ip>
eiptool tags <ip> [slot]
eiptool read <ip> <tag> [slot]
eiptool write <ip> <tag> <dint|int|sint|real|bool> <value> [slot]
eiptool get <ip> <class> <instance> <attribute> [slot]
eiptool adapter
```

These rules apply:

- The default value of `slot` is -1.
- A number can be decimal or hexadecimal with `0x`.
- `adapter` runs the built-in test adapter until you stop it. Use it as a
  target when you test a new port.
- `EIP_NETIF=<name>` selects the interface that the adapter reports and that
  the scan uses first.

```sh
./build/eiptool adapter &
./build/eiptool get 127.0.0.1 4 101 3    # test assembly: 11 22 33 44 55 66 77 88
```

You can send explicit messages to `127.0.0.1`. Class 1 needs the address
that the adapter reports.

## Examples

The table lists the example directories.

| Directory | Shows |
|---|---|
| `examples/netburner_adapter` | An adapter on NetBurner, and an optional scanner tag read. |
| `examples/esp_idf_adapter` | An adapter as an ESP-IDF project. |
| `examples/clearcore_adapter` | A poll-mode adapter that controls ClearCore I/O. |
| `examples/posix_eiptool` | `eiptool`, which uses most of the scanner API. |
| `examples/eds` | A sample EDS file that agrees with the quick start. |
| `examples/netburner_webui` | A browser test page. It is application code, not a part of the stack. It has no `main` and no makefile. Add it to an NNDK application. |

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
tools/     eds_builder: a Windows tool that writes an adapter's EDS file
docs/      INTEGRATION.md, PORTING.md, MIGRATION.md
```

The repository root is also an ESP-IDF component (`CMakeLists.txt`,
`idf_component.yml`).

## Documentation

The table lists the documents.

| Document | Read it to |
|---|---|
| [docs/INTEGRATION.md](docs/INTEGRATION.md) | Add QuickEIP to a project. It covers configuration, the adapter API, the scanner API, PLC connection, setup for each platform, and troubleshooting. |
| [docs/PORTING.md](docs/PORTING.md) | Write a port for a new platform, and test it. |
| [docs/MIGRATION.md](docs/MIGRATION.md) | Upgrade from QuickEIP 1.x (NetBurner). |
| [docs/WRITING_STYLE.md](docs/WRITING_STYLE.md) | Write or change documentation. |
| [tools/eds_builder/README.md](tools/eds_builder/README.md) | Write an EDS file from your source files, on Windows. |

The API reference is the comments in `include/`. Start with `eip.h`. Each
port also has a header of its own: `ports/esp-idf/eip_esp_idf.h` and
`ports/clearcore/eip_clearcore.h`.

If you use a coding agent, tell it to read [AGENTS.md](AGENTS.md).

## License

MIT. See [LICENSE](LICENSE). Copyright (c) 2026 Adam G. Sweeney.
