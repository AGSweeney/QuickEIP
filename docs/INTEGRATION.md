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
- [10. Troubleshooting](#10-troubleshooting)

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
    cfg.revision_minor = 0;
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

How the adapter gets run depends on the port:

- **RTOS ports** (NetBurner, ESP-IDF, POSIX) start a task. Nothing else to do.
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
refuses a Forward_Open that doesn't match its assemblies. Keep them in
step:

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
| `link_info` | every 100 ms | Report link up, speed and duplex for the Ethernet Link object when the port can't read them (ESP-IDF). |
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
  reopen. Check `eip_session_is_open` after an error.

### 4.2 Discovery and symbols

```c
EipDevice found[16];
uint16_t n = 16;
eip_scan_devices(found, &n, 0);          /* broadcast on every interface, ~3 s */

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

**2. Add to your project's makefile** (also in `examples/netburner_adapter/makefile`):

```make
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
CFLAGS  += -std=gnu99
```

For NBEclipse, add the same files to the project and the two include
directories to both the C and C++ compiler settings.

**3. Start it from `UserMain`** after `init()` and `WaitForActiveNetwork()`,
as shown in section 3.

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

**1. Add the component.** The QuickEIP root directory is an IDF component.
Do one of the following:

- In your project's top-level `CMakeLists.txt`, before `project()`:
  ```cmake
  list(APPEND EXTRA_COMPONENT_DIRS "path/to/QuickEIP")
  ```
  The component takes the directory's name, for example `QuickEIP`, so in
  your main component use `REQUIRES QuickEIP`.
- With the component manager, in `main/idf_component.yml`:
  ```yaml
  dependencies:
    quickeip:
      path: ../../QuickEIP      # or: git: <url>
  ```
  and `REQUIRES quickeip`.

**2. Point it at your network interface and start it** after the netif has
been created:

```c
#include "eip.h"
#include "eip_esp_idf.h"

esp_netif_t *eth = /* your Ethernet netif */;
eip_esp_set_netif(&eth, 1);      /* optional: default is ETH_DEF, WIFI_STA_DEF, WIFI_AP_DEF */
eip_adapter_start(&cfg);
```

**3. Link speed (optional).** esp_netif doesn't report speed or duplex.
Read them from your `esp_eth_handle_t` instead:

```c
static esp_eth_handle_t s_eth;

static int link_info(void *ctx, uint32_t *mbps, int *full, int *autoneg)
{
    eth_speed_t speed;
    eth_duplex_t duplex;
    (void)ctx;
    if (esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed) != ESP_OK) return 0;
    esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
    *mbps = (speed == ETH_SPEED_100M) ? 100 : 10;
    *full = (duplex == ETH_DUPLEX_FULL);
    *autoneg = 1;
    return 1;   /* track link up/down from ETHERNET_EVENT_CONNECTED/DISCONNECTED if needed */
}
cfg.link_info = link_info;
```

**4. Options** go in compile definitions for every component, set before
`project()`:

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

**5. Things to know**

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

The worked example is `examples/esp_idf_adapter`. Run `idf.py menuconfig`
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

**1. Start from the ClearCore project template** (`ProjectTemplate` in
the ClearCore library install). It already references libClearCore and
LwIP.

**2. Add the files.** Use *Add → Existing Item → Add As Link* so the
files stay in your QuickEIP checkout:
- `src/*.c` (every file)
- `ports/lwip_raw/eip_hal_lwip_raw.c`
- `ports/clearcore/eip_hal_clearcore.cpp`

**3. Project properties → Toolchain:**
- *ARM/GNU C Compiler → Directories* and *ARM/GNU C++ Compiler →
  Directories*: add `QuickEIP/include` and `QuickEIP/src`.
- *ARM/GNU C Compiler → Miscellaneous*: add `-std=gnu99`.

**4. Main loop** (full example in `examples/clearcore_adapter/main.cpp`):

```cpp
EthernetMgr.Setup();
EthernetMgr.DhcpBegin();            // or EthernetMgr.LocalIp(IpAddress(...))
eip_adapter_start(&cfg);            // poll mode: starts no task

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
  session, and a scanner session takes another. On ClearCore, set
  `EIP_ADAPTER_MAX_CLIENTS` to 2 or 3. If you need more, rebuild the LwIP
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

**Arduino (ClearCore wrapper):** the same three file groups should work as
an Arduino library, but this hasn't been tested.

## 9. Linux, macOS, WSL

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

This builds `libquickeip.a` (core + POSIX port), the three tests, and
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
  scan uses first.
- Ports 44818 and 2222 are above 1024, so no root is needed. Another
  EtherNet/IP program on the same host will block them.
- Under WSL2 the host is behind NAT. Scanning works outward, but PLCs on
  your LAN can't reach a WSL adapter unless you use mirrored networking.

## 10. Troubleshooting

| Symptom | Likely cause |
|---|---|
| `listening` stays 0 | Another program holds TCP/UDP 44818 or UDP 2222, or the interface has no address yet. The server retries every second. |
| PLC shows the module faulted with code 16#0127 / 16#0128 | The EDS connection sizes don't match the assembly sizes. See 3.3. Remember the 4-byte run/idle header on O→T. |
| Connection times out every few seconds | O→T isn't arriving or is rejected. `rx_dropped_source` counts packets from the wrong address (NAT, multi-homed hosts). `rx_dropped_seq` counts duplicate or old sequence numbers. On Wi-Fi, check latency. |
| `opens` climbs and `timeouts` with it | Same as above. Also check that no firewall blocks UDP 2222. |
| Outputs read zero although the PLC sends data | The PLC is in Program mode (idle). That's by design. |
| Scanner `EIP_ERR_CIP` with 0x05 / 0x04 | Tag or path not found / malformed. Check the tag name and the slot (-1 vs a backplane slot). |
| Scanner `EIP_ERR_CIP` with 0x16 | Object doesn't exist (for example a wrong assembly instance). |
| Scanner `EIP_ERR_CIP` with 0x01 / 0x0204 on a routed session | The router timed out reaching the slot. Check the slot number, and that a module is there. |
| Scanner calls return `EIP_ERR_STATE` after an error | The session closed itself after a broken packet. Reopen it. |
| Scan finds nothing | Broadcasts don't cross routers or VLANs, so use `eip_list_identity` with an address instead. |
