# RfUdrl

A reflective DLL loader built on [Crystal Palace](https://tradecraftgarden.org/crystalpalace.html),
starting from Tradecraft Garden's `simple_rdll` and adding the runtime setup and
entry transfer that `simple_rdll` leaves out.

## Why this exists

`simple_rdll` loads a DLL and jumps to it, but it never:

* registers the image exception directory, so the OS cannot unwind through the
  loaded image and every frame in a stack walk shows as a bare address;
* runs the TLS callbacks;
* applies per-section page protection, so the image stays RWX for its lifetime;
* keeps a return address into the PIC off the stack, so a stack walk ends in
  fileless memory that belongs to no module.

This project adds all four.

## Build

```
make x64
cpl link loader.spec /path/to/capability.dll out.bin -g out.yar
```

`out.bin` is the PIC. `-g` writes Yara rules for the invariant parts of the
output, which is worth reading early: it shows exactly what a defender can sign.

## How the runtime setup is wired in

`src/loader.c` is structurally identical to `simple_rdll` and does not reference
the runtime setup at all. It is applied by `loader.spec`:

```
redirect "ProcessImports" "rdll_process_imports"
preserve "ProcessImports" "rdll_process_imports"
```

`rdll_process_imports` calls the real `ProcessImports` and then does the setup.
`preserve` is required, otherwise the call inside `rdll_process_imports` would be
rewritten to itself and recurse forever.

Because the redirect is on the function, any caller of `ProcessImports` gets the
setup, not just our `go()`.

### Why ProcessImports and not LoadDLL

Order matters. A real load is: map, relocate, resolve imports, register the
exception directory, run the TLS callbacks, then call the entry point. The TLS
callbacks run the image's own code, which may call imported APIs, so imports have
to be resolved first. Wrapping `LoadDLL` would run the callbacks before that and
break any image whose TLS callback uses an import.

## How the entry transfer works

`go()` does not call the entry point. It hands it to `rdll_transfer_execution`,
which builds a synthetic stack bottom and enters the entry point with
`NtContinue`. `NtContinue` restores registers from a `CONTEXT` and pushes nothing,
so no return address into this PIC is ever placed on the stack.

Two things here were found the hard way and are easy to get wrong.

### The synthetic frames must live on the thread's own stack

Building them in their own `VirtualAlloc` looks tidier and does not work. RSP then
falls outside the thread's `TEB.StackBase` / `StackLimit`, and the OS walker
refuses to walk it at all. Measured:

| | RSP in TEB bounds | frames reported |
| --- | --- | --- |
| frames in a separate allocation | no | `0` |
| frames carved from the live stack | yes | walk proceeds |

Zero frames is worse than leaving the return address there, so the frames are
carved out of the live stack instead, `RDLL_STACK_MARGIN` below the current RSP.

### Each frame sits one 8 byte slot higher than the frame size suggests

The unwinder takes the next return address from `[Rsp + frame_size]` and gives
that frame an `Rsp` of `Rsp + frame_size + 8`. Laying the slots out at
`frame_size` alone puts a garbage value in the chain immediately after the first
synthetic frame. The correct layout is one `+8` per level:

```
slot_0 = rsp
slot_1 = slot_0 + 8 + size_of(BaseThreadInitThunk)
slot_2 = slot_1 + 8 + size_of(RtlUserThreadStart)   /* the NULL sentinel */
```

The frame sizes are not guessed. They are computed from the unwind info of the
two functions with the same unwind opcodes the OS unwinder uses, so they follow
whatever the running OS build actually has.

The two `+0x14` and `+0x21` return offsets are the one build specific part. If a
stack walk ever comes out wrong, re-check those first on that OS build.

## Verification

Measured on Windows, comparing `simple_rdll` and this project loading the same
payloads. Everything else in each payload log is identical, so the differences
below are attributable to the additions.

| what | `simple_rdll` | this project |
| --- | --- | --- |
| TLS callback ran (`tls_flag`) | `0` | `1` |
| header page | `RWX` | `R` |
| `.text` | `RWX` | `RX` |
| `.rdata` / `.pdata` / `.idata` / `.reloc` | `RWX` | `R` |
| `.data` / `.bss` / `.tls` | `RWX` | `RW` |
| `RtlLookupFunctionEntry` on own code | not found | found, correct image base |
| stack walk past the entry point | stops, frame is `NO_MODULE` | continues into kernel32 and ntdll |

The stack walk, from a payload that walks with `RtlVirtualUnwind`:

```
frame 0  <payload>                    payload
frame 1  <payload>                    payload
frame 2  <payload>                    payload
frame 3  kernel32 BaseThreadInitThunk+0x14
frame 4  ntdll    RtlUserThreadStart+0x21
frame 5  0                            end of chain
```

`simple_rdll` stops at frame 0 and cannot even name the module.

### A note on the verification tool

`RtlCaptureStackBackTrace` is not usable as a verifier here: it reported a single
frame even for a thread started with `CreateThread`, which should show the whole
chain. The payloads therefore walk manually with `RtlVirtualUnwind`, the
primitive an EDR uses. If a stack looks wrong, check it that way before believing
it.

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

Taking the address of a DFR symbol (`KERNEL32$BaseThreadInitThunk` rather than
calling it) works, which is what the entry transfer needs.

## Layout

| path | what |
| --- | --- |
| `loader.spec` | Crystal Palace link script |
| `src/loader.c` | entry point (`go`), appended DLL handling |
| `src/rdllsetup.c` | TLS, exception directory, per-section protection |
| `src/transfer.c` | synthetic stack and `NtContinue` entry |
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

The higher layers: module overwrite, call stack spoofing of the loader's own API
calls, and sleep masking.

The entry transfer assumes the entry point does not return, the same way a real
thread start does not come back to `RtlUserThreadStart`. An image whose DllMain
returns will return into `BaseThreadInitThunk`.

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