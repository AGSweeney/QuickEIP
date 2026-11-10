# EDS Builder

EDS Builder is a Windows tool that writes the EDS file for a QuickEIP
adapter. It reads the identity and the assemblies from the source files of
your application. The EDS file then agrees with what the adapter accepts in a
Forward_Open.

The tool has two programs:

- `eds_builder.exe` is the window.
- `eds_builder_cli.exe` does the same scan, checks, and output from a build
  script.

## Contents

- [1. Test status](#1-test-status)
- [2. Run the tool](#2-run-the-tool)
- [3. Use the command line](#3-use-the-command-line)
- [4. What the scan reads](#4-what-the-scan-reads)
- [5. What the tool writes](#5-what-the-tool-writes)
- [6. Checks](#6-checks)
- [7. Limits](#7-limits)
- [8. Build the tool](#8-build-the-tool)

## 1. Test status

The table gives the status of each part.

| Part | Status |
|---|---|
| `eds_builder.exe`, `eds_builder_cli.exe` | Runs on Windows 11, 64-bit. |
| Scan | Runs on the three adapter examples and on `tests/fixture`. The results agree with the source files. |
| EDS file in EZ-EDS 3.40 | One file from an earlier version opened with no error list. That file had no `ModDate` entry. The tool now writes `ModDate` and `ModTime`. Not yet tested with those entries. |
| EDS file with config data in EZ-EDS 3.40 | EZ-EDS showed an error list. The cause is not yet known. |
| EDS file in PLC software | Not yet tested. |

> **Note:** EZ-EDS does not find all errors when it opens a file. A file with
> `MinRev = 0` opened with no error list.

## 2. Run the tool

The `bin` folder contains a built copy for 64-bit Windows, with the Qt 6.10
libraries. The programs need the Microsoft Visual C++ 2015–2022 x64 runtime.

**Procedure: Write an EDS file**

1. Start `bin\eds_builder.exe`.
2. Select the folder of your application in the *Project* field.
3. Click *Scan*.
4. If the scan finds more than one adapter definition, select the correct one
   in the *Adapter definition* list.
5. Enter the vendor name on the *Device* tab.
6. If the issue list shows an error, correct it.
7. Make sure that the status bar is green and shows "Valid EDS."
8. Click *Save EDS*.

The source files do not contain a vendor name, a catalog number, a home URL,
or an icon. You enter these on the *Device* tab. Only the vendor name is
necessary.

The *Assemblies* tab shows what the scan found. The adapter refuses a
Forward_Open that names a different instance or size. Thus, change the source
file and scan again. Edit an assembly in the tool only to enter a value that
the scan could not read.

The window uses the light or dark app mode of Windows. To select one, set the
environment variable `EDS_BUILDER_THEME` to `light` or `dark`.

## 3. Use the command line

`eds_builder_cli.exe` takes the project folder and these options:

| Option | Function |
|---|---|
| `--list` | Shows the adapter definitions that the scan found, then stops. |
| `--pick <n>` | Selects a definition from the list. The default is 1. |
| `--vendor-name <text>` | Sets `VendName`. |
| `--catalog <text>` | Sets `Catalog`. The default is the product name. |
| `--icon <file>` | Puts an `.ico` file into the EDS file. |
| `-o <file>` | Sets the output file. Without this option, the EDS text goes to standard output. |
| `--force` | Writes the file although the checks found errors. |

Example:

```
eds_builder_cli <project> --list
eds_builder_cli <project> --pick 1 --vendor-name "My Company" -o MyDevice.eds
```

The exit code is 0 only when the tool wrote a file with no errors.

## 4. What the scan reads

The scan does not compile your application. It reads files with the
extensions `.c`, `.cpp`, `.cc`, `.cxx`, `.h`, `.hpp`, and `.ino`. It ignores
build folders and folders with a name that starts with a period.

The table shows what the scan looks for.

| Source | Result |
|---|---|
| An `EipAdapterConfig` variable: its initializer, and lines of the form `cfg.field = value;` | Vendor ID, device type, product code, revision, and product name |
| An `EipAssemblyConfig` array, with positional or designated initializers | Assemblies |
| Calls to `eip_adapter_set_identity` and `eip_adapter_add_assembly` | The same data, for an application that uses `eip_adapter.h` directly |
| `eip_adapter_start(0)` | The built-in test identity and test assemblies |
| `EIP_ADAPTER_MAX_ASSEMBLIES` and `EIP_ADAPTER_DATA_POOL`, from a header or from a `-D` option in a build file | The limits that the checks use |

A value can be a literal, a `#define`, an enum constant, or a `const`
integer. It can also be arithmetic with these. The scan cannot calculate
`sizeof` or a value that your application calculates when it runs. The
*Assemblies* tab then shows the source text of the value and the issue list
shows an error. Type the number in its place.

## 5. What the tool writes

The table shows the source of each section of the EDS file.

| Section | Content |
|---|---|
| `[File]`, `[Device]`, `[Device Classification]` | The *Device* tab. |
| `[Params]` | `Param1` is the requested packet interval (RPI). `Param2` is a placeholder that the tool adds when the config assembly has no data. |
| `[Assembly]` | One `AssemN` entry for each assembly, as one block of bytes. |
| `[Connection Manager]` | The connections. See the rules after this table. |
| `[Port]`, `[Capacity]`, class sections | Fixed values of the stack: one Class 1 connection and no Class 3 connection. |

These rules apply to the connections:

- The tool writes one exclusive owner connection for each pair of a consumed
  assembly and a produced assembly.
- The tool writes one input only connection for each produced assembly, if
  your application has a consumed assembly of size 0. That assembly is the
  heartbeat.
- The tool writes no listen only connection, because the adapter refuses a
  second originator.
- O→T is point-to-point with the 32-bit run/idle header. T→O has no header
  and can be multicast or point-to-point.

The sizes in the EDS file are assembly sizes, not wire sizes.

## 6. Checks

The issue list shows errors and warnings of two types:

- **EDS rules.** These are the rules that EZ-EDS enforces. Examples are
  necessary fields, `MajRev` 1–127, `MinRev` 1–255, `VendCode` and `ProdCode`
  1–65535, and an icon in `.ico` format.
- **Adapter rules.** Each connection point must be an assembly with the
  correct direction. Each assembly size must be 505 bytes or fewer. The
  assemblies must not exceed `EIP_ADAPTER_MAX_ASSEMBLIES` or
  `EIP_ADAPTER_DATA_POOL`.

The tool shows a warning for vendor ID 65000. That value is a placeholder,
not a vendor ID from ODVA.

A file with no errors obeys these rules. That does not show that PLC software
accepts the file. See section 1.

## 7. Limits

These limits apply:

- The EDS file describes each assembly as one block. It has no parameter for
  each field, and a PLC shows the data as an array of bytes.
- The EDS file gives only the size of the config data.
- All connections use the same RPI range.

## 8. Build the tool

The build needs Qt 6 with the Widgets module, CMake 3.21 or later, and Visual
Studio 2022.

**Procedure: Build the tool and update `bin`**

1. Open a command prompt in the root folder of the repository.
2. Configure the build. Replace the Qt path with the path on your computer:

   ```
   cmake -S tools/eds_builder -B build-eds -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.10.0/msvc2022_64
   ```

3. Build the programs:

   ```
   cmake --build build-eds --config Release
   ```

4. Copy the `.exe` files and the `.dll` files from `build-eds\Release` to
   `tools\eds_builder\bin`.
5. Copy the folders `platforms`, `styles`, `imageformats`, and `iconengines`
   to the same place.
6. Start `bin\eds_builder.exe` and make sure that the window opens.

The build runs `windeployqt`, which puts the Qt files into
`build-eds\Release`. The tool does not use `dxcompiler.dll`, `dxil.dll`,
`Qt6Network.dll`, or the folders `tls`, `networkinformation`, and `generic`.
Do not copy them.

The files in `res` are the icon. `make_icon.py` draws `eds_builder.ico` and
needs Python with the Pillow package.

`tests/fixture` is a small application for a check of the scan. It contains
macros, an enum, designated initializers, a 16-bit instance, and a `sizeof`
that the scan must show as not read.
