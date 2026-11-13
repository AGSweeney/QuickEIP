# Integrating QuickEIP

This guide tells you how to add QuickEIP to a firmware project. It covers the
file groups, the configuration, the adapter API, the scanner API, and the
setup procedure for each platform.

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

> **WARNING:** QuickEIP does not implement CIP Safety. Do not use QuickEIP as
> a safety function.

---

## 1. The pieces

Each build compiles the three file groups in this table.

| Group | Files | Notes |
|---|---|---|
| Core | `src/eip_codec.c` `src/eip_adapter.c` `src/eip_session.c` `src/eip_io.c` `src/eip_scan.c` `src/eip_server.c` `src/eip_adapter_default.c` | The same on all platforms. C99, with no OS calls. |
| Port | One platform directory in `ports/`. Some platforms also use a shared socket layer. | Implements `include/eip_hal.h`. |
| Include paths | `include/` (public) and `src/` (internal headers that the core needs) | |

You can omit a core file that you do not use. The table shows what each file
is necessary for.

| File | Necessary for |
|---|---|
| `eip_codec.c` | All functions |
| `eip_adapter.c`, `eip_server.c` | The adapter |
| `eip_adapter_default.c` | `eip_adapter_start()` and the other calls for the built-in instance. It contains a static `EipServer` (approximately 11 KB). |
| `eip_session.c` | Scanner sessions, tags, explicit messages, and symbols |
| `eip_io.c` | Scanner Class 1 I/O. It needs `eip_session.c`. |
| `eip_scan.c` | `eip_list_identity` and `eip_scan_devices` |

With `-ffunction-sections -fdata-sections -Wl,--gc-sections`, the linker
removes the functions that you do not call. Thus, you can usually keep all
files in the build.

## 2. Build-time configuration

The sizes are in `include/eip_config.h`. Each size is a `#ifndef` default. To
change sizes, put the new values in a header of your own:

```c
/* my_eip_config.h */
#define EIP_ADAPTER_MAX_CLIENTS 2
#define EIP_ADAPTER_DATA_POOL   1024
```

Then build **all** files with `-DEIP_CONFIG_FILE=\"my_eip_config.h\"`.

> **CAUTION:** Each file that includes `eip.h` must see the same values. The
> settings change the sizes of structures. If the C files and your C++ files
> use different flags, the layouts of `EipSession` or `EipServer` are
> different, and memory becomes corrupt. Put the definition in a flag that
> applies to C and to C++, or use `EIP_CONFIG_FILE` for the two languages.

The table lists the settings.

| Setting | Default | Effect |
|---|---|---|
| `EIP_MAX_PACKET` | 1536 | The largest encapsulation packet. The minimum is 1024, and a smaller value is a build error. It sets the size of the session buffers (3×), the reply buffers of the server (2×), and each adapter TCP client (1×). |
| `EIP_ADAPTER_MAX_ASSEMBLIES` | 8 | The number of assembly instances for each adapter. |
| `EIP_ADAPTER_DATA_POOL` | 512 | The number of bytes that all assembly data shares. |
| `EIP_ADAPTER_MAX_CLIENTS` | 4 | The number of explicit-messaging (TCP) sessions at the same time. Each one uses approximately `EIP_MAX_PACKET` bytes. |
| `EIP_ADAPTER_EVICT_MS` | 60000 | When all TCP slots are in use, the adapter closes a session that was silent for this time, to accept a new client. The adapter never closes the session of the Class 1 owner. |
| `EIP_ADAPTER_UDP_REPLY_MS` / `_BURST` | 20 / 20 | The rate limit for replies on UDP port 44818. It protects the adapter from a large number of ListIdentity requests. |
| `EIP_ADAPTER_IDLE_WAIT_MS` | 5 | The longest wait in one `eip_server_service()` call. |
| `EIP_ADAPTER_TASK_STACK` / `_PRIORITY` | 0 (port default) | The stack and the priority of the adapter task, in the units of the RTOS. ESP-IDF uses the two values. NetBurner uses only the priority. POSIX uses neither. |

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

`eip_adapter_start` copies the data that it needs. Thus, `cfg` and the
assembly array can be on the stack.

The adapter listens on TCP port 44818, UDP port 44818, and UDP port 2222. If
the network is not ready, the adapter tries to open its sockets again each
second. `EipAdapterView.listening` tells you when the adapter is ready.

You select the instance numbers. This guide uses 150, 100, and 151 as
examples only. If you pass `config = 0`, the adapter uses a built-in test set
with different functions. In that set, 100 is consumed, and 101 and 102 are
produced.

The port controls how the adapter runs:

- **Task-mode ports** (NetBurner, ESP-IDF, POSIX, Windows) start a task. You
  do nothing more.
- **Poll-mode ports** (ClearCore, bare-metal lwIP) start no task. Call
  `eip_adapter_poll()` from the main loop on each pass. Call it at intervals
  of a few milliseconds or less, and at a higher rate than your fastest
  requested packet interval (RPI).

### 3.2 Exchange data

```c
uint8_t out[32], in[32];
uint16_t len = sizeof out;
eip_adapter_read(150, out, &len);   /* latest data from the PLC          */
eip_adapter_write(100, in, 32);     /* sent on the next T->O packet      */
```

You can make these two calls from any task. `eip_adapter_write` needs the
exact assembly size.

> **WARNING:** The consumed assembly reads as all zeros in the two conditions
> that follow. Make sure that all zeros is a safe state for your machinery.

The consumed assembly reads as **all zeros** in these conditions:

- The connection timed out or closed.
- The PLC is in Program mode. The run/idle header says idle.

Before the first connection, the consumed assembly reads as its `initial`
data. That data is also all zeros, unless you set other values.

`eip_adapter_snapshot(&view)` returns this data:

- The identity and the connection state.
- The RPI and the inactivity timeout.
- The counters: packets in and out, opens, timeouts, and dropped packets by
  cause (sequence, source, or malformed).

This data is applicable to a status page.

### 3.3 Assemblies and your EDS file

The PLC configures the connection from your EDS file. The adapter refuses a
Forward_Open that does not agree with its assemblies.

**You enter the assembly size.** In an EDS file and in PLC software, the
connection size is the assembly size. The wire size adds the sequence count
and the run/idle header, and the adapter examines the wire size. The table
shows a 32-byte assembly in each direction.

| Direction | Assembly size (EDS, PLC) | Wire size | Contents |
|---|---|---|---|
| O→T | 32 | 38 | 2 bytes of sequence count + 4 bytes of run/idle header + 32 bytes of data |
| T→O | 32 | 34 | 2 bytes of sequence count + 32 bytes of data |

In the EDS file, the run/idle header is a format flag. It is not a part of
the size. The next table gives the rules for a Forward_Open, in wire sizes.

| Rule | Status if the rule is broken |
|---|---|
| The connection path names a consumed instance, a produced instance, and a config instance that exist. | `0x01` / `0x012A` (consumed), `0x012B` (produced), `0x0129` (config) |
| The O→T size is 2 (sequence count) + 4 (if there is a run/idle header) + the assembly size. | `0x01` / `0x0127` |
| The T→O size is 2 + the assembly size. T→O usually has no header. If the size also includes the 4-byte header, the adapter accepts it and sends a header that always says run. | `0x01` / `0x0128` |
| If the path contains config data, its size is equal to the size of the config assembly. | `0x01` / `0x0126` |
| If the PLC sends an electronic key, the key agrees with the vendor ID, the device type, the product code, and the revision. A zero in the key agrees with all values, and also with a minor revision of 0. | `0x01` / `0x0114`–`0x0116` |
| No other originator owns the connection. | `0x01` / `0x0106`, or `0x01` / `0x0113` if the request has no O→T |

An `EIP_ASM_CONFIG` assembly of size 0 accepts a path with a config point and
no data. Most PLCs send that path. `implicit_only = 1` hides an assembly from
explicit Get and Set requests (status `0x16`). Class 1 can continue to use
the assembly.

### 3.4 Optional callbacks

> **WARNING:** A scanner can change the IP settings or reset the adapter
> while machinery moves. Use `allow_change` to refuse these requests when a
> change is not safe.

The table lists the callbacks in `EipAdapterConfig`.

| Field | When the adapter calls it | Use |
|---|---|---|
| `link_info` | Each 100 ms | Report the link state, the speed, and the duplex mode for the Ethernet Link object, when the port cannot read them (ESP-IDF). Return 1 while the link is up. Return 0 to report link down. |
| `apply_interface` | Approximately 150 ms after a scanner sets TCP/IP Interface attribute 3, 5, or 6. The adapter sent the reply before the call. | Store and apply the new IP settings. If the field is 0, those attributes are read-only. |
| `reset` | Approximately 300 ms after an Identity Reset request | Restart (type 0), or go back to the factory settings and restart (type 1). If the field is 0, the adapter refuses the service. |
| `allow_change` | Before the adapter accepts a TCP/IP Interface Set or an Identity Reset, while it prepares the reply | Refuse the change, for example while machinery moves. Return a CIP general status (`0x10`, device state conflict), and the requester gets that error. A refused request does not go to `apply_interface` or `reset`. Return 0 to permit the change. |
| `on_event` | When the Class 1 connection opens, closes, or times out, and when it changes between run and idle | Start or stop your machine logic without a poll of `eip_adapter_snapshot`. |
| `on_leds` | One time after the start, and then when an indicator state changes | Set the module status and network status indicators of your device. |
| `store_nv` | After a scanner changes a stored setting | Write the `EipNvSettings` structure to non-volatile memory. If the field is 0, the stored settings are read-only. |

These rules apply to the callbacks:

- All callbacks run in the adapter context (the task or `eip_adapter_poll`).
- The first argument of each callback is `callback_ctx`.
- `allow_change` runs while the adapter holds `eip_hal_lock`. Keep it short,
  and do not call `eip_adapter_*` from it.
- `on_event`, `on_leds`, and `store_nv` run with no lock. They can call
  `eip_adapter_read` and `eip_adapter_write`. The adapter sends no T→O packet
  until the callback returns, so keep each callback short.

**Events.** The adapter reports an event in the same pass in which it finds
the change. The table lists the events.

| Event | Meaning |
|---|---|
| `EIP_EVENT_IO_OPENED` | A Class 1 connection opened. |
| `EIP_EVENT_IO_CLOSED` | The originator closed the connection, or opened it again. |
| `EIP_EVENT_IO_TIMEOUT` | The inactivity watchdog closed the connection. |
| `EIP_EVENT_RUN` | The connection changed to run. |
| `EIP_EVENT_IDLE` | The connection changed to idle. The consumed assembly reads as all zeros. |

A connection with a run/idle header opens in idle. `EIP_EVENT_RUN` follows
when the first packet says run. A connection with no header opens in run, and
`EIP_EVENT_RUN` follows `EIP_EVENT_IO_OPENED` immediately. The adapter does
not send `EIP_EVENT_IDLE` after a close or a timeout.

> **WARNING:** Do not use an event as the only signal that stops machinery.
> Two changes of the same type in one pass give one event. Read the consumed
> assembly in each cycle of your machine logic.

**Indicators.** `on_leds` gives the state of the two indicators as
`EIP_LED_OFF`, `EIP_LED_GREEN`, `EIP_LED_GREEN_FLASH`, `EIP_LED_RED`, or
`EIP_LED_RED_FLASH`. Your application drives the hardware. A flash is 500 ms
on and 500 ms off. `EipAdapterView` contains the same two states.

| Indicator | State | Condition |
|---|---|---|
| Network status | Off | The interface has no IP address. |
| Network status | Flashing green | No connection is open. |
| Network status | Green | A connection is open. |
| Network status | Flashing red | A connection timed out. The state stays until a connection opens again. |
| Module status | Green | The adapter runs. |

The adapter does not know the faults of your device. Thus, it does not set
the module status to red or to flashing green. Your application sets those
states.

**Stored settings.** A scanner can set the values in the table. The
application must keep them through a restart.

| Setting | CIP attribute | Default | Applies |
|---|---|---|---|
| `encap_timeout_s` | TCP/IP Interface (`0xF5`), attribute 13 | 120 | Immediately |
| `dscp_urgent`, `dscp_scheduled`, `dscp_high`, `dscp_low` | QoS (`0x48`), attributes 4–7 | 55, 47, 43, 31 | At the next start |
| `dscp_explicit` | QoS (`0x48`), attribute 8 | 27 | At the next start |

**Procedure: Keep the stored settings through a restart**

1. Set `store_nv` in `EipAdapterConfig`.
2. In the callback, write the full `EipNvSettings` structure to non-volatile
   memory.
3. At the start, read the structure from non-volatile memory.
4. Set `nv` in `EipAdapterConfig` to point to the structure.
5. Call `eip_adapter_start`.

If the memory has no stored structure, set `nv` to 0. The adapter then uses
the defaults. If a stored value is out of range, the adapter uses the default
for that value.

`encap_timeout_s` is the encapsulation inactivity timeout. The adapter closes
a TCP connection that receives no data for this number of seconds. A value of
0 stops this function. The adapter does not close the TCP connection of the
originator while its Class 1 connection is open.

The DSCP values mark the packets that the adapter sends. A T→O packet gets
the value for the priority in the Forward_Open. Explicit messages get
`dscp_explicit`. The QoS object is available only on a port that can mark
packets. `EipAdapterView.qos` is 1 on such a port. The table gives the
status of each port.

| Port | QoS object |
|---|---|
| POSIX | Yes. Tested with Linux: the DSCP value arrives in the received packet. |
| ESP-IDF | Yes. Tested with an ESP32-S31: a packet capture shows the stored values on UDP port 2222, UDP port 44818, and TCP port 44818. The SYN-ACK packet of each TCP connection has the value 0, because lwIP sends it before the adapter gets the socket. |
| ClearCore | Yes. The port file builds. Not yet tested on hardware. |
| NetBurner | No. The port sends with the socket functions of NNDK, and they have no DSCP setting. |
| Windows | No. Windows does not mark packets from a socket option. |

### 3.5 More control: the EipServer API

`eip_server.h` gives access to the instance behind `eip_adapter_*`. It has
these functions:

- `eip_server_init`
- `eip_server_poll`
- `eip_server_service`
- `eip_server_read`, `eip_server_write`, and `eip_server_snapshot`
- `eip_server_close`

Use this API for these tasks:

- Put the server (approximately 11 KB) in memory that you select.
- Run the loop from a task of your own.
- Stop the adapter and start it again.

`eip_adapter.h` is the sans-I/O core below the server. Use it if you write an
event loop of your own.

### 3.6 Connecting a PLC

Before you connect the PLC, examine the adapter from a PC on the same
network:

```
eiptool identity <adapter-ip>
eiptool get <adapter-ip> 4 100 3    # the produced assembly's data
```

You also need a vendor ID from ODVA. The value 65000 in this guide is a
placeholder. Use your vendor ID in real device names and EDS files.

**EDS file.** `examples/eds/QuickEIP_Adapter.eds` agrees with the assemblies
in 3.1. To adapt it, change the fields in this table.

| EDS field | Must be equal to |
|---|---|
| `VendCode`, `ProdType`, `ProdCode` | `vendor_id`, `device_type`, `product_code` |
| `MajRev`, `MinRev` | `revision_major`, `revision_minor`. EDS tools accept only 1–255. Do not use a minor revision of 0. |
| `ProdName` | `product_name` |
| `VendName`, `Catalog` | Your company and your part number. No configuration field agrees with these. |
| `AssemNNN`, its size, and its member size in bits | The instance of an `EipAssemblyConfig`, its `size`, and `size` × 8 |
| Connection sizes, `TSpec1`, `TSpec2` | The assembly sizes |
| `Number_Of_Static_Instances` in `[Assembly]` | `assembly_count` |
| Connection path | `20 04 24 <config> 2C <consumed> 2C <produced>`, with the instances in hexadecimal. This form is for instances of 255 or less. |
| Assembly path | `20 04 24 <instance> 30 03`, with the instance in hexadecimal |

Do not change the other fields. When you change an assembly layout, also
change `ProdCode` or the revision. A PLC then cannot use an old EDS file with
the new device.

On Windows, EDS Builder (`tools/eds_builder`) writes the file for you. It
reads the identity and the assemblies from your source files, sets the fields
in the table, and examines the result. Its
[README](../tools/eds_builder/README.md) gives the procedure and the test
status. Files from EDS Builder are not yet tested in PLC software.

The connection values of the sample opened a connection to an adapter. The
sample file is not yet tested in PLC software.

**Micro850 (Connected Components Workbench).** Tested with a Micro850 on the
ClearCore port. Add a generic EtherNet/IP device at the address of the
adapter. Then enter its assemblies: output (O→T), input (T→O), and config.
For the assemblies in 3.1, enter instances 150, 100, and 151, with 32, 32,
and 0 bytes. The table shows what a Micro850 sent.

| Setting | Value |
|---|---|
| RPI | 50 ms in each direction |
| O→T | Point-to-point, with a run/idle header |
| T→O | No header |
| Timeout multiplier | 0 (×4, thus 200 ms at an RPI of 50 ms) |
| Electronic key | All zeros |
| Config | Names the config instance and sends no data |

The config instance must exist. If it does not exist, the adapter refuses the
open with `0x01` / `0x0129`. An `EIP_ASM_CONFIG` assembly of size 0 is
enough.

**CompactLogix and ControlLogix (Studio 5000).** Not yet tested with this
adapter. A different adapter exchanged data with a CompactLogix. It used an
EDS file with these connection values and generic module settings in this
form.

To use the EDS file, install it with the *EDS Hardware Installation Tool*.
Then add the device as a module. As an alternative, add a *Generic Ethernet
Module* with the values in this table.

| Field | Value |
|---|---|
| Comm Format | Data - SINT |
| IP Address | The address of the adapter |
| Input | Assembly Instance 100, Size 32 |
| Output | Assembly Instance 150, Size 32 |
| Configuration | Assembly Instance 151, Size 0 |

Then set the RPI on the *Connection* tab. With the two methods, the config
assembly must exist on the adapter, also at size 0. It comes first in the
connection path.

**Adapter on a PC (POSIX port).** Not yet tested with a PLC. These rules
apply:

- Open TCP port 44818, UDP port 44818, and UDP port 2222 in the firewall.
- On a host with more than one interface, set `EIP_NETIF`. The adapter then
  reports the address that the PLC uses.
- With WSL2, use mirrored networking.
- The timing of a PC is less regular than that of an embedded target. Start
  with a longer RPI, such as 50 ms.

**For all PLCs,** these rules apply:

- The adapter has one Class 1 connection at a time. It refuses a second
  originator.
- Keep the RPI much longer than the clock step of your port. On NetBurner at
  20 Hz, use 50 ms or more (see section 6).
- If the PLC reports a fault, find the status in 3.3. `16#0127` and `16#0128`
  are size errors.
- The outputs read as zeros while the PLC is in Program mode.

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

These rules apply:

- A tag name uses `.` for a member and `[n]` or `[n,m]` for an element.
- The scanner divides a read or a write that is larger than one packet into
  fragments.
- When a call returns `EIP_ERR_CIP`, `status` or `eip_last_cip_status()`
  contains the CIP general status. `eip_last_cip_extended()` contains the
  extended status.
- With a `slot` of 0 or more, the router can cause the failure, not the
  target. For example, `0x01` / `0x0204` means that the slot did not answer.
- You can call `eip_session_is_open(&s)` on a zeroed session that was never
  open.
- Close a session before you open it again.
- After `EIP_ERR_TIMEOUT`, the session usually stays open. If a late reply
  arrives, the next request discards it.
- A packet that is cut short or too large closes the session, because the
  scanner cannot find the start of the next packet. The call that fails
  returns its error. Subsequent calls return `EIP_ERR_STATE`. Then open the
  session again.
- `eip_session_is_open` tells you only that the session is registered on
  your side. It stays 1 after the peer stops. Do not use it to find a lost
  device.
- After `EIP_ERR_IO`, or when requests time out again and again, close the
  session and open it again.

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

`eip_device_kind()` puts Rockwell parts into three groups by catalog name:
Micro800, CompactLogix, and other. The result is an estimate. Your
application makes the decision about what to do with it.

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

The originator binds a UDP port in the range 2223–2231, unless you set
`local_port`. Thus, it can share a device with the adapter on UDP port 2222.
You control the packet rate: call `eip_io_produce` one time for each RPI.

Open Class 1 on a session with `slot = -1`. On a routed session, the
Forward_Open goes through Unconnected Send. That combination is not yet
tested.

**Use the address that the adapter reports.** These rules apply:

- Connect to the adapter at the IP address that a scan shows.
- The adapter tells the originator to send to that address. It accepts O→T
  only from the address that sent the Forward_Open.
- With a different address on the same host, such as `127.0.0.1`, the two
  addresses are different. The adapter drops the packets
  (`rx_dropped_source`), and the connection times out.
- This rule does not apply to explicit messages.
- On a POSIX host with more than one address, `EIP_NETIF` selects the address
  that the adapter reports.

**Find a lost connection.** `eip_io_produce` uses UDP, and thus it continues
to succeed. `eip_io_consume` returns `EIP_ERR_TIMEOUT` when no T→O packet
arrives in `timeout_ms`. A sequence of timeouts shows that the connection is
lost.

**Procedure: Recover a lost Class 1 connection**

1. Call `eip_io_close`.
2. Call `eip_session_close`.
3. Call `eip_session_open`.
4. Call `eip_io_open`.
5. Make sure that `eip_io_consume` returns `EIP_OK` again.

Do all steps. If you open the I/O again on the old session, the call fails
with `EIP_ERR_IO`. When the peer is not there, `eip_io_close` can block for
the session timeout and return an error. This is normal, and the call
releases the UDP socket.

**Packet rate with a consume that blocks.** `eip_io_consume` waits. Thus, a
loop such as the sample runs at the T→O rate. There is no non-blocking form,
and a `timeout_ms` of 0 gives the default of 1 second. Use a short timeout,
or use different tasks for produce and consume.

**Config instance.** Use the value from the EDS file of the target.
`config_instance = 0` sends instance 0 in the connection path. The QuickEIP
adapter accepts that. A different target can refuse it.

## 5. Threads, blocking and memory

These rules apply to tasks:

- **Scanner calls block** until they are complete or they time out. On an
  RTOS, call them from a task of their own.
- On bare metal, a scanner call keeps the network stack in operation while it
  waits, and the sockets of the adapter continue to receive packets. The
  other parts of your main loop stop until the call returns.
- Only one task at a time can use an `EipSession` or an `EipIo`. Different
  sessions can run in different tasks.
- You can call the read, write, and snapshot functions of the adapter from
  any task. They take `eip_hal_lock`. The adapter holds the lock for
  microseconds, and never during a socket call.

The table gives the memory use with the default settings.

| Item | Memory |
|---|---|
| Built-in adapter | Approximately 11 KB of static RAM, plus a shared reply buffer of 1.5 KB |
| Each `EipSession` | Approximately 4.7 KB of RAM |
| Each `EipIo` | 100 bytes of RAM, plus a stack buffer of 1.5 KB during produce and consume |
| Scanner calls | Approximately 2 KB or less of caller stack. A tag write builds requests of 400 bytes. |
| Code, all files | Approximately 24 KB at `-Os` on a Cortex-M4 |

## 6. NetBurner NNDK

The port builds with NNDK 3.5.8 (ARM platforms). The adapter is tested with
a MODRT1171 and the QuickEIP scanner on a PC. The test included explicit
messages, Class 1 at an RPI of 50 ms, and the inactivity timeout. It also
included the event and indicator callbacks, the stored settings, and Identity
Reset. The adapter is
not yet tested with a PLC. The scanner is not yet tested on hardware. The
NNDK 3.x socket API is the same on all platforms.

The NNDK rules put each object at `obj/release/<source path>`. A source path
that starts with `../` puts the objects outside `obj/`, adjacent to your
source files. Thus, put QuickEIP in your project. The example in this
repository must use `../..`, and it shows this effect.

Scanner calls block. Thus, the procedure starts them in a task of their own.
Each task needs a different priority, and a lower number runs first. The
adapter takes `MAIN_PRIO - 1`. Thus, `MAIN_PRIO + 1` is a safe value for the
scanner task. `UserMain` must then sleep to let the task run.

**Procedure: Add QuickEIP to an NNDK project**

1. Put QuickEIP in a subfolder or a git submodule of your project, for
   example `MyProject/QuickEIP`.
2. Write the makefile of the project. This is a complete makefile:

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

3. Start the adapter and the scanner task from `UserMain`:

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

4. Set the NNDK environment and build, for example:

   ```sh
   export NNDK_ROOT=C:/nburn PLATFORM=MODM7AE70 DEFPLATFORM=MODM7AE70
   export PATH=/c/nburn/gcc/bin:/c/nburn/pcbin:/c/nburn/gcc/msys/1.0/bin:$PATH
   make
   ```

5. Make sure that the build makes `obj/release/MyProject.bin`.

The first build takes some minutes, because the NNDK compiles its system
library into the `obj/` folder of the project. This is normal.

For NBEclipse, add the same files to the project. Add the two include
directories to the C compiler settings and to the C++ compiler settings.

**Notes for NetBurner**

- **Clock resolution.** `eip_hal_millis` follows `TimeTick`: 50 ms at the
  default of 20 Hz. The adapter never sets a Class 1 timeout shorter than two
  ticks (100 ms). T→O packets go out on tick boundaries. For an RPI of less
  than approximately 50 ms, increase `TICKS_PER_SECOND` in your platform
  configuration.
- **Task.** By default, the adapter task runs at `MAIN_PRIO - 1`. To change
  the priority, set `EIP_ADAPTER_TASK_PRIORITY`. The task uses the default
  task stack of the NNDK.
- **Lock.** `eip_hal_lock` is `OSLock()`, which stops task switches for a
  short time. The adapter never holds it during a socket call.
- **UDP sockets.** The port uses `CreateRxTxUdpSocket4` with an explicit
  local port. When the library asks for "any port," the port selects one in
  the range 50000–50063.
- **WebUI.** `examples/netburner_webui` is the earlier test web page, changed
  for the new API. It is application code, not a part of the stack.

## 7. ESP-IDF (ESP32)

The port builds with ESP-IDF 5.5.1 on the esp32 target. That target is not
yet tested on hardware. The adapter is tested with an ESP32-S31 (Ethernet,
YT8531 PHY), ESP-IDF 6.1, and the QuickEIP scanner on a PC. The test included
explicit messages, Class 1 at an RPI of 10 ms, and the inactivity timeout. It
also included the event and indicator callbacks, the stored settings, and
Identity Reset. A packet capture showed the DSCP values in the transmitted
packets. The adapter is not yet tested with a
PLC. The scanner is not yet tested on hardware. Other ESP-IDF versions are
not yet tested.

The QuickEIP root directory is an IDF component. The component takes the name
of its directory. Use the same spelling in all places.

You need the PHY type, the pins, and the RMII clock mode of your board. With
a wrong value, the build is successful, but the link does not come up.

**Procedure: Add QuickEIP to an ESP-IDF project**

1. Add the component to the top-level `CMakeLists.txt`. The path is relative
   to the project. In this example, QuickEIP is adjacent to the project:

   ```cmake
   cmake_minimum_required(VERSION 3.16)
   list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../QuickEIP")
   include($ENV{IDF_PATH}/tools/cmake/project.cmake)
   project(myapp)
   ```

2. Name the component and your network components in `main/CMakeLists.txt`:

   ```cmake
   idf_component_register(SRCS "main.c"
                          INCLUDE_DIRS "."
                          REQUIRES QuickEIP esp_eth esp_netif esp_event)
   ```

3. Start Ethernet. Use `examples/ethernet/basic` in the IDF as a guide for
   the driver. This code is for the internal EMAC of the ESP32 with a LAN87xx
   PHY:

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

4. If you want the adapter to report the link, add a `link_info` callback.
   This step is optional:

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

5. Set all `cfg` fields, then start the adapter:

   ```c
   #include "eip.h"
   #include "eip_esp_idf.h"

   eth_start();
   cfg.link_info = link_info;       /* with the rest of cfg, from section 3 */
   eip_esp_set_netif(&s_netif, 1);  /* optional: default is ETH_DEF, WIFI_STA_DEF, WIFI_AP_DEF */
   eip_adapter_start(&cfg);
   ```

6. Run `idf.py set-target esp32`.
7. Run `idf.py build`, and make sure that the build is successful.

**Component manager.** As an alternative to step 1, use the component manager
in `main/idf_component.yml`. The dependency key **must be the same as the
directory name**. The path is relative to `main/`:

```yaml
dependencies:
  QuickEIP:
    path: ../../QuickEIP
```

If the names are different, the build fails with
`Failed to resolve component ... unknown name`. A change to `REQUIRES` does
not correct it.

**Link report.** `esp_netif` does not report the speed or the duplex mode.
Thus, the callback in step 4 reads them from the driver. It returns 1 while
the link is up, and 0 to report link down.

**Start sequence.** `eip_adapter_start` copies the `cfg` fields. Thus, set
all fields before the call. `ports/esp-idf/eip_esp_idf.h` describes
`eip_esp_set_netif`. Call it before `eip_adapter_start`. It copies the list,
and the netifs must exist for as long as the adapter runs.

**Options.** Put the options in the compile definitions of all components.
Set them after `include(.../project.cmake)` and before `project()`:

```cmake
idf_build_set_property(COMPILE_DEFINITIONS "EIP_ESP_TASK_PRIORITY=12" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "EIP_ESP_TASK_WDT=1" APPEND)
```

The table lists the options.

| Define | Default | Effect |
|---|---|---|
| `EIP_ESP_TASK_STACK` | 6144 | The stack of the adapter task, in bytes. `EIP_ADAPTER_TASK_STACK` has the same effect. |
| `EIP_ESP_TASK_PRIORITY` | 12 | The priority of the adapter task. `EIP_ADAPTER_TASK_PRIORITY` has the same effect. |
| `EIP_ESP_TASK_CORE` | `tskNO_AFFINITY` | The core that the adapter task runs on. |
| `EIP_ESP_TASK_WDT` | 0 | The value 1 adds the adapter task to the task watchdog. |

**Notes for ESP-IDF**

- Use **wired Ethernet** for Class 1 in production. The latency of Wi-Fi is
  frequently longer than a timeout of 8 × RPI.
- The adapter task waits in `select()`. Thus, it wakes when a packet arrives.
  While idle, it runs a pass at intervals of `EIP_ADAPTER_IDLE_WAIT_MS`
  (5 ms) or less.
- **Arduino-ESP32** runs on ESP-IDF and can use the same files. Not yet
  tested. Make a library folder with a `src/` that contains `include/*`,
  `src/*`, `ports/bsd_sockets/*`, and `ports/esp-idf/*`. Omit the other
  ports, because Arduino compiles each file that it finds.

The full example is `examples/esp_idf_adapter`. It uses
`protocol_examples_common` from the IDF. For that example only, run
`idf.py menuconfig` and configure the network in *Example Connection
Configuration*.

## 8. Teknic ClearCore

`examples/clearcore_adapter` builds with arm-gcc 6.3 from Microchip Studio
and the Enhanced ClearCore Library. That library is a fork of ClearCore
library 1.7. The adapter runs on a ClearCore, tested with a Micro850. The
example itself is not yet tested.

ClearCore runs on bare metal (no RTOS) with the raw API of lwIP and
`NO_SYS=1`. QuickEIP calls lwIP directly through `ports/lwip_raw`. It does
not use the `EthernetTcp*` and `EthernetUdp` classes. Those classes have a
TCP buffer of 600 bytes and no broadcast send. EtherNet/IP needs more.

QuickEIP uses only standard ClearCore calls and lwIP calls. It compiles with
the standard library 1.7. It links with the Enhanced fork.

In step 5 of the procedure, the template sets the lwIP directories for C++
only, and the lwIP port is a C file. Without those directories, the build
stops at `lwip/ip_addr.h`. In step 6, the default of four clients is too many
for this lwIP build. Do not change the `-std=gnu11` setting of the template.

**Procedure: Add QuickEIP to a ClearCore project**

1. Copy `ProjectTemplate`, `libClearCore`, and `LwIP` from the ClearCore
   library folder to a folder that you can write to.
2. Put your QuickEIP copy adjacent to them. The four folders must be in the
   same parent folder, because the paths are relative.
3. Open `ProjectTemplate.atsln`, and make sure that the three projects load.
4. Add the QuickEIP files with *Add → Existing Item → Add As Link*. The files
   then stay in your QuickEIP copy. Add these files:
   - `src/*.c` (all files)
   - `ports/lwip_raw/eip_hal_lwip_raw.c`
   - `ports/clearcore/eip_hal_clearcore.cpp`
5. Open *Project Properties → Toolchain*, and add the directories for the
   `Debug` and `Release` configurations. The paths are relative to the output
   folder:
   - In *ARM/GNU C Compiler → Directories* and *ARM/GNU C++ Compiler →
     Directories*, add `../../QuickEIP/include` and `../../QuickEIP/src`.
   - In *ARM/GNU C Compiler → Directories* only, also add
     `../../LwIP/LwIP/src/include` and `../../LwIP/LwIP/port/include`.
6. In *ARM/GNU C Compiler → Symbols* and *ARM/GNU C++ Compiler → Symbols*,
   add `EIP_ADAPTER_MAX_CLIENTS=2` for the two configurations. Keep the
   `DEBUG` and `NDEBUG` symbols.
7. Replace the `main.cpp` of the template. The full example is in
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

8. Build the solution, and make sure that the build is successful.

> **CAUTION:** `EIP_ADAPTER_MAX_CLIENTS` changes the sizes of structures. Set
> the same value for the C compiler and the C++ compiler. Different values
> corrupt memory.

**Compile without Studio.** To do a check of a compile, run
`arm-none-eabi-gcc` or `arm-none-eabi-g++` with `-c` from
`ProjectTemplate/Debug`. The toolchain is in
`Atmel/Studio/7.0/toolchain/arm/arm-gnu-toolchain/bin`, and `<Packs>` is
`Atmel/Studio/7.0/Packs`. Use these flags:

```
-mthumb -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16
-D__SAME53N19A__ -DEIP_ADAPTER_MAX_CLIENTS=2
-I../../QuickEIP/include -I../../QuickEIP/src -I../../QuickEIP/ports/clearcore
-I../../libClearCore/inc -I../../LwIP/LwIP/src/include -I../../LwIP/LwIP/port/include
-I<Packs>/atmel/SAME53_DFP/1.1.118/include
-I<Packs>/arm/CMSIS/4.5.0/CMSIS/Include
-I<Packs>/arm/CMSIS/5.4.0/CMSIS/Core/Include
```

**Notes for ClearCore**

- **Call `eip_adapter_poll()` frequently.** T→O packets go out from
  `eip_adapter_poll()`. Thus, the timing of your loop becomes RPI jitter.
  Keep the loop time much shorter than your RPI.
- **The lwIP build is small.** It has a heap of 4 KB (`MEM_SIZE`), 5 TCP
  PCBs, and 8 listening PCBs. The adapter uses 1 listening PCB plus 1 for
  each explicit session. A scanner session uses one more. Thus, step 6 sets
  `EIP_ADAPTER_MAX_CLIENTS`. Use 2 or 3.
- **More clients.** If you need more clients, build the LwIP project again
  with a larger `MEM_SIZE` and `MEMP_NUM_TCP_PCB`. Its `lwipopts.h` uses
  `#ifndef`, and thus `-D` changes the values.
- **Scanner calls that block.** These calls continue to call
  `EthernetMgr.Refresh()` while they wait, and motion continues from
  interrupts. Your main loop does not run until the call returns. Keep
  `timeout_ms` short.
- **Interrupts.** Do not call QuickEIP from an interrupt handler. lwIP
  callbacks run in `EthernetMgr.Refresh()` in the main context. Thus, the
  port needs no lock.
- **Socket table.** `EIP_LWIP_MAX_SOCKETS` (12), `EIP_LWIP_UDP_QUEUE` (8),
  and `EIP_LWIP_ACCEPT_QUEUE` (4) set the size of the socket table for the
  raw API.
- **Link report.** The port reports the link as 100 Mbit/s full duplex while
  the PHY link is up.
- **Multicast T→O.** The port sets TTL 1 only in lwIP builds with
  `LWIP_MULTICAST_TX_OPTIONS`, and lwIP selects the interface. The standard
  build does not have that option. Use a point-to-point connection.

**Arduino (ClearCore wrapper).** Not yet tested. The same three file groups
can be an Arduino library.

## 9. Linux, macOS, WSL

> **WARNING:** A PC is not a real-time platform. Packet timing can change
> without notice. Do not use a PC adapter as a safety function.

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

These commands build `libquickeip.a` (the core plus the POSIX port), the
tests, and `eiptool`:

```
eiptool scan
eiptool identity 192.168.1.10
eiptool read  192.168.1.10 MyDint            # slot -1; append a slot number for ControlLogix
eiptool write 192.168.1.10 MyDint dint 42
eiptool tags  192.168.1.10
eiptool get   192.168.1.10 0x01 1 7          # class instance attribute
eiptool adapter                              # run the test adapter
```

These rules apply:

- Set `EIP_NETIF=eth1` to select the interface that the adapter reports and
  that the scan uses first. The setting applies to each program that uses the
  POSIX port, not only to `eiptool`.
- If the name in `EIP_NETIF` is not known, the port ignores it and uses the
  default interface.
- Ports 44818 and 2222 are above 1024. Thus, you do not need root access.
- A different EtherNet/IP program on the same host prevents the use of these
  ports.
- With WSL2, the host is behind NAT. A scan out to the network is possible. A
  PLC on your LAN cannot connect to a WSL adapter, unless you use mirrored
  networking.

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

The `quickeip` link target adds the include path and the thread library.
Without the two `OFF` lines, the build also makes the tests and `eiptool`.
`eiptool` is then in `build/quickeip/`.

## 10. Windows

The port runs on Windows 11 (Visual Studio 2022, x64 and Win32). The port is
`ports/windows/eip_hal_windows.c`. It needs Windows 7 or later.

> **WARNING:** Windows is not a real-time platform. A stall of approximately
> 100 ms occurred on a bench PC. Do not use a Windows adapter as a safety
> function. Set `timeout_floor_ms` in `EipAdapterConfig` to 500 or more.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build/Release/eiptool scan
```

`eiptool` takes the same commands as in section 9.

**In a project of your own,** do these steps:

1. Compile `src/*.c` and `ports/windows/eip_hal_windows.c`.
2. Add `include/` and `src/` to the include path.
3. Link `ws2_32`, `iphlpapi`, and `winmm`.

The table lists the options.

| Define | Default | Effect |
|---|---|---|
| `EIP_WIN_POLL_MODE` | Off | The port starts no adapter thread. Your loop calls `eip_adapter_poll()`. |
| `EIP_ADAPTER_TASK_PRIORITY` | 0 | A `THREAD_PRIORITY_*` value for the adapter thread. The value 0 selects `THREAD_PRIORITY_HIGHEST`. |
| `EIP_ADAPTER_TASK_STACK` | 0 | The stack of the adapter thread, in bytes. The value 0 selects the system default. |
| `EIP_WIN_MAX_SOCKETS` | 64 | The number of sockets that can be open at the same time. |
| `EIP_WIN_MAX_INTERFACES` | 16 | The number of interfaces that the port reports. |
| `EIP_WIN_NETIF_REFRESH_MS` | 2000 | The interval at which the port reads the interface list again. A different thread from the adapter thread does this. |

**Notes for Windows**

- **Select the interface on a PC with more than one.** The adapter reports
  interface 0 as its address, in ListIdentity and in the O→T socket address
  of a Forward_Open. Interface 0 is the first interface that is up and has a
  default gateway. Frequently, that is not the machine network.
- **`EIP_NETIF`.** Set it to the friendly name or the IPv4 address of the
  network adapter: `EIP_NETIF="Ethernet 2"` or `EIP_NETIF=10.168.41.1`. The
  port keeps a named interface while its cable is disconnected, and reports
  link down. Scanner calls do not need this setting. They use the interface
  that has a route to the target.
- **Firewall.** An adapter needs inbound TCP port 44818, UDP port 44818, and
  UDP port 2222 for your program. A scanner that uses Class 1 needs inbound
  UDP on its local port. `Get-NetConnectionProfile` shows the profile of the
  network.
- **A new `.exe` can lose its first replies.** Security software from a third
  party can hold inbound UDP for the first seconds that a new program runs.
  This can occur while Windows Firewall is off.
- **Test result for a new `.exe`.** On a test PC with AVG, each new build
  failed its first `eiptool identity`. Two copies failed one time, and a
  third copy failed two times. Then each copy answered in approximately
  45 ms. If a new build times out or a scan finds too few devices, run it
  again before you look for a network fault.
- **Ports in use.** Other EtherNet/IP software on the PC, for example RSLinx,
  can hold port 44818 or port 2222. Each bind in this port is exclusive.
  Thus, the adapter cannot open the ports, and `listening` stays 0.
- **Timing.** From its first call, the port sets a system timer period of
  1 ms for the process. This increases the power use of a laptop. On the test
  PC, an RPI of 10 ms ran at a mean of 10.00 ms. The worst value was 11.83 ms
  in 1,500 packets, with the two ends on the same host.
- **Inactivity timeout.** Keep the inactivity timeout much longer than the
  stall time in the warning.
- **Link report.** The speed comes from Windows. Windows does not give the
  duplex mode or the autonegotiation state. Thus, the port reports full
  duplex and autonegotiated while the link is up.
- **Not yet tested:** multicast T→O, and a PLC that connects to a Windows
  adapter.

## 11. Troubleshooting

The table gives the usual cause of each symptom.

| Symptom | Usual cause |
|---|---|
| `listening` stays 0 | A different program holds TCP port 44818, UDP port 44818, or UDP port 2222. Or the interface has no address yet. The server tries again each second. |
| The PLC shows a module fault with code 16#0127 or 16#0128 | The connection sizes in the EDS file do not agree with the assembly sizes. See 3.3. Include the 4-byte run/idle header on O→T. |
| Class 1 opens, then stops after a fraction of a second, and `rx_dropped_source` increases | Connect to the adapter at the IP address that it reports, not `127.0.0.1` or a different address on the host. See 4.3. |
| `eip_io_open` returns `EIP_ERR_IO` after a lost connection | Close the session and open it again. See 4.3. |
| ESP-IDF: `Failed to resolve component ... unknown name` | The key in `idf_component.yml` is not the same as the QuickEIP directory name. See section 7. |
| ClearCore: `lwip/ip_addr.h: No such file or directory` | The C compiler does not have the LwIP paths. See section 8, step 5. |
| The connection times out at intervals of a few seconds | O→T does not arrive, or the adapter drops it. `rx_dropped_source` counts packets from the wrong address (NAT, hosts with more than one address). `rx_dropped_seq` counts duplicate or old sequence numbers. On Wi-Fi, examine the latency. |
| `opens` and `timeouts` increase together | The same causes as the row before. Also make sure that no firewall stops UDP port 2222, TCP port 44818, or UDP port 44818. |
| The outputs read zero although the PLC sends data | The PLC is in Program mode (idle). This is the design. |
| Scanner `EIP_ERR_CIP` with `0x05` or `0x04` | The target did not find the tag or the path, or the path is malformed. Examine the tag name and the slot (-1 or a backplane slot). |
| Scanner `EIP_ERR_CIP` with `0x16` | The object does not exist, for example a wrong assembly instance. |
| Scanner `EIP_ERR_CIP` with `0x01` / `0x0204` on a routed session | The router timed out before it reached the slot. Examine the slot number, and make sure that a module is there. |
| Scanner calls return `EIP_ERR_STATE` after an error | The session closed after a bad packet. Open it again. |
| A scan finds no devices | Broadcasts do not go through routers or VLANs. Use `eip_list_identity` with an address. On Windows, also run a newly built program a second time. See section 10. |
