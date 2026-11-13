# Hardware tests

These files test the QuickEIP adapter on a device. A PC runs the QuickEIP
scanner as the originator. No PLC is necessary.

## Contents

- [1. Test status](#1-test-status)
- [2. Files](#2-files)
- [3. What the test does](#3-what-the-test-does)
- [4. Build the PC program](#4-build-the-pc-program)
- [5. Test an ESP32-S31](#5-test-an-esp32-s31)
- [6. Test a NetBurner module](#6-test-a-netburner-module)
- [7. Read the results](#7-read-the-results)

## 1. Test status

The table gives the results of the last test.

| Device | Tools | Result |
|---|---|---|
| ESP32-S31, internal EMAC, YT8531 PHY | ESP-IDF 6.1 | All checks pass at an RPI of 10 ms. A packet capture shows the stored DSCP values. |
| NetBurner MODRT1171 | NNDK 3.5.8, 20 Hz tick | All checks pass at an RPI of 50 ms. The port has no QoS object. |

The PC was a Windows 11 computer with the Windows port of QuickEIP. The
adapter is not yet tested with a PLC on these devices.

In the capture of the MODRT1171, 3 of 199 packets from UDP port 2222 had the
DSCP value 27. All other packets had the value 0. The NetBurner port does not
set a DSCP value. The cause is not yet known.

## 2. Files

| File | Function |
|---|---|
| `hw_test.c` | The PC program. It does the checks of one stage. |
| `run_hw.ps1` | Runs the stages in sequence. It can also start a packet capture and a serial log. |
| `serlog.py` | Writes the console of a serial port to a file. It needs Python with the pyserial package. |
| `esp32s31/` | Test firmware for an ESP32-S31 board. |
| `netburner/` | Test firmware for a NetBurner module. |

Each firmware has these assemblies:

| Instance | Type | Size | Contents |
|---|---|---|---|
| 150 | Consumed | 16 | Data from the PC. |
| 100 | Produced | 16 | Bytes 0–3 are a counter that increases at 10 Hz. Byte 4 is a copy of byte 0 of instance 150. |
| 151 | Config | 0 | No data. |
| 102 | Produced | 16 | NetBurner only. The counts of the callbacks. |

The ESP32-S31 firmware writes each callback to its console. The NetBurner
firmware puts the callback counts in instance 102, and the PC program reads
them with an explicit message.

## 3. What the test does

The test has three stages. The device restarts between the stages.

| Stage | Checks |
|---|---|
| `a` | ListIdentity, the class revisions, and the default stored settings. A Class 1 connection in run and in idle, then a close. A second connection that the inactivity watchdog closes. The event and indicator callbacks. New DSCP values. An encapsulation inactivity timeout of 3 seconds, which closes a silent session. Identity Reset, type 0. |
| `b` | The stored settings after the restart. The Class 1 checks again. Identity Reset, type 1. |
| `c` | The default stored settings after the factory reset. |

Stage `w` is a warm-up with no checks. `run_hw.ps1` runs it first.

> **Note:** Security software on a PC can drop the first seconds of UDP data
> from a new program, in the two directions. The device then closes the first
> connection after the inactivity timeout. The warm-up stage prevents this
> false failure.

> **CAUTION:** Stage `b` sends a factory reset. The test firmware erases its
> stored settings. Do not run the test on a device that controls machinery.

## 4. Build the PC program

**Procedure: Build `hw_test`**

1. Open a command prompt in the root folder of the repository.
2. Configure and build:

   ```
   cmake -S . -B build
   cmake --build build --config Release
   ```

3. Make sure that the build made `hw_test`. With Visual Studio, the file is
   `build\Release\hw_test.exe`.

## 5. Test an ESP32-S31

The firmware is for a board with the internal EMAC and a YT8531 PHY on RGMII.
The pin numbers are in `esp32s31/main/main.c`. If your board is different,
change the pin numbers before you build.

The firmware uses DHCP. If it gets no address in 10 seconds, it uses
172.16.82.240.

**Procedure: Load the firmware and run the test**

1. Open an ESP-IDF 6.1 command prompt in the folder `tests/hardware/esp32s31`.
2. Build the firmware and load it. Replace the port with the port of your
   board:

   ```
   idf.py -p COM20 flash monitor
   ```

3. Read the IP address of the board from the console.
4. Close the monitor.
5. Open PowerShell in the folder `tests/hardware`.
6. Run the test. Replace the address, the adapter name, and the port:

   ```
   .\run_hw.ps1 -HwTest ..\..\build\Release\hw_test.exe -Ip 172.16.82.153 -Qos 1 -Rpi 10 -Nic Ethernet -Com COM20
   ```

7. Make sure that each stage shows `0 failed`.

## 6. Test a NetBurner module

The NNDK build puts the objects of `../` source files outside the `obj`
folder. Thus, build from a copy that has QuickEIP in a subfolder.

**Procedure: Load the firmware and run the test**

1. Copy the folder `tests/hardware/netburner` to a work folder.
2. In the work folder, make a folder with the name `QuickEIP`.
3. Copy the folders `src`, `include`, and `ports` of the repository into
   `QuickEIP`.
4. Build the firmware in the work folder. Replace the platform with the
   platform of your module:

   ```
   make QUICKEIP=QuickEIP PLATFORM=MODRT1171 DEFPLATFORM=MODRT1171
   ```

5. Load the firmware. Replace the address with the address of your module:

   ```
   nbupdate obj/release/QuickEIPHwTest.bin 10.168.41.12
   ```

6. Open PowerShell in the folder `tests/hardware`.
7. Run the test. Replace the address and the adapter name:

   ```
   .\run_hw.ps1 -HwTest ..\..\build\Release\hw_test.exe -Ip 10.168.41.12 -Qos 0 -Rpi 50 -Nic "Ethernet 2"
   ```

8. Make sure that each stage shows `0 failed`.

> **Note:** `nbupdate` replaces the application on the module. Keep a copy of
> your application before you load the test firmware.

The default NNDK tick is 20 Hz. Thus, use an RPI of 50 ms or more.

## 7. Read the results

`run_hw.ps1` writes its files to the folder `quickeip_hw` in the temporary
folder of Windows. Use `-Out` to select a different folder.

| File | Contents |
|---|---|
| `result.txt` | The checks of each stage. |
| `capture.txt` | One line for each packet: time, source address, DSCP value, TCP source port, UDP source port, and TCP flags. Only with `-Nic`. |
| `serial.log` | The console of the device. Only with `-Com`. |

The option `-Nic` needs Wireshark, because the script uses `tshark`. The
script shows a count of the packets from the device for each source port and
DSCP value.

On a port with a QoS object, these DSCP values are correct:

| Stage | UDP port 2222 | TCP port 44818 and UDP port 44818 |
|---|---|---|
| `a` | 31 | 27 |
| `b` | 26 | 10 |
| `c` | No packets | 27 |

The scanner asks for low priority, so the test uses only the DSCP value for
low priority on UDP port 2222. One TCP packet for each connection has the
value 0. That packet is the SYN-ACK. See
[INTEGRATION.md §3.4](../../docs/INTEGRATION.md#34-optional-callbacks).
