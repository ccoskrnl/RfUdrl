# RfUdrl

A reflective DLL loader built on [Crystal Palace](https://tradecraftgarden.org/crystalpalace.html),
starting from Tradecraft Garden's `simple_rdll` and adding the runtime setup that
`simple_rdll` leaves out.

## Why this exists

`simple_rdll` loads a DLL and jumps to it, but it never:

* registers the image exception directory, so the OS cannot unwind through the
  loaded image and every frame in a stack walk shows as a bare address;
* runs the TLS callbacks;
* applies per-section page protection, so the image stays RWX for its lifetime.

This project adds those three and is the base for further tradecraft.

## Build

```
make x64
cpl link loader.spec /path/to/capability.dll out.bin -g out.yar
```

`out.bin` is the PIC. `-g` writes Yara rules for the invariant parts of the
output, which is worth reading early: it shows exactly what a defender can sign.

## How the setup is wired in

`src/loader.c` is structurally identical to `simple_rdll` and does not reference
the runtime setup at all. The setup is applied by `loader.spec`:

```
redirect "ProcessImports" "rdll_process_imports"
preserve "ProcessImports" "rdll_process_imports"
```

`rdll_process_imports` calls the real `ProcessImports` and then does the setup.
`preserve` is required, otherwise the call inside `rdll_process_imports` would be
rewritten to itself and recurse forever. This is the same pattern the reference
loaders use to break their `LoadLibraryA` recursion.

Because the redirect is on the function, any caller of `ProcessImports` gets the
setup, not just our `go()`.

### Why ProcessImports and not LoadDLL

Order matters. A real load is: map, relocate, resolve imports, register the
exception directory, run the TLS callbacks, then call the entry point. The TLS
callbacks run the image's own code, which may call imported APIs, so imports have
to be resolved first. Wrapping `LoadDLL` would run the callbacks before that and
break any image whose TLS callback uses an import.

## Verification

Measured on Windows, comparing `simple_rdll` and this project loading the same
payload. Everything else in each payload log is identical, so the differences
below are attributable to the runtime setup alone. Both the direct-call and the
redirect form give the same results.

| what | `simple_rdll` | this project |
| --- | --- | --- |
| TLS callback ran (`tls_flag`) | `0` | `1` |
| header page | `RWX` | `R` |
| `.text` | `RWX` | `RX` |
| `.rdata` / `.pdata` / `.idata` / `.reloc` | `RWX` | `R` |
| `.data` / `.bss` / `.tls` | `RWX` | `RW` |
| `RtlLookupFunctionEntry` on own code | not found | found, correct image base |

The last row is the exception directory check: a payload calls
`RtlLookupFunctionEntry` on an address inside itself. Without registration the
lookup returns nothing, which is what makes a stack walk through the image fail.

Sizes, with a 104388 byte test DLL as input:

| | bytes |
| --- | --- |
| `simple_rdll` PIC | 106883 |
| this project, direct call | 107969 |
| this project, redirect | 108001 |

## The DFR hash convention (verified, and easy to get wrong)

`dfr "resolve" "ror13"` turns `MODULE$Function` into a resolver call with two
constants. They are **not** hashed the same way:

| part | hashed input | example |
| --- | --- | --- |
| module | the module name as **UTF-16LE bytes** (interleaved zero bytes included), lowercase folded to uppercase | `KERNEL32.DLL` -> `0x6A4ABC5B` |
| function | the function name as **plain ASCII** | `VirtualProtect` -> `0x7946C61B` |

Checking this matters because DFR does not verify at link time that the module
exists. A wrong constant links fine and fails silently at run time. The module
side matches LibTCG's `hash_module`, which walks the PEB and hashes
`BaseDllName.pBuffer` byte by byte with `Length` in bytes, which is why the
UTF-16 form is the right one.

To check a build, read the `.yar` it emits:

| symbol | constant |
| --- | --- |
| KERNEL32 module | `0x6A4ABC5B` |
| NTDLL module | `0x3CFA685D` |
| VirtualAlloc | `0x91AFCA54` |
| VirtualProtect | `0x7946C61B` |
| RtlAddFunctionTable | `0x22FC1661` |

## Layout

| path | what |
| --- | --- |
| `loader.spec` | Crystal Palace link script |
| `src/loader.c` | entry point (`go`), appended DLL handling |
| `src/rdllsetup.c` | TLS, exception directory, per-section protection |
| `src/tcg.h` | copy of LibTCG's interface header |

`src/tcg.h` is a copy of `libtcg/src/tcg.h` with one local change: upstream has
no include guard, so `#pragma once` was added at the top. Without it, any header
that also includes `tcg.h` produces redefinition errors.

## Dependencies

Built against the Tradecraft Garden tree it sits in:

* `../libtcg/libtcg.x64.zip`
* `../simple_pic/services.spec`

Build both with `make x64` in their own directories first. Both need `zip` on
PATH.

## Not done yet

The higher layers: module overwrite, entry transfer, call stack spoofing and
sleep masking.

## Attribution

This project builds on other people's work and keeps the same lineage.

* `src/tcg.h` is a copy of `libtcg/src/tcg.h` from the Tradecraft Garden,
  Copyright 2025-2026 Adversary Fan Fiction Writers Guild, BSD 3-Clause. Its
  copyright header is retained in the file, with one local addition (`#pragma
  once`, because upstream has no include guard).
* `src/loader.c` follows the structure of the Tradecraft Garden's `simple_rdll`.
* The DLL parsing and loading primitives are LibTCG's, which is itself derived
  from Stephen Fewer's ReflectiveDLLInjection (BSD).
* Crystal Palace, the linker that builds this, is by Raphael Mudge.

This repository has no license of its own yet; pick one before publishing it.
