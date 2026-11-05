# Porting QuickEIP to a new platform

A port is one source file (sometimes two) that implements the functions in
`include/eip_hal.h`. The core calls them directly, with no function table.
This guide explains what each function must do and how to check a new port.

## Start from the closest existing port

| Your platform has | Start from | Write |
|---|---|---|
| BSD sockets (`socket`, `select`, `fcntl`): lwIP with `LWIP_SOCKET`, Zephyr, Linux, FreeRTOS+TCP with its BSD layer | `ports/bsd_sockets/eip_hal_bsd_sockets.c` (reuse as is) | A platform file like `ports/esp-idf/eip_hal_esp_idf.c`: clock, lock, interfaces, task |
| lwIP raw API, `NO_SYS=1`, no socket layer: STM32Cube bare metal, SAM, other ClearCore-like boards | `ports/lwip_raw/eip_hal_lwip_raw.c` (reuse as is) | A platform file like `ports/clearcore/eip_hal_clearcore.cpp`: clock, yield (= your stack poll), interfaces |
| A vendor socket API that is neither (NetBurner) | `ports/netburner/eip_hal_netburner.cpp` | The whole contract |
| Winsock | `ports/windows/eip_hal_windows.c` (reuse as is) | Nothing |

The platform file is short. For ESP-IDF it's about 190 lines with comments,
and for ClearCore about 130.

## The contract

General rules:

- **Handles.** A socket handle is an `int`, and negative means invalid.
  The core keeps track of which handles it owns, so 0 is a valid handle.
- **Addresses.** IPv4 addresses are `uint8_t[4]` in network order
  (`{192,168,1,10}`). Ports are plain host-order integers.
- **Timeouts.** `timeout_ms == 0` means "don't wait".
- **Blocking waits.** Every blocking wait must keep the network stack
  running. On bare metal that means calling your stack's poll inside the
  loop, which is what `eip_hal_yield()` is for.
- **Return codes.** `EIP_OK` (0) or a negative `EIP_ERR_*`.

### Time and scheduling

| Function | Must |
|---|---|
| `uint32_t eip_hal_millis(void)` | Return a free-running millisecond counter that wraps at 2³². Only differences are used. T→O packets are paced on it, so use a real 1 ms source wherever the platform has one. On Windows, `GetTickCount64` steps about every 16 ms: the adapter's T→O ran at about half rate and the connection timed out. |
| `uint32_t eip_hal_clock_resolution_ms(void)` | Return the counter's real step in ms (1 for a 1 kHz source). The adapter never sets a Class 1 timeout under two steps. `tests/test_hal.c` measures the step and fails a clock that steps more coarsely than it reports. |
| `void eip_hal_yield(void)` | Let other work run for a moment. On an RTOS, sleep one tick. On bare metal, run the network stack's poll (for example `EthernetMgr.Refresh()` or `sys_check_timeouts()` + `ethernetif_input()`). |
| `void eip_hal_lock(void)` / `eip_hal_unlock(void)` | Mutual exclusion between the adapter context and application tasks. It isn't nested, is held for microseconds, and is never held across a socket call (the core may read `eip_hal_millis` under it). Single-threaded ports leave them empty. |

### Network interfaces

`int eip_hal_netif_get(uint8_t index, EipNetifInfo *info)` fills in
interface `index`, where 0 is the primary one. Rules:
- Return `EIP_ERR_ARG` past the last interface.
- An interface without an address yet still returns `EIP_OK`, with
  `ip = 0.0.0.0`. The POSIX port is an exception: it lists only interfaces
  that are up with an IPv4 address, so it returns `EIP_ERR_ARG` until one is.
- Zero anything you don't know.
- Set `link_known = 1` when you can report `link_up`. Otherwise the server
  treats "has an address" as "link up, 100 Mbit/s full duplex". Fill
  `speed_mbps`, `full_duplex` and `autonegotiated` with real values if you
  can. The shipped ports vary: ClearCore and POSIX report the link state with
  a fixed 100 Mbit/s full duplex, and NetBurner reads speed and duplex but
  sets `autonegotiated` from `link_up`.

How the result is used:
- The adapter calls this every 100 ms for `netif_index` (usually 0), to
  fill in its Identity, TCP/IP Interface and Ethernet Link objects.
- The scan walks indexes 0, 1, 2 … to find broadcast addresses.
- The MAC sets the default serial number.

### TCP

| Function | Must |
|---|---|
| `eip_hal_tcp_connect(ip, port, timeout_ms)` | Connect, waiting up to `timeout_ms`. Return a handle or `EIP_SOCK_INVALID`. |
| `eip_hal_tcp_listen(port, backlog)` | Listen on all addresses. Return a handle or `EIP_SOCK_INVALID`. |
| `eip_hal_tcp_accept(listener, peer_ip)` | **Never block.** Return a connected handle and fill `peer_ip`, or `EIP_SOCK_INVALID` when nobody is waiting. |
| `eip_hal_tcp_send(sock, data, len)` | Queue **all** `len` bytes. Return `EIP_ERR_IO` on a broken connection or if the bytes can't be queued within a few hundred ms. Don't block indefinitely. |
| `eip_hal_tcp_recv(sock, buf, cap, timeout_ms)` | Return >0 bytes read, 0 if nothing arrived in time, or `EIP_ERR_IO` once the peer has closed or the connection failed. With `timeout_ms == 0`, return immediately. |
| `eip_hal_tcp_local_ip(sock, ip)` | Return the local address of the connection. It goes into the T→O socket address of a Forward_Open. |
| `eip_hal_tcp_close(sock)` | Close the socket and free the handle. Closes both connected and listening sockets. |

Recommended for adapter sessions: TCP_NODELAY and keep-alive (about 30 s
idle, 3 probes). A shipped port does this as far as its stack allows:

| Layer | TCP_NODELAY | Keep-alive |
|---|---|---|
| `bsd_sockets` | All connections | Accepted sockets only. The 30 s tuning needs `TCP_KEEPIDLE`, `TCP_KEEPINTVL` and `TCP_KEEPCNT`. |
| `lwip_raw` | All connections | Only in lwIP builds with `LWIP_TCP_KEEPALIVE`. |
| `netburner` | No | No, so a vanished peer's slot comes back through eviction. |

### UDP

| Function | Must |
|---|---|
| `eip_hal_udp_open(local_port, flags)` | Bind on all addresses. `local_port == 0` means any free port. **Fail if the port is already taken.** Don't use `SO_REUSEADDR`: the scanner's automatic search of UDP 2223–2231 relies on a failed bind to move past a port in use, and it's what refuses an explicit `local_port` of 2222 while an adapter runs on the same device. `EIP_UDP_BROADCAST` allows broadcast sends. `EIP_UDP_MULTICAST_TX` allows multicast sends with TTL 1 on the primary interface, where the stack can set them: `lwip_raw` needs `LWIP_MULTICAST_TX_OPTIONS`, and `netburner` ignores the flag. |
| `eip_hal_udp_sendto(sock, ip, port, data, len)` | Send one datagram. Return `EIP_OK` or `EIP_ERR_IO`. |
| `eip_hal_udp_recvfrom(sock, buf, cap, ip, port, timeout_ms)` | Receive one datagram. Return its length, 0 if nothing arrived in time, or `EIP_ERR_IO`. Truncate to `cap`. `ip` and `port` may be NULL. |
| `eip_hal_udp_close(sock)` | Close and free the handle. |

Receiving broadcasts on a socket bound to the any-address must work. The
adapter answers broadcast ListIdentity requests on UDP 44818.

### Waiting and the adapter runner

| Function | Must |
|---|---|
| `eip_hal_wait(socks, count, timeout_ms)` | Wait until one of the handles may be readable (or a listener has a pending connection), or until the timeout. Skip invalid handles. It may return early. Without `select()`, a loop of `eip_hal_yield()` and readiness checks is fine. |
| `eip_hal_adapter_start_task(server)` | Called once by `eip_adapter_start()`. **RTOS:** create a task that runs `for (;;) eip_server_service(server, EIP_ADAPTER_IDLE_WAIT_MS);`. Stack: 4–6 KB (the server itself is static; `service` needs about 1 KB, plus your platform's socket call depth). **Bare metal:** do nothing and return `EIP_OK`. The application calls `eip_adapter_poll()`. |

## Checking a new port

Before you try a PLC:

1. **Compile with warnings on** (`-Wall -Wextra`). The core builds without
   warnings with these flags on the compilers the shipped ports use
   (arm-gcc 6.3 and 8.1, xtensa-gcc from IDF 5.5, host gcc 13).
2. **Run `tests/test_hal.c`** if your platform can host it. It calls the
   port directly over loopback and checks the rules above: busy ports fail,
   long datagrams truncate, ICMP errors are not fatal, timeouts hold, `accept`
   never blocks, and a stalled peer can't hang `send`.
3. **Run the test adapter on the target** with an empty config:
   `eip_adapter_start(0)`. Then, from a PC on the same network, using the
   POSIX build (`cmake -S . -B build && cmake --build build`):
   ```
   ./build/eiptool scan                     # finds "EIP Test Adapter"
   ./build/eiptool identity <target-ip>
   ./build/eiptool get <target-ip> 1 1 7    # Identity product name
   ./build/eiptool get <target-ip> 4 101 3  # produced assembly: 11 22 33 44 55 66 77 88
   ./build/eiptool get <target-ip> 0xF5 1 5 # TCP/IP interface configuration
   ```
4. **Test Class 1 from the PC.** `tests/test_loopback.c` shows the
   originator calls to use: `eip_io_open` with config 150, output 100,
   input 101, 8 bytes each way. Turn that into a short program against the
   target and leave it running for a few minutes. Watch `rx_packets`,
   `timeouts` and `rx_dropped_*` in `eip_adapter_snapshot` on the target.
5. **Test the scanner side on the target** against the PC:
   `eiptool adapter` runs the same test adapter on the PC. Call
   `eip_list_identity`, `eip_session_open`, `eip_get_attribute` and
   `eip_io_open` from the target.
6. **Test with a real PLC**, with an EDS that matches your assemblies.

If something fails, compare with the POSIX port. It's the reference, and
`test_loopback` runs every path through it.
