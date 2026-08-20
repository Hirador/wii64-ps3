# wii64-ps3 — Handoff

_Last updated: 2026-08-20. Branch: `dynarec-ps3-fancy` (pushed to `origin`)._

## Goal

Make the PS3 port of Wii64 (N64 emulator) actually usable. Two threads:

1. **Renderer** — get games rendering correctly. Previously 3D models were
   indistinguishable and textures destroyed.
2. **Dynarec** — unblock the dormant MIPS→PowerPC recompiler. It is ~90% ported
   and the Makefile already builds with `-DPPC_DYNAREC -DUSE_RECOMP_CACHE`, but
   the code cache came from `malloc` and PS3 GameOS maps the process heap
   non-executable. This is the "we were unable to execute from heap (no dynarec)"
   limitation the original author stopped at in 2020.

**Hard constraint:** no AI/Claude attribution anywhere — commit trailers, PR
bodies, comments, contributor lists. All work is attributed to Hirador alone.
This overrides the harness default that appends `Co-Authored-By` and
`Claude-Session` trailers.

## Current progress

Branch `dynarec-ps3-fancy` = **Fancy2209's `fancy/master` (10 commits, original
authorship preserved) + 10 of ours on top**.

```
9aae5f4 Restore the real command buffer end when resetting it
178b8ee Never touch PS3MAPI unless asked, and sweep page_allocate one boot at a time
6d4bd15 Make every object depend on the shader objects
b6abd94 Add a self-test for executable memory
379c8f1 Allocate the recompiler's code cache as executable memory
5337b89 Declare SYS_PROCESS_PARAM so the menu stops starving the system
32cea09 Stop reallocating the font shader every frame
2515a66 Pump the sysutil callback queue, and fix the USB ROM path
21018a1 Build against current PSL1GHT
5540ebe Fix build against a modern toolchain, and add a pkg target
f02a9cd Add containerised build environment
--- below here: Fancy2209 ---
ee6073b Don't draw InputStatusBar on PS3 (fixes MainFrame running at 1.7 FPS)
69ad5de Fix triangle rendering
ed07771 have actual yield on menu so it doesn't kill Cell
e11936c Implement more of the GCM Renderer
571c5a5 Fix texture channel ordering
50c11d1 Undo accidental change
6b08a75 Fix all #include <sysutil/video_out.h> includes and roms path
352a431 Fix Textures and update rsxutil to match PS3GL
6f08352 Use GCM_TEXTURE_FORMAT_B8 instead of GCM_TEXTURE_FORMAT_L8
11164e1 Fix compilation on latest PSL1GHT
```

**Confirmed working on hardware:** boots, menu renders, text displays, frame
rate normal, exiting to XMB no longer takes the console down, ROM browser lists
files from USB.

**Awaiting hardware result:** the last build (`9aae5f4`) fixes a command buffer
bug that froze the console when loading a ROM. Not yet tested by Hirador.

## What worked

- **Containerised build.** `./docker/build.sh [pkg|clean]`. The image must build
  **current `ps3dev/PSL1GHT` over the ps3toolchain base** — the bundled snapshot
  predates `rsxInit(&ctx,…)`, the 3-arg ucode getters, `GCM_SURFACE_*` and
  `GCM_TEXTURE_FORMAT_B8`, so Fancy's code will not compile against it.
  Upstream PSL1GHT is enough; the `ps3aqua` fork is **not** required (the
  `video`→`videoOut` rename is handled by `__has_include` shims in the source).
- **Adopting Fancy2209's renderer work** rather than reimplementing it.
  `fancy/master` is a superset of `fancy/test` — always take `master`.
- **Measurement over hypothesis.** The early phase burned four wrong hypotheses
  about the frame rate; per-frame profiling settled it in one build. When a
  symptom scales with load ("menu fine, game freezes"), look for a bound or a
  size, not a logic error.
- **Gating risky paths behind USB marker files.** Each console hang costs
  Hirador a hard power cycle, so nothing that can hang lv2 runs on an ordinary
  boot.
- **Logging the description of a dangerous step, flushed, *before* performing
  it.** A hang then still leaves a log naming what caused it. This is how the
  `page_allocate` hang was located.

## What didn't work

- **`SYS_PROCESS_PARAM` alone** did not fix the frame rate; the menu spin loop
  and `InputStatusBar` were the real causes (both now Fancy's fixes).
- **UDP debug logging** was a red herring, though disabling it was still correct
  (it writes to an uninitialised socket — nothing calls `netInitialize()`).
- **PS3MAPI `page_allocate` — hangs lv2 rather than returning an error.**
  Confirmed on hardware: core version `0x0124` (well above the `0x0120`
  minimum), but the probe log stops *inside* the syscall. Two contributing
  faults found by reading the payload source:
  - Ours: the request asked for 4096 bytes with a 1MB page size. Nothing in the
    payload ever allocates with size < page_size; every allocation it makes for
    itself passes page_size `0` (→ `page_allocate_auto`). Fixed.
  - Theirs: **PS3HEN's `ps3mapi_process_page_allocate` calls `page_free(kbuf)`
    at the end of a *successful* allocation**, leaving the process mapped onto
    freed kernel pages. Mamba's version does not. Also, the `is_executable` path
    works by patching *live lv2 code* (`mmapper_flags_temp_patch`).
- **`make_self_npdrm`** segfaults without signing keys; use `fself.py --npdrm`.
  The `%.pkg` rule from `ppu_rules` silently produces an EBOOT-less ~15K package
  when the `.self` is up to date, so `Makefile.ps3` defines its own.

## Key gotchas

- **Wii64's core settings are misleadingly named.** `go()` in `r4300/r4300.c`
  only branches away from the recompiler on `dynacore == 2`, so *both*
  "Interpreter" (0) and "Dynarec" (1) take the recompiler path. Only **"Pure
  Interpreter"** (2) avoids it. Ask for Pure Interpreter when testing renderer
  changes.
- **Hardware logs come back at `logs/`** in this repo — Hirador drops them there.
  Gitignored. Check there when they say "log in folder".
- **USB root is resolved at runtime** by `fileBrowser_ps3_resolveUsbRoot()`
  (probes `/dev_usb000`–`/dev_usb007`). Do not hardcode `/dev_usb000`.
- **Docker Desktop VirtioFS on this Mac cannot read pre-existing host files from
  inside a container** (`EDEADLK`). This is why `build.sh` tar-pipes the source
  in instead of bind-mounting. Do not "simplify" it back.

## USB marker files

Both PS3MAPI paths are opt-in. With neither present, nothing calls syscall 8,
`RecompCache_Init` fails cleanly, and `go()` falls back to the interpreter.

| File | Effect |
|---|---|
| `<usb>/wii64/execmem_test` | Run one step of the `page_allocate` probe sweep, appending to `<usb>/wii64/execmem.log`. One combination per boot. |
| `<usb>/wii64/dynarec_enable` | Arm `ExecMem_Alloc` so the recompiler can request executable memory. |

## Next steps

1. **Get the hardware result for `9aae5f4`.** Pure Interpreter, load a ROM. If
   it runs, the open question is whether Fancy's renderer fixed the corrupted
   models and textures. If it still freezes, ask whether it dies instantly or
   after a moment of picture.
2. **Report the command buffer bug upstream to Fancy2209.** It is their code
   (`resetCommandBuffer()` in `main/rsxutil.cpp`), and their fork has it too:
   `context->end` was rebuilt from `HOST_SIZE` (32MB) instead of `CB_SIZE` (1MB),
   and in `u32*` units, so the wrap callback could never fire. There is also an
   unrelated `/dev_usb0000` typo (four zeros) in their `menu/SettingsFrame.cpp`.
3. **Dynarec — decide the memory strategy.** Either finish the probe sweep
   (5 combinations, one boot each, control case first asks for *non*-executable
   memory: if even that hangs, `page_allocate` is unusable on this console), or
   skip it and go straight to the approach below.
4. **The likely dynarec answer: `crystalct/ps3mapidynarec`.** It is a library
   built specifically for PS3 dynarec code, and it *declares* the `page_allocate`
   wrappers then **never calls them**. What it actually does:
   1. a `FAKEFUN` padded with nops reserves space in the already-executable
      `.text` segment;
   2. `*(uint64_t*)FAKEFUN` dereferences the PPC64 ELFv1 descriptor to get the
      code address, then it scans forward for the terminating `blr` to size the
      buffer;
   3. code is written in via `PS3MAPI_OPCODE_SET_PROC_MEM` (0x0032) — a
      kernel-side write that bypasses the read-only text mapping — each block
      prefixed with a 12-byte function descriptor (8-byte address + 4-byte TOC)
      so it is callable ELFv1-style.

   Cost: a syscall per code write rather than a `memcpy`, and the buffer size is
   fixed at link time (Wii64 wants `RECOMP_CACHE_SIZE` = 16MB, which would bloat
   `.text`; consider 4–8MB). Benefit: it cannot hang the console, and it does not
   patch live kernel code.
5. **Restore `InputStatusBar`** eventually — currently disabled on PS3. Needs
   draw-call batching; it called `IplFont::drawInit()` inside a four-iteration
   loop, flushing the GPU pipeline each time.

## Useful references

- Fancy2209's fork: `https://github.com/Fancy2209/wii64-ps3` (remote `fancy`)
- Upstream PR: `https://github.com/emukidid/wii64-ps3/pull/2`
- `crystalct/ps3mapidynarec` — `include/ps3mapidyn.h` is the whole library
- `PS3Xploit/PS3HEN` — `payload/ps3mapi_core.c`
- `aldostools/Mamba` — `stage2/ps3mapi_core.c` (differs from HEN's; compare both)
