# EDS Builder

A Windows tool that writes the EDS file for a QuickEIP adapter. It reads the
identity and assemblies from your project's source, so the EDS matches what
the adapter accepts in a Forward_Open.

- `eds_builder.exe`: the window.
- `eds_builder_cli.exe`: the same scanner, checks and writer for a build script.

## Run

`bin/` holds a built copy for 64-bit Windows, with the Qt 6.10 libraries it
needs: run `bin\eds_builder.exe`. It needs the Microsoft Visual C++ 2015-2022
x64 runtime, which most PCs already have.

## Build

Needs Qt 6 (Widgets) and CMake. With the Qt 6.10 MSVC kit:

```
cmake -S tools/eds_builder -B build-eds -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.10.0/msvc2022_64
cmake --build build-eds --config Release
```

The build runs `windeployqt`, so `build-eds/Release` holds everything
`eds_builder.exe` needs. `eds_builder_cli.exe` needs only `Qt6Core.dll`, which
is in the same folder. To refresh `bin/`, copy that folder's `.exe` and
`.dll` files and its `platforms`, `styles`, `imageformats` and `iconengines`
folders. The Direct3D compiler DLLs, `Qt6Network.dll`, `tls`,
`networkinformation` and `generic` aren't used.

The window follows the Windows light or dark app mode. Set
`EDS_BUILDER_THEME=light` or `dark` to force one.

## Use

1. Choose the project folder and press **Scan**.
2. Pick the adapter definition if the tree has more than one.
3. Fill in what the source can't supply: the vendor name, and optionally a
   catalog number, home URL and icon.
4. Fix anything in the issue list. **Save EDS** asks before it writes a file
   that has errors.

From a script:

```
eds_builder_cli <project> --list
eds_builder_cli <project> --pick 1 --vendor-name "My Company" -o MyDevice.eds
```

The exit code is 0 only when a file with no errors was written.

## What the scanner reads

Nothing is compiled. The scanner reads `.c`, `.cpp`, `.cc`, `.cxx`, `.h`,
`.hpp` and `.ino` files, skipping build folders and folders that start with
a dot, and looks for:

| Source | Gives |
|---|---|
| An `EipAdapterConfig` variable: its initializer, and `cfg.field = value;` lines | Vendor id, device type, product code, revision, product name |
| An `EipAssemblyConfig` array, positional or designated | Assemblies |
| `eip_adapter_set_identity()` and `eip_adapter_add_assembly()` calls | The same, for the sans-I/O API |
| `eip_adapter_start(0)` | The built-in test identity and assemblies |
| `EIP_ADAPTER_MAX_ASSEMBLIES`, `EIP_ADAPTER_DATA_POOL` from headers or `-D` in build files | Limits the checks use |

Values may be literals, `#define`s, enum constants, `const` integers, and
arithmetic on those. A value the scanner can't work out, such as `sizeof(x)`,
appears as its source text in the Assemblies tab with an error: type the
number over it.

## What it writes

| Section | Content |
|---|---|
| `[File]`, `[Device]`, `[Device Classification]` | From the Device tab |
| `[Params]` | `Param1`, the RPI. `Param2`, a placeholder, when the config assembly has no data. |
| `[Assembly]` | One `AssemN` per assembly, as a single block of bytes |
| `[Connection Manager]` | One exclusive owner connection per consumed/produced pair. One input only connection per produced assembly, when the project has a size-0 consumed assembly to act as the heartbeat. |
| `[Port]`, `[Capacity]`, class sections | Fixed by the stack: one Class 1 connection, no Class 3 |

The adapter has no listen only connection, so the EDS offers none.

## Checks

The issue list covers two things:

- The EDS rules that EZ-EDS enforces: required fields, `MajRev` 1-127,
  `MinRev` 1-255, `ProdCode` and `VendCode` 1-65535, unique entries, an icon
  in `.ico` format.
- What the adapter accepts: each connection point exists with the right
  direction, sizes fit a Forward_Open, and the assemblies fit
  `EIP_ADAPTER_MAX_ASSEMBLIES` and `EIP_ADAPTER_DATA_POOL`.

A file with no errors is valid by these rules. Import into your PLC software
is still the real test.

## Limits

- Assemblies are described as one block. The EDS has no per-field parameters,
  so a PLC shows the data as an array of bytes.
- Config data is described by size only.
- One RPI range is shared by every connection.

`tests/fixture` is a small project the scanner is checked against: macros, an
enum, designated initializers, a 16-bit instance and a `sizeof` it must
report as unresolved.
