# Integrating QuickEIP

This guide covers adding QuickEIP to a firmware project: the pieces, the
configuration, the adapter and scanner APIs, and step-by-step setup for each
supported platform.

- [1. The pieces](#1-the-pieces)
- [2. Build-time configuration](#2-build-time-configuration)
- [3. Running an adapter](#3-running-an-adapter)
- [4. Using the scanner](#4-using-the-scanner)
- [5. Threads, blocking and memory](#5-threads-blocking-and-memory)
- [6. NetBurner NNDK](#6-netburner-nndk)
- [7. ESP-IDF (ESP32)](#7-esp-idf-esp32)
- [8. Teknic ClearCore](#8-teknic-clearcore)
- [9. Linux, macOS, WSL](#9-linux-macos-wsl)
- [10. Windows](#10-windows)
- [11. Troubleshooting](#11-troubleshooting)

---

## 1. The pieces

Every build compiles three groups of files:

| Group | Files | Notes |
|---|---|---|
| Core | `src/eip_codec.c` `src/eip_adapter.c` `src/eip_session.c` `src/eip_io.c` `src/eip_scan.c` `src/eip_server.c` `src/eip_adapter_default.c` | Same on every platform. C99, no OS calls. |
| Port | one platform directory under `ports/` (plus a shared socket layer for some) | Implements `include/eip_hal.h`. |
| Include paths | `include/` (public) and `src/` (internal headers the core needs) | |

Each core file can be left out if you don't use it:

| File | Needed for |
|---|---|
| `eip_codec.c` | everything |
| `eip_adapter.c`, `eip_server.c` | adapter |
| `eip_adapter_default.c` | `eip_adapter_start()` and the other built-in-instance calls. Holds a static `EipServer` (~11 KB). |
| `eip_session.c` | scanner sessions, tags, explicit messages, symbols |
| `eip_io.c` | scanner Class 1 I/O (needs `eip_session.c`) |
| `eip_scan.c` | `eip_list_identity`, `eip_scan_devices` |

With `-ffunction-sections -fdata-sections -Wl,--gc-sections` the linker
drops whatever you don't call, so leaving every file in is normally fine.

## 2. Build-time configuration

Sizes live in `include/eip_config.h`. Each one is a `#ifndef` default.
Override them in a header of your own:

```c
/* my_eip_config.h */
#define EIP_ADAPTER_MAX_CLIENTS 2
#define EIP_ADAPTER_DATA_POOL   1024
```

Then build **every** file with `-DEIP_CONFIG_FILE=\"my_eip_config.h\"`.

> **Every file that includes `eip.h` must see the same values.** The settings
> change struct sizes. If the C files and your C++ application files are
> built with different flags, they disagree about the layout of
> `EipSession` or `EipServer` and corrupt memory. Put the definition in a
> flag that applies to both C and C++ (or use `EIP_CONFIG_FILE` in both).

| Setting | Default | Effect |
|---|---|---|
| `EIP_MAX_PACKET` | 1536 | Largest encapsulation packet, minimum 1024 (a smaller value is a build error). Sizes the session buffers (3×), the server's reply buffers (2×) and each adapter TCP client (1×). |
| `EIP_ADAPTER_MAX_ASSEMBLIES` | 8 | Assembly instances per adapter. |
| `EIP_ADAPTER_DATA_POOL` | 512 | Bytes shared by all assembly data. |
| `EIP_ADAPTER_MAX_CLIENTS` | 4 | Concurrent explicit-messaging (TCP) sessions. About `EIP_MAX_PACKET` bytes each. |
| `EIP_ADAPTER_EVICT_MS` | 60000 | When every TCP slot is busy, a session silent this long is dropped for a new client. The Class 1 owner's session is never dropped. |
| `EIP_ADAPTER_UDP_REPLY_MS` / `_BURST` | 20 / 20 | Rate limit for UDP 44818 replies (ListIdentity storms). |
| `EIP_ADAPTER_IDLE_WAIT_MS` | 5 | Longest wait in one `eip_server_service()` call. |
| `EIP_ADAPTER_TASK_STACK` / `_PRIORITY` | 0 (port default) | Adapter task stack and priority, in that RTOS's units. ESP-IDF uses both, NetBurner only the priority, POSIX neither. |

## 3. Running an adapter

### 3.1 Start it

```c
#include <string.h>
#include "eip.h"

static const EipAssemblyConfig kAssemblies[] = {
    /* instance, direction,        size, name,      initial, implicit_only */
    {150, EIP_ASM_CONSUMED, 32, "Outputs", 0, 0},   /* O->T: the PLC writes it  */
    {100, EIP_ASM_PRODUCED, 32, "Inputs",  0, 0},   /* T->O: we send it         */
    {151, EIP_ASM_CONFIG,    0, "Config",  0, 0},   /* config point of the path */
};

void start_eip(void)
{
    EipAdapterConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.vendor_id = 65000;            /* use your own ODVA vendor id */
    cfg.device_type = 43;             /* 43 generic device, 12 communications adapter */
    cfg.product_code = 1;
    cfg.revision_major = 1;
    cfg.revision_minor = 1;           /* an EDS needs 1 or higher */
    cfg.product_name = "My Device";   /* up to 31 characters */
    cfg.serial_number = 0;            /* 0: derived from the MAC */
    cfg.assemblies = kAssemblies;
    cfg.assembly_count = 3;
    eip_adapter_start(&cfg);
}
```

`eip_adapter_start` copies what it needs, so `cfg` and the assembly array
can live on the stack. The adapter listens on TCP 44818, UDP 44818 and UDP
2222. If the network isn't up yet, it retries opening its sockets once a
second. `EipAdapterView.listening` tells you when it's ready.

Instance numbers are yours to choose. 150, 100 and 151 are only the values
this guide uses. Passing `config = 0` selects a built-in test set with
different roles: 100 is consumed there, and 101 and 102 are produced.

How the adapter gets run depends on the port:

- **RTOS ports** (NetBurner, ESP-IDF, POSIX, Windows) start a task. Nothing else to do.
- **Poll-mode ports** (ClearCore, bare-metal lwIP) start nothing. Call
  `eip_adapter_poll()` from the main loop on every pass, at least every
  few milliseconds and more often than your fastest RPI.

### 3.2 Exchange data

```c
uint8_t out[32], in[32];
uint16_t len = sizeof out;
eip_adapter_read(150, out, &len);   /* latest data from the PLC          */
eip_adapter_write(100, in, 32);     /* sent on the next T->O packet      */
```

Both calls are safe from any task, and `write` needs the exact assembly
size. The consumed assembly reads as **all zeros** when:
- the connection timed out or closed, or
- the PLC is in Program mode (the run/idle header says idle).

Treat zero as your safe state. Before the first connection it reads as its
`initial` data, which is also zeros unless you set some.

`eip_adapter_snapshot(&view)` returns the following:
- identity and connection state
- the RPI and the inactivity timeout
- counters: packets in and out, opens, timeouts, and drops by cause (sequence, source, malformed)

These are what you want on a status page.

### 3.3 Assemblies and your EDS file

The PLC configures the connection from your EDS file, and the adapter
refuses a Forward_Open that doesn't match its assemblies.

**Assembly bytes are what you enter.** In an EDS file and in PLC software, the
connection size is the assembly size. The sequence count and run/idle header
are added on the wire, and they're what the adapter checks. For a 32-byte
assembly each way:

| Direction | Enter (EDS, PLC) | On the wire | Made up of |
|---|---|---|---|
| O→T | 32 | 38 | 2 sequence count + 4 run/idle header + 32 data |
| T→O | 32 | 34 | 2 sequence count + 32 data |

The run/idle header is a format flag in the EDS, not part of its size. The
table below uses wire sizes:

| Rule | Status if broken |
|---|---|
| The connection path names an existing consumed / produced / config instance | `0x01 / 0x012A` (consume), `0x012B` (produce), `0x0129` (config) |
| O→T size = 2 (sequence count) + 4 (if run/idle header) + assembly size | `0x01 / 0x0127` |
| T→O size = 2 + assembly size. T→O normally has no header. If the size also counts the 4-byte header, the adapter accepts that and sends a header that always says run. | `0x01 / 0x0128` |
| Config data in the path, if any, equals the config assembly size | `0x01 / 0x0126` |
| Electronic key, if the PLC checks it, matches vendor / device type / product code / revision. A zero in the key matches anything, including a minor revision of 0. | `0x01 / 0x0114`–`0x0116` |
| Another originator already owns the connection | `0x01 / 0x0106`, or `0x01 / 0x0113` if the request has no O→T |

A size-0 `EIP_ASM_CONFIG` assembly accepts a path with a config point and
no data, which is what most PLCs send. `implicit_only = 1` hides an
assembly from explicit Get/Set (status `0x16`) but still allows Class 1.

### 3.4 Optional callbacks

| Field | When it's called | Use |
|---|---|---|
| `link_info` | every 100 ms | Report link up, speed and duplex for the Ethernet Link object when the port can't read them (ESP-IDF). Return 1 while the link is up, and 0 to report it down. |
| `apply_interface` | ~150 ms after a scanner sets TCP/IP Interface attr 3, 5 or 6 (the reply has already gone out) | Store and apply the new IP settings. Leave it 0 to make those attributes read-only. |
| `reset` | ~300 ms after an Identity Reset request | Reboot (type 0) or restore factory settings and reboot (type 1). Leave it 0 to refuse the service. |
| `allow_change` | Before a TCP/IP Interface Set or an Identity Reset is accepted, while the request is being answered | Refuse the change, for example while machinery is moving: return a CIP general status (`0x10` device state conflict) and the requester gets that error. A refused request never reaches `apply_interface` or `reset`. Return 0 to allow. Runs under `eip_hal_lock`, so keep it short and don't call `eip_adapter_*` from it. |

All of them run in the adapter context (task or `eip_adapter_poll`) with
`callback_ctx` as their first argument.

### 3.5 More control: the EipServer API

`eip_server.h` exposes the instance behind `eip_adapter_*`:
- `eip_server_init`
- `eip_server_poll`
- `eip_server_service`
- `eip_server_read` / `eip_server_write` / `eip_server_snapshot`
- `eip_server_close`

Use it to:
- put the ~11 KB server in memory you choose,
- run the loop from your own task, or
- stop and restart the adapter.

`eip_adapter.h` is the sans-I/O core underneath, for people writing their
own event loop.

### 3.6 Connecting a PLC

Before you touch the PLC, check the adapter from a PC on the same network:

```
eiptool identity <adapter-ip>
eiptool get <adapter-ip> 4 100 3    # the produced assembly's data
```

You also need an ODVA vendor id. 65000 in this guide is a placeholder, and
real device names and EDS files should carry your own.

**EDS file.** `examples/eds/QuickEIP_Adapter.eds` matches the assemblies in
3.1. To adapt it:

| EDS field | Must equal |
|---|---|
| `VendCode`, `ProdType`, `ProdCode` | `vendor_id`, `device_type`, `product_code` |
| `MajRev`, `MinRev` | `revision_major`, `revision_minor`. EDS tools require 1–255, so don't use a minor revision of 0. |
| `ProdName` | `product_name` |
| `VendName`, `Catalog` | your company and part number (no matching config field) |
| `AssemNNN`, its size, and its member size in bits | an `EipAssemblyConfig` instance, `size`, and `size` × 8 |
| Connection sizes, `TSpec1`, `TSpec2` | the assembly sizes |
| `Number_Of_Static_Instances` under `[Assembly]` | `assembly_count` |
| Connection path | `20 04 24 <config> 2C <consumed> 2C <produced>`, instances in hex, for instances up to 255 |
| Assembly path | `20 04 24 <instance> 30 03`, instance in hex |

Leave the rest as it is. When an assembly layout changes, change `ProdCode` or
the revision too, so a PLC can't match an old EDS to the new device.

The sample's connection values have been checked by opening them against a
running adapter, but the file itself hasn't been imported into PLC software
yet.

**Micro850 (Connected Components Workbench).** Tested with the ClearCore port.
Add a generic EtherNet/IP device at the adapter's address, and enter its
assemblies: output (O→T), input (T→O) and config. For the assemblies in 3.1
that's instances 150, 100 and 151, with 32, 32 and 0 bytes. A Micro850 was seen
to send:

| Setting | Value |
|---|---|
| RPI | 50 ms each way |
| O→T | Point-to-point, with run/idle header |
| T→O | No header |
| Timeout multiplier | 0 (×4, so 200 ms at 50 ms RPI) |
| Electronic key | All zeros |
| Config | Names the config instance and sends no data |

The config instance must exist, or the open is refused with `0x01 / 0x0129`. A
size-0 `EIP_ASM_CONFIG` assembly is enough.

**CompactLogix and ControlLogix (Studio 5000).** Not yet tested with this
adapter. An EDS with these same connection values, and generic module settings
in this form, have worked on a CompactLogix with a different adapter.

To use the EDS, install it with the *EDS Hardware Installation Tool*, then add
the device as a module. Or add a *Generic Ethernet Module* with:

| Field | Value |
|---|---|
| Comm Format | Data - SINT |
| IP Address | The adapter's address |
| Input | Assembly Instance 100, Size 32 |
| Output | Assembly Instance 150, Size 32 |
| Configuration | Assembly Instance 151, Size 0 |

Then set the RPI on the *Connection* tab. Either way, the config assembly must
exist on the adapter, even at size 0, and comes first in the connection path.

**Adapter on a PC (POSIX port).** Not yet tested with a PLC. Open TCP and UDP
44818 and UDP 2222 in the firewall, and set `EIP_NETIF` on a host with several
interfaces, so that the adapter reports the address the PLC uses. Under WSL2,
use mirrored networking. A PC's timing is less even than an embedded target's,
so start with a longer RPI, such as 50 ms.

**Whichever PLC you use:**
- One Class 1 connection at a time. A second originator is refused.
- Keep the RPI well above your port's clock step. On NetBurner at 20 Hz, stay
  at 50 ms or more (see section 6).
- If the PLC reports a fault, look up the status in 3.3. `16#0127` and
  `16#0128` are size mismatches.
- Outputs read as zeros while the PLC is in Program mode.

## 4. Using the scanner

### 4.1 Sessions and tags

```c
static EipSession s;      /* ~4.7 KB with the default EIP_MAX_PACKET: static, not on the stack */

EipOpenConfig open = {0};
open.ip = "192.168.1.10";
open.slot = -1;           /* -1: CompactLogix, Micro800, any adapter.  0..n: ControlLogix slot */
open.timeout_ms = 2000;   /* 0 = 5000 */
open.vendor_id = 65000;   /* only used by Class 1 Forward_Open */

if (eip_session_open(&s, &open) == EIP_OK) {
    uint8_t buf[4];
    uint16_t len = sizeof buf, type = 0;
    uint8_t status = 0;

    eip_tag_read(&s, "Program:Main.Speed", 1, buf, &len, &type, &status);

    eip_store_le32(buf, 1234);
    eip_tag_write(&s, "Setpoint", EIP_DINT, 1, buf, 4, &status);

    len = sizeof buf;
    eip_get_attribute(&s, 0x01, 1, 1, buf, &len, &status);   /* Identity vendor id */

    eip_session_close(&s);
}
```

- Tag names use `.` for members and `[n]` or `[n,m]` for elements.
  Reads and writes larger than one packet are fragmented automatically.
- When a call returns `EIP_ERR_CIP`, `status` or `eip_last_cip_status()`
  hold the CIP general status, and `eip_last_cip_extended()` holds the
  extended status.
- With a `slot` of 0 or higher, the failure can come from the router rather
  than the target. For example, `0x01 / 0x0204` means the slot didn't answer.
- `eip_session_is_open(&s)` is safe on a zeroed, never-opened session.
- Close a session before opening it again.
- After `EIP_ERR_TIMEOUT` the session normally stays open, and the next
  request discards the late reply if one arrives. A packet cut short or too
  large closes the session, because the stream can't be resynchronized. The
  failing call returns its error, later calls return `EIP_ERR_STATE`, and you
  reopen.
- `eip_session_is_open` only says the session is registered here. It stays 1
  after the peer dies, so don't use it to detect a lost device. After
  `EIP_ERR_IO`, or when requests keep timing out, close the session and open it
  again.

### 4.2 Discovery and symbols

```c
EipDevice found[16];
uint16_t n = 16;
eip_scan_devices(found, &n, 0);          /* broadcast; ~0.5 s after the last reply, 3 s with none */

EipIdentity id;
eip_list_identity("192.168.1.10", &id, 0);

static EipSymbol tags[256];
uint16_t count = 256;
eip_list_symbols(&s, tags, &count);      /* Micro800: open with slot -1 */
```

`eip_device_kind()` sorts Rockwell parts into Micro800 / CompactLogix /
other by catalog name. It's a heuristic, so your application decides what
to do with the result.

### 4.3 Class 1 as an originator

```c
static EipIo io;
EipIoConfig ioc = {0};
ioc.config_instance = 151;
ioc.output_instance = 150;  ioc.output_bytes = 32;   /* we produce (O->T) */
ioc.input_instance  = 100;  ioc.input_bytes  = 32;   /* we consume (T->O) */
ioc.o2t_rpi_us = ioc.t2o_rpi_us = 10000;
ioc.run_idle = 1;
ioc.timeout_multiplier = 2;                          /* timeout = RPI x 16 */

if (eip_io_open(&s, &io, &ioc) == EIP_OK) {
    for (;;) {                     /* every RPI */
        eip_io_produce(&io, out, 32, 1 /* run */);
        uint16_t in_len = 32;
        eip_io_consume(&io, in, &in_len, 100);
    }
    eip_io_close(&s, &io);
}
```

The originator binds UDP 2223–2231 (unless `local_port` is set), so it
can share a device with the adapter on 2222. Pacing is up to you: call
produce once per RPI.

Open Class 1 on a session with `slot = -1`. On a routed session the
Forward_Open itself goes through Unconnected Send, and that combination
hasn't been tested.

- **Address the adapter at the IP it reports,** the one a scan shows. It tells
  the originator to send to that address, and accepts O→T only from the address
  the Forward_Open came from. Any other address on the same host, such as
  `127.0.0.1`, makes those differ, so packets are dropped (`rx_dropped_source`)
  and the connection times out. Explicit messages aren't affected. On a POSIX
  host with several addresses, `EIP_NETIF` picks the one the adapter reports.
- **Detecting a lost connection:** `eip_io_produce` is UDP, so it keeps
  succeeding. `eip_io_consume` returns `EIP_ERR_TIMEOUT` when no T→O packet
  arrives in `timeout_ms`. Treat a run of timeouts as lost. To recover, do all
  four steps, because reopening I/O on the old session fails with `EIP_ERR_IO`:
  `eip_io_close`, `eip_session_close`, `eip_session_open`, `eip_io_open`.
  `eip_io_close` can block for the session timeout and return an error when the
  peer is gone. That's expected, and it still releases the UDP socket.
- **Pacing with a blocking consume:** `eip_io_consume` waits, so a sample loop
  like the one above runs at the T→O rate. There's no non-blocking form, and
  `timeout_ms` 0 means the 1-second default. Use a short timeout, or separate
  tasks for produce and consume.
- **Config instance:** use the value from the target's EDS. `config_instance
  = 0` still sends instance 0 in the connection path. QuickEIP's own adapter
  accepts that, but another target may refuse it.

## 5. Threads, blocking and memory

- **Scanner calls block** until they finish or time out. On an RTOS, call
  them from a task of their own. On bare metal they keep the network stack
  running while they wait, and the adapter's sockets still receive
  packets, but the rest of your main loop stops until the call returns.
- One task at a time per `EipSession` / `EipIo`. Separate sessions can run
  in separate tasks.
- The adapter's read / write / snapshot calls can come from any task. They
  take `eip_hal_lock`, which is held for microseconds, never across a
  socket call.
- **RAM** with the defaults:
  - the built-in adapter: about 11 KB of static memory, plus 1.5 KB of shared reply buffer
  - each `EipSession`: about 4.7 KB
  - each `EipIo`: 100 bytes, plus a 1.5 KB stack buffer during produce/consume
  - scanner calls: up to about 2 KB of caller stack (tag write builds 400-byte requests)
- **Code**: about 24 KB at `-Os` on Cortex-M4 for everything.

## 6. NetBurner NNDK

Tested with NNDK 3.5.8 (ARM platforms). The NNDK 3.x socket API is the
same across platforms.

**1. Put QuickEIP inside the project.** Use a subfolder or a git
submodule, for example `MyProject/QuickEIP`. The NNDK rules put each object
at `obj/release/<source path>`, so a source path that starts with `../`
puts the objects outside `obj/`, next to your sources. The in-repo example
has to use `../..` and shows this.

**2. Write the project's makefile.** This is a complete one, with QuickEIP in
a `QuickEIP` subfolder:

```make
NAME    = MyProject

CPP_SRC += src/main.cpp

QUICKEIP = QuickEIP

C_SRC   += $(QUICKEIP)/src/eip_codec.c \
           $(QUICKEIP)/src/eip_adapter.c \
           $(QUICKEIP)/src/eip_session.c \
           $(QUICKEIP)/src/eip_io.c \
           $(QUICKEIP)/src/eip_scan.c \
           $(QUICKEIP)/src/eip_server.c \
           $(QUICKEIP)/src/eip_adapter_default.c
CPP_SRC += $(QUICKEIP)/ports/netburner/eip_hal_netburner.cpp
NBINCLUDE += -I$(QUICKEIP)/include -I$(QUICKEIP)/src

include $(NNDK_ROOT)/make/boilerplate.mk
```

Build with the NNDK environment set, for example:

```sh
export NNDK_ROOT=C:/nburn PLATFORM=MODM7AE70 DEFPLATFORM=MODM7AE70
export PATH=/c/nburn/gcc/bin:/c/nburn/pcbin:/c/nburn/gcc/msys/1.0/bin:$PATH
make
```

The image is `obj/release/MyProject.bin`. The first build is slow, several
minutes, because the NNDK compiles its system library into the project's `obj/`
folder. That's normal. For NBEclipse, add the same files to the project and
the two include directories to both the C and C++ compiler settings.

**3. Start it from `UserMain`.** Scanner calls block, so run them in a task of
their own. Each task needs its own priority, and a lower number runs first.
The adapter takes `MAIN_PRIO - 1`, so `MAIN_PRIO + 1` is a safe choice, and
`UserMain` must then sleep so that the task can run:

```cpp
#include <init.h>
#include <nbrtos.h>
#include <string.h>

#include "eip.h"

const char *AppName = "MyProject";

static void ScannerTask(void *)
{
    for (;;) {
        /* eip_session_open, eip_tag_read and so on, from section 4 */
        OSTimeDly(TICKS_PER_SECOND);
    }
}

void UserMain(void *)
{
    EipAdapterConfig cfg;

    init();
    WaitForActiveNetwork(TICKS_PER_SECOND * 10);

    memset(&cfg, 0, sizeof cfg);
    /* fill in cfg as in section 3 */
    eip_adapter_start(&cfg);
    OSSimpleTaskCreatewName(ScannerTask, MAIN_PRIO + 1, "EipScanner");

    for (;;) {
        /* eip_adapter_read and eip_adapter_write */
        OSTimeDly(1);
    }
}
```

**4. Things to know**

- **Clock resolution.** `eip_hal_millis` follows `TimeTick`: 50 ms at the
  default 20 Hz. The adapter never sets a Class 1 timeout shorter than two
  ticks (100 ms), and T→O packets go out on tick boundaries. For RPIs under
  about 50 ms, raise `TICKS_PER_SECOND` in your platform configuration.
- **Task.** The adapter task runs at `MAIN_PRIO - 1` by default. Set
  `EIP_ADAPTER_TASK_PRIORITY` to change it. The task uses NNDK's default
  task stack.
- **Locking.** `eip_hal_lock` is `OSLock()`, which pauses task switching
  briefly. It's never held across a socket call.
- **UDP sockets** are `CreateRxTxUdpSocket4` with an explicit local port.
  When the library asks for "any port", the port picks one in
  50000–50063.
- **WebUI.** `examples/netburner_webui` is the earlier test web page,
  updated for the new API. It's application code, not part of the stack.

## 7. ESP-IDF (ESP32)

Tested with ESP-IDF 5.5.1 on the esp32 target. Any ESP-IDF 5.x should work.

**1. Add the component.** The QuickEIP root directory is an IDF component,
and it takes the name of its directory. Use the same spelling everywhere.
This is the simpler route, with a complete top-level `CMakeLists.txt`. The path
is relative to the project, here with QuickEIP beside it:

```cmake
cmake_minimum_required(VERSION 3.16)
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../QuickEIP")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(myapp)
```

With the component manager instead, in `main/idf_component.yml`, the
dependency key **must match the directory name**. The path is relative to
`main/`:

```yaml
dependencies:
  QuickEIP:
    path: ../../QuickEIP
```

A mismatch fails with `Failed to resolve component ... unknown name`, and
changing `REQUIRES` doesn't fix it.

Either way, `main/CMakeLists.txt` names the component and the network
components you use. Build with `idf.py set-target esp32`, then `idf.py build`.

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES QuickEIP esp_eth esp_netif esp_event)
```

**2. Bring up Ethernet.** Follow IDF's `examples/ethernet/basic` for the
driver. **You need your board's PHY, pins and RMII clock mode.** A wrong value
builds fine, but the link never comes up. For the ESP32's internal EMAC with a
LAN87xx PHY:

```c
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_netif.h"

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static volatile int s_link_up;

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) s_link_up = 1;
    else if (id == ETHERNET_EVENT_DISCONNECTED) s_link_up = 0;
}

static void eth_start(void)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG(); /* set smi_gpio and clock_config */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();                /* and your PHY address */
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();

    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &s_eth));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL));
    ESP_ERROR_CHECK(esp_eth_start(s_eth));
}
```

**3. Report the link (optional).** esp_netif doesn't report speed or duplex,
so read them from the driver. Return 1 while the link is up, and 0 to report
it down:

```c
static int link_info(void *ctx, uint32_t *mbps, int *full, int *autoneg)
{
    eth_speed_t speed;
    eth_duplex_t duplex;
    (void)ctx;
    if (!s_link_up) return 0;
    if (esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed) != ESP_OK) return 0;
    if (esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex) != ESP_OK) return 0;
    *mbps = (speed == ETH_SPEED_100M) ? 100 : 10;
    *full = (duplex == ETH_DUPLEX_FULL);
    *autoneg = 1;
    return 1;
}
```

**4. Start the adapter.** Set every `cfg` field first, because
`eip_adapter_start` copies them:

```c
#include "eip.h"
#include "eip_esp_idf.h"

eth_start();
cfg.link_info = link_info;       /* with the rest of cfg, from section 3 */
eip_esp_set_netif(&s_netif, 1);  /* optional: default is ETH_DEF, WIFI_STA_DEF, WIFI_AP_DEF */
eip_adapter_start(&cfg);
```

`eip_esp_set_netif` is documented in `ports/esp-idf/eip_esp_idf.h`. Call it
before `eip_adapter_start`. It copies the list, and the netifs must outlive
the adapter.

**5. Options** go in compile definitions for every component, set after
`include(.../project.cmake)` and before `project()`:

```cmake
idf_build_set_property(COMPILE_DEFINITIONS "EIP_ESP_TASK_PRIORITY=12" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "EIP_ESP_TASK_WDT=1" APPEND)
```

| Define | Default | Effect |
|---|---|---|
| `EIP_ESP_TASK_STACK` | 6144 | Adapter task stack in bytes (or `EIP_ADAPTER_TASK_STACK`). |
| `EIP_ESP_TASK_PRIORITY` | 12 | Adapter task priority (or `EIP_ADAPTER_TASK_PRIORITY`). |
| `EIP_ESP_TASK_CORE` | `tskNO_AFFINITY` | Pin the adapter task to a core. |
| `EIP_ESP_TASK_WDT` | 0 | 1 adds the adapter task to the task watchdog. |

**6. Things to know**

- Use **wired Ethernet** for Class 1 in production. Wi-Fi latency spikes
  regularly exceed an 8 × RPI timeout.
- The adapter task waits in `select()`, so it wakes as soon as a packet
  arrives. While idle it still runs a pass at least every
  `EIP_ADAPTER_IDLE_WAIT_MS` (5 ms).
- **Arduino-ESP32** runs on ESP-IDF and can use the same files. Make a
  library folder whose `src/` contains `include/*`, `src/*`,
  `ports/bsd_sockets/*` and `ports/esp-idf/*`. Leave out the other ports,
  because Arduino compiles every file it finds. This setup hasn't been
  tested.

The worked example is `examples/esp_idf_adapter`. It uses IDF's
`protocol_examples_common`, so for that example only, run `idf.py menuconfig`
and set up the network under *Example Connection Configuration*.

## 8. Teknic ClearCore

`examples/clearcore_adapter` compiles and links with Microchip Studio's
arm-gcc 6.3 against the Enhanced ClearCore Library, a fork of ClearCore
library 1.7. The adapter has run on a ClearCore with a Micro850 PLC. The
example itself hasn't been run.

ClearCore runs bare metal (no RTOS) on lwIP's raw API, with `NO_SYS=1`.
QuickEIP talks to lwIP directly through `ports/lwip_raw`, not through the
`EthernetTcp*` / `EthernetUdp` classes. Those classes have a 600-byte TCP
buffer and no broadcast send, and EtherNet/IP needs more than that.

**1. Start from the ClearCore project template.** Copy `ProjectTemplate`,
`libClearCore` and `LwIP` from the ClearCore library folder somewhere writable,
and put your QuickEIP checkout beside them. All four must be siblings, because
the paths below are relative. Open `ProjectTemplate.atsln` and check that all
three projects load.

QuickEIP uses only standard ClearCore and lwIP calls, and compiles against
stock library 1.7. Linking has been done with the Enhanced fork.

**2. Add the files.** Use *Add → Existing Item → Add As Link* so the
files stay in your QuickEIP checkout:
- `src/*.c` (every file)
- `ports/lwip_raw/eip_hal_lwip_raw.c`
- `ports/clearcore/eip_hal_clearcore.cpp`

**3. Project properties → Toolchain.** Paths are relative to the `Debug` or
`Release` output folder. Make these changes in both configurations:
- *ARM/GNU C Compiler → Directories* and *ARM/GNU C++ Compiler →
  Directories*: add `../../QuickEIP/include` and `../../QuickEIP/src`.
- *ARM/GNU C Compiler → Directories* only: also add
  `../../LwIP/LwIP/src/include` and `../../LwIP/LwIP/port/include`. The template
  sets these for C++ only, and the lwIP port is a C file, so without them the
  build stops at `lwip/ip_addr.h`.
- *ARM/GNU C Compiler → Symbols* and *ARM/GNU C++ Compiler → Symbols*: add
  `EIP_ADAPTER_MAX_CLIENTS=2`. It must be in both, because it changes struct
  sizes, and the default of 4 is too many for this lwIP build. Keep the
  existing `DEBUG` and `NDEBUG` symbols.

Leave the template's `-std=gnu11` as it is.

To check a compile without Studio, run `arm-none-eabi-gcc` or
`arm-none-eabi-g++` with `-c` from `ProjectTemplate/Debug` and these flags. The
toolchain is under `Atmel/Studio/7.0/toolchain/arm/arm-gnu-toolchain/bin`, and
`<Packs>` is `Atmel/Studio/7.0/Packs`:

```
-mthumb -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16
-D__SAME53N19A__ -DEIP_ADAPTER_MAX_CLIENTS=2
-I../../QuickEIP/include -I../../QuickEIP/src -I../../QuickEIP/ports/clearcore
-I../../libClearCore/inc -I../../LwIP/LwIP/src/include -I../../LwIP/LwIP/port/include
-I<Packs>/atmel/SAME53_DFP/1.1.118/include
-I<Packs>/arm/CMSIS/4.5.0/CMSIS/Include
-I<Packs>/arm/CMSIS/5.4.0/CMSIS/Core/Include
```

**4. Main loop.** Replace the template's `main.cpp`. The full example is in
`examples/clearcore_adapter/main.cpp`:

```cpp
#include "ClearCore.h"
#include "eip.h"

while (!EthernetMgr.PhyLinkActive()) {                 // wait for the cable
    Delay_ms(100);
}
EthernetMgr.Setup();
if (!EthernetMgr.DhcpBegin()) {
    EthernetMgr.LocalIp(IpAddress(192, 168, 1, 50));   // fallback static address
}
if (eip_adapter_start(&cfg) != EIP_OK) {               // poll mode: starts no task
    while (true) { EthernetMgr.Refresh(); }            // stop: outputs stay off
}

while (true) {
    eip_adapter_poll();             // every pass
    EthernetMgr.Refresh();
    // motion, I/O, eip_adapter_read / eip_adapter_write ...
}
```

**5. Things to know**

- **Call `eip_adapter_poll()` often.** T→O packets go out from inside
  poll, so loop timing turns into RPI jitter. Keep the loop well under
  your RPI.
- **The lwIP build is small:** a 4 KB heap (`MEM_SIZE`), 5 TCP PCBs and 8
  listening PCBs. The adapter uses 1 listening PCB plus 1 per explicit
  session, and a scanner session takes another, which is why step 3 sets
  `EIP_ADAPTER_MAX_CLIENTS`. Use 2 or 3. If you need more, rebuild the LwIP
  project with larger `MEM_SIZE` / `MEMP_NUM_TCP_PCB` (its `lwipopts.h`
  uses `#ifndef`, so `-D` works).
- **Blocking scanner calls** keep calling `EthernetMgr.Refresh()` while they
  wait, and motion keeps running from interrupts. Your main loop doesn't
  run until the call returns, so keep `timeout_ms` short.
- **Interrupts.** Don't call QuickEIP from an interrupt handler. lwIP
  callbacks run inside `EthernetMgr.Refresh()` in the main context, which
  is why the port needs no lock.
- **Socket table.** `EIP_LWIP_MAX_SOCKETS` (12), `EIP_LWIP_UDP_QUEUE` (8)
  and `EIP_LWIP_ACCEPT_QUEUE` (4) size the raw-API socket table.
- **Link reporting.** The port reports the link as 100 Mbit/s full duplex
  whenever the PHY link is up.
- **Multicast T→O.** The port sets TTL 1 only in lwIP builds with
  `LWIP_MULTICAST_TX_OPTIONS`, and leaves the interface to lwIP's routing. The
  stock build doesn't have that option, so prefer a point-to-point connection.

**Arduino (ClearCore wrapper):** the same three file groups should work as
an Arduino library, but this hasn't been tested.

## 9. Linux, macOS, WSL

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

This builds `libquickeip.a` (core + POSIX port), the tests, and
`eiptool`:

```
eiptool scan
eiptool identity 192.168.1.10
eiptool read  192.168.1.10 MyDint            # slot -1; append a slot number for ControlLogix
eiptool write 192.168.1.10 MyDint dint 42
eiptool tags  192.168.1.10
eiptool get   192.168.1.10 0x01 1 7          # class instance attribute
eiptool adapter                              # run the test adapter
```

- Set `EIP_NETIF=eth1` to choose the interface the adapter reports and the
  scan uses first. It applies to any program using the POSIX port, not only
  `eiptool`. An unknown name is ignored, and the default interface is used.
- Ports 44818 and 2222 are above 1024, so no root is needed. Another
  EtherNet/IP program on the same host will block them.
- Under WSL2 the host is behind NAT. Scanning works outward, but PLCs on
  your LAN can't reach a WSL adapter unless you use mirrored networking.

### Using QuickEIP from your own CMake project

```cmake
cmake_minimum_required(VERSION 3.13)
project(myapp C)

set(QUICKEIP_BUILD_TESTS OFF CACHE BOOL "")
set(QUICKEIP_BUILD_TOOLS OFF CACHE BOOL "")
add_subdirectory(../QuickEIP quickeip)

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE quickeip)
```

Linking `quickeip` brings in the include path and threads. Without the two
`OFF` lines the tests and `eiptool` also build, and `eiptool` lands in
`build/quickeip/`.

## 10. Windows

Tested with Visual Studio 2022 on Windows 11, as x64 and Win32. The port is
`ports/windows/eip_hal_windows.c`, and it needs Windows 7 or later.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build/Release/eiptool scan
```

`eiptool` takes the same commands as in section 9.

**In your own project,** compile `src/*.c` and
`ports/windows/eip_hal_windows.c`, add `include/` and `src/` to the include
path, and link `ws2_32`, `iphlpapi` and `winmm`.

| Define | Default | Effect |
|---|---|---|
| `EIP_WIN_POLL_MODE` | off | Start no adapter thread. Your loop calls `eip_adapter_poll()`. |
| `EIP_ADAPTER_TASK_PRIORITY` | 0 | A `THREAD_PRIORITY_*` value for the adapter thread. 0 picks `THREAD_PRIORITY_HIGHEST`. |
| `EIP_ADAPTER_TASK_STACK` | 0 | Adapter thread stack in bytes. 0 is the system default. |
| `EIP_WIN_MAX_SOCKETS` | 64 | Sockets open at once. |
| `EIP_WIN_MAX_INTERFACES` | 16 | Interfaces reported. |
| `EIP_WIN_NETIF_REFRESH_MS` | 2000 | How often the interface list is re-read, off the adapter thread. |

**Things to know**

- **Choose the interface on a PC with several.** The adapter reports interface
  0 as its address, in ListIdentity and in the O→T socket address of a
  Forward_Open. Interface 0 is the first one that's up and has a default
  gateway, which is often not the machine network. Set `EIP_NETIF` to the
  adapter's friendly name or IPv4 address: `EIP_NETIF="Ethernet 2"` or
  `EIP_NETIF=10.168.41.1`. A named interface is kept even while its cable is
  out, and reports link down. Scanner calls don't need this: they use whichever
  interface routes to the target.
- **Firewall.** An adapter needs inbound TCP 44818, UDP 44818 and UDP 2222
  allowed for your program. A scanner running Class 1 needs inbound UDP on
  its local port. Check which profile the network is in with
  `Get-NetConnectionProfile`.
- **A new `.exe` can lose its first replies.** Third-party security software
  may hold back inbound UDP for the first seconds a program it hasn't seen
  runs, even with Windows Firewall off. On a test PC with AVG, every freshly
  built binary failed its first `eiptool identity` (once in two copies, twice
  in a third) and answered in about 45 ms from then on. If a new build times
  out or a scan comes back short, run it again before looking for a network
  fault.
- **Busy ports.** Other EtherNet/IP software on the PC (RSLinx, for example)
  may already hold 44818 or 2222. Every bind here is exclusive, so the adapter
  then fails to open them and `listening` stays 0 rather than sharing the port.
- **Timing.** The port sets a 1 ms system timer period for the process from its
  first call, which costs some power on a laptop. On the test PC a 10 ms RPI
  ran at a mean of 10.00 ms, worst 11.83 ms over 1,500 packets, with both ends
  on the one host.
- **Windows isn't real time.** A stall of about 100 ms has been measured on a
  bench PC. Keep the inactivity timeout well above that: set `timeout_floor_ms`
  in `EipAdapterConfig` to 500 or more. Don't put a safety function on this.
- **Link reporting.** Speed comes from Windows. Duplex and autonegotiation
  aren't exposed, so the port reports full duplex and autonegotiated while the
  link is up.
- **Untested:** multicast T→O, and a real PLC connecting to a Windows adapter.

## 11. Troubleshooting

| Symptom | Likely cause |
|---|---|
| `listening` stays 0 | Another program holds TCP/UDP 44818 or UDP 2222, or the interface has no address yet. The server retries every second. |
| PLC shows the module faulted with code 16#0127 / 16#0128 | The EDS connection sizes don't match the assembly sizes. See 3.3. Remember the 4-byte run/idle header on O→T. |
| Class 1 opens, then dies after a fraction of a second, and `rx_dropped_source` climbs | Address the adapter at the IP it reports, not `127.0.0.1` or another address on the host. See 4.3. |
| `eip_io_open` returns `EIP_ERR_IO` after a lost connection | Close and reopen the session as well. See 4.3. |
| ESP-IDF: `Failed to resolve component ... unknown name` | The `idf_component.yml` key doesn't match the QuickEIP directory name. See section 7. |
| ClearCore: `lwip/ip_addr.h: No such file or directory` | The C compiler lacks the LwIP paths. See section 8, step 3. |
| Connection times out every few seconds | O→T isn't arriving or is rejected. `rx_dropped_source` counts packets from the wrong address (NAT, multi-homed hosts). `rx_dropped_seq` counts duplicate or old sequence numbers. On Wi-Fi, check latency. |
| `opens` climbs and `timeouts` with it | Same as above. Also check that no firewall blocks UDP 2222 or TCP and UDP 44818. |
| Outputs read zero although the PLC sends data | The PLC is in Program mode (idle). That's by design. |
| Scanner `EIP_ERR_CIP` with 0x05 / 0x04 | Tag or path not found / malformed. Check the tag name and the slot (-1 vs a backplane slot). |
| Scanner `EIP_ERR_CIP` with 0x16 | Object doesn't exist (for example a wrong assembly instance). |
| Scanner `EIP_ERR_CIP` with 0x01 / 0x0204 on a routed session | The router timed out reaching the slot. Check the slot number, and that a module is there. |
| Scanner calls return `EIP_ERR_STATE` after an error | The session closed itself after a broken packet. Reopen it. |
| Scan finds nothing | Broadcasts don't cross routers or VLANs, so use `eip_list_identity` with an address instead. On Windows, also run a newly built program a second time: see section 10. |
