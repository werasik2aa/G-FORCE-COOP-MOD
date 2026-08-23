# Co-op plan — G-Force (Eurocom EngineX), local split-screen 2P

## Why split-screen (not online)
Engine already has the plumbing for LOCAL multiplayer:
- XInput pads 0..3 (`EXGamePad::StoreJoyStateXInputController, Invalid XInput pad index '%d' (should be 0...3)`).
- `XCameraMode_SecondPad`, `XCameraMode_Third*` camera modes exist.
- Input enumeration string `"2 Player"` + a table of known controllers (Logitech, Twin USB, PSX adaptor…) in `sub_677D95`.
Online would need netcode (absent) → out of scope.

## What we have established
- **Data**: full main archive extracted to `extracted/Filelist_v7/` (410 named `.edb`; maps=`io_*.edb`). Format from `gforce-tools` (Swyter).
- **Input architecture** (decompiled, `docs/coop_decomp.txt`):
  - `0x67A4C3` `sub_67A4C3(this, padIndex)` — per-pad XInput read; reads `this[padIndex+913]`, builds per-pad input state. SINGLE hook point to feed pad 2 → player 2.
  - `0x67A77C` `sub_67A77C(this)` — DirectInput device enumeration loop over `this[7852]` devices (struct @ this+3492, 16B stride).
  - `0x677D95` `sub_677D95()` — input processing/calibration; holds the "2 Player"/controller-name table.
  - `0x63919B` `EXItemAnimator_Camera` — computes camera bounding box from up to 5 attached items (loop `v11<5`); relevant for split-screen viewport sizing.
- **Camera/player modes are DATA-DRIVEN** (vtable/static structs), NOT directly referenced from code. `XCameraMode_SecondPad`, `XCameraMode_Third*`, `XGPig` appear only as data. So player-factory + camera-manager live in RTTI/static tables → need deeper RE.
- **Camera class-registration table** at `0x70a9cc`–`0x70b060` (`XCameraManager` name @ `0x70a8dc`). Contains `XCameraMode_Third`, `ThirdShooter`, `ThirdStrafe`, `SecondPad`, `Fly`, `Jetpack`, `RDV`, `Dash`, `GrappleFly`, `Vacuum*`, … → the engine's `EXClassFactory` can instantiate `XCameraMode_SecondPad` by name. Strong evidence 2P split-screen camera was prototyped.
- Player/camera spawn is via factory from **map/script data** (`.edb`), not hardcoded call sites. Reuse the same spawn path for a 2nd `XGPig` + 2nd `XCameraMode_SecondPad` camera.

## Status (2026-08-21)
- **Runtime-confirmed** (read-only scan DLL, `tools/coop_test/`): the single player is `XController_GPig`,
  created at boot, living in an engine object pool with `count=1` at `pool+0x410`. Two player-pointer
  slots exist at `pool+0xE30` (P1) and `pool+0xEAC` (P2); both currently point at the one object.
  A manager (separate heap, e.g. `0x133BF170`) references player/pool/count.
- The "2 Player" string is only in the input layer (`0x677D95`) — no dormant flag found to flip; co-op =
  spawn a real 2nd `XController_GPig` into the `0xEAC` slot + `count=2`.
- The factory is statically invisible (zero vtable xrefs in IDA) → spawn path is being captured at
  runtime via the DLL's safe vtable-method hook (`caller=` lines in the log).

## Open questions (now narrower)
1. **Spawn path**: which engine function allocates/registers a `XController_GPig`? (Being traced from the
   `caller=` addresses captured by the runtime DLL.)
2. **Camera per player**: how `XCameraMode_SecondPad` / `Third` get instantiated + how viewports are set.
3. **Input mirror**: bind player 2 to the same keyboard (per user goal) via `0x67A4C3` per-pad read.

## Phased plan
- **Phase 0 (done)**: unpack data; map input architecture.
- **Phase 1 — find the co-op gate**: locate the "2 Player" mode handling + player-factory + camera-manager. Decompile. Hypothesis: a player-count / co-op-enabled flag exists.
- **Phase 2 — minimal patch**: if a flag gates it, flip it (data/code patch) → test. If not, patch player-factory to spawn 2nd XGPig bound to pad 2, and camera-manager to attach `XCameraMode_SecondPad` + split viewport.
- **Phase 3 — runtime validation**: use the `gforce-tools` x32dbg debug-printer patch on `gforce.exe.unpacked.exe` (Steam-unpacked) for logging; or instrument via a DLL injection / code cave in retail.
- **Phase 4 — polish**: camera framing for 2P (reuse `0x63919B` bbox), HUD, pause/split handling, save compat.

## Key addresses (retail GForce.exe, VA=0x400000+fileoff)
Player / spawn:
- `XController_GPig` ctor/creator: `0x5bc600`
- Player vtable `0x717F14`, base vtable `0x717C44`
- Object pool: heap (randomized per run); player object at `pool+0xF10`; **`count` at `pool+0x410`**
- **Player-1 slot `pool+0xE30`, player-2 slot `pool+0xEAC`** (both → player object)
- Manager object: separate heap (e.g. `0x133BF170`)

Input:
- XInput per-pad read: `0x67A4C3`
- DInput enum: `0x67A77C`
- Input processing / "2 Player" table: `0x677D95`
- Input player-count byte: input-manager `+0x1EAC` (loop at `0x67a9de`)

Camera:
- `XCameraMode_SecondPad` ctor `0x5178d0`, `XCameraMode_Third` `0x526fe0`, `XCameraManager` `0x515ce0`
- Camera bbox: `0x63919B`
- GPig controller default ctor refs: `0x4671b0`, `0x4924a0`
