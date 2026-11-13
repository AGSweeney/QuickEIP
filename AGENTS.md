# Guide for coding agents

QuickEIP is an EtherNet/IP scanner and adapter stack in portable C99. Use this
file to find the right document, and to avoid the mistakes that cost the most
time.

## Where to look

| Task | Read |
|---|---|
| Run an adapter | [INTEGRATION.md §3](docs/INTEGRATION.md#3-running-an-adapter) |
| Connect a PLC, write an EDS | [INTEGRATION.md §3.6](docs/INTEGRATION.md#36-connecting-a-plc) and `examples/eds/QuickEIP_Adapter.eds` |
| Generate an EDS from a project (Windows) | [tools/eds_builder/README.md](tools/eds_builder/README.md) |
| Talk to a PLC or device | [INTEGRATION.md §4](docs/INTEGRATION.md#4-using-the-scanner) |
| NetBurner | [INTEGRATION.md §6](docs/INTEGRATION.md#6-netburner-nndk) |
| ESP-IDF | [INTEGRATION.md §7](docs/INTEGRATION.md#7-esp-idf-esp32) |
| ClearCore | [INTEGRATION.md §8](docs/INTEGRATION.md#8-teknic-clearcore) |
| Linux, or your own CMake project | [INTEGRATION.md §9](docs/INTEGRATION.md#9-linux-macos-wsl) |
| Windows | [INTEGRATION.md §10](docs/INTEGRATION.md#10-windows) |
| A new platform | [docs/PORTING.md](docs/PORTING.md) |
| Something isn't working | [INTEGRATION.md §11](docs/INTEGRATION.md#11-troubleshooting) |
| What isn't supported | [README Limitations](README.md#limitations) |
| Write or edit documentation | [docs/WRITING_STYLE.md](docs/WRITING_STYLE.md) |

The API reference is the comments in `include/`, plus each port's own header:
`ports/esp-idf/eip_esp_idf.h` and `ports/clearcore/eip_clearcore.h`.

## Common mistakes

- **`slot`:** a zeroed `EipOpenConfig` has slot 0, which routes across a
  backplane. Set `slot = -1` for anything reached directly.
- **Sizes:** enter assembly bytes in an EDS or PLC. The 2-byte sequence count
  and 4-byte run/idle header are added on the wire. See
  [§3.3](docs/INTEGRATION.md#33-assemblies-and-your-eds-file).
- **Direction:** consumed is what the PLC writes, and produced is what the PLC
  reads. The built-in test set uses 100 as consumed, the reverse of the quick
  start.
- **Class 1 needs the address the adapter reports.** It fails on `127.0.0.1` or
  any other address on that host. See
  [§4.3](docs/INTEGRATION.md#43-class-1-as-an-originator).
- **Recovering a lost connection** means reopening the session too.
  `eip_session_is_open` doesn't detect a lost peer. See the same section.
- **Config must match everywhere.** Settings in `eip_config.h` change struct
  sizes, so C and C++ files must all see the same values.
- **`link_info` and other config fields** must be set before
  `eip_adapter_start()`, which copies them.
- **Polling:** only poll-mode ports (ClearCore) call `eip_adapter_poll()`.
  Don't call it on an RTOS port, where a task already runs the adapter.

## What only a person can supply

Ask for these instead of guessing:

- An ODVA vendor id. 65000 is a placeholder.
- The PLC's IP address, tag names and types, and slot.
- On ESP-IDF, the board's Ethernet PHY, pins and RMII clock mode. A wrong value
  builds fine, but the link never comes up.
- Hardware and PLC testing. A successful build says nothing about the network.

## Working on this repository

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

This needs Linux, macOS, WSL, or Windows with Visual Studio (add `--config
Release` to the build and `-C Release` to ctest). `test_loopback` also needs a
non-loopback interface, and TCP and UDP 44818 and UDP 2222 free.

- Keep `src/` free of OS calls: everything goes through `eip_hal.h`.
- Add a test with each behaviour change: `tests/test_core.c` for the codec and
  adapter core, `tests/test_session.c` for scanner sessions, `tests/test_hal.c`
  for the port contract.
- Use `/* */` comments in ASCII, and explain why rather than what.
