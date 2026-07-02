# YabauseVita — Milestone 0 porting notes

Base: Yabause 0.9.10 (2009), the source you provided.

## What this milestone actually is

A **walking skeleton**, not a playable emulator. The goal is only: does the
Yabause core boot on real Vita hardware. Nothing about this is fast, and no
game will run yet (CDCORE_DUMMY = no real disc reading is wired up).

What it uses, and why:

| Subsystem | Core selected | Why |
|---|---|---|
| SH-2 CPU (x2) | `SH2CORE_INTERPRETER` | Pure C, in `sh2int.c`. The PSP's `psp-sh2.c` is a hand-written MIPS JIT — literally emits MIPS machine code — and cannot run on ARM at all. Writing an ARM JIT is Phase 3+, not Milestone 0. |
| Video (VDP1/VDP2) | `VIDCORE_SOFT` | Pure C rasterizer in `vidsoft.c`, draws into a plain memory buffer (`dispbuffer`). No GPU calls at all. The PSP's `psp-video.c` uses `sceGu` (PSP-only); Vita's equivalent is `sceGxm`/vitaGL, which is Phase 4 work. |
| Sound (SCSP + 68k) | `SNDCORE_DUMMY` / `M68KCORE_DUMMY` | Silence. `q68`'s 68k core also has a PSP/x86-only JIT (`q68-jit-psp.S`), same problem as the SH-2 JIT. |
| Input | `PERCORE_DUMMY` | No buttons do anything yet. Phase 2 work (`sceCtrl`). |
| CD/disc | `CDCORE_DUMMY` | No ISO loading yet. Phase 2 work. |

## The port interface (`yui.h`) — smaller than you'd expect

Every Yabause port only has to implement 4 functions:
`YuiErrorMsg`, `YuiSetVideoAttribute`, `YuiSetVideoMode`, `YuiSwapBuffers`.
That's it — the emulation core calls these, everything else is internal.

## The CoreList pattern

Each port's `main.c` must define six arrays — `SH2CoreList`, `VIDCoreList`,
`M68KCoreList`, `PERCoreList`, `SNDCoreList`, `CDCoreList` — one entry per
core the port compiles in, NULL-terminated. `YabauseInit()` searches these
by numeric ID (the `yinit.xxxcoretype` fields) to find the right one. This
is exactly how you add a new subsystem later: implement it, add it to the
matching list, done.

## Known risk for the first CI build

`vidsoft.c` and `vidshared.c` both `#include "ygl.h"`, and `ygl.h`
unconditionally pulls in SDL (`<SDL/SDL.h>`) on non-Windows platforms. This
will likely be the first compile error. The fix, when we see the exact
error: either stub the few symbols `ygl.h` actually needs, or install SDL2
via vdpm (VitaSDK has a port). We'll know which once the real compiler
tells us — same approach that worked for DSVita.

## Legal note on the BIOS

`yinit.biospath = NULL` uses **HLE BIOS emulation** (best-effort, no real
Sega BIOS required) so Milestone 0 can be tested without a copyrighted BIOS
dump. This won't be as compatible as a real BIOS. Sega's BIOS is
copyrighted — you'd need to legally dump it from your own hardware for
later phases; this project cannot include or download one for you.

## Roadmap after Milestone 0 boots

1. **Phase 1** (this milestone): confirm the core boots on hardware.
2. **Phase 2**: real input (`sceCtrl`), real audio (`sceAudio`), real ISO
   loading (`CDCORE_ISO`, already portable C in `cs2.c`).
3. **Phase 3**: replace `sceDisplay`-only blit with `vitaGL` for filtered
   upscaling (the resolution/blur question) — same technique DSVita uses.
4. **Phase 4**: the big one — a real ARM dynamic recompiler for the SH-2
   cores, replacing the interpreter. This is where actual playable
   performance comes from. Expect this to be the largest single phase.

## How to build

Same GitHub Actions flow we used for DSVita: push this repo, the workflow
in `.github/workflows/build.yml` installs VitaSDK via `vdpm`'s
`install-all.sh` (the same fix that solved every one-off package error we
hit before) and builds with CMake. Expect several rounds of real compiler
errors before it links — that's normal for a first bring-up, not a sign
something is wrong with the approach.
