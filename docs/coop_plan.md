# Co-op plan — G-Force (Eurocom EngineX)

> **Historical document.** This file started as a split-screen research plan.
> The current implementation is a networked local/IP or Steam-P2P co-op
> experiment with one local P1 in each process and the peer represented as P2.
> For current facts, first use `tools/coop_v2/IMPLEMENTATION.md`, then
> `re_cache/RE_CATALOG.md`, and finally the detailed `tools/coop_v2/README.md`.
> The old player-pool, `0x5bc600` and `XCameraMode_SecondPad` assumptions below
> must not be used as implementation requirements.

## Current implementation status (2026-09-05)

- The mod targets one verified x86 retail `GForce.exe` build and uses injected
  `winmm.dll` → `coop_dll.dll` startup.
- `Player2Module` hooks the native GPig factory at `0x545370`, captures the P1
  spawn context and creates P2 on the game thread. The local test path uses
  `F5`; the network path queues P2 after the handshake. `F6` first ensures that
  local P2 exists, then requests native ABR for P1.
- `CoopNetGame` captures local input and sends it through standalone GNS or
  Steam P2P. The peer's snapshot is applied only during scoped native updates:
  remote P2, and the one shared Mooch while the peer owns it.
- P1 remains the shared camera target. World and save synchronization are
  handled by `WorldSync` and `SaveSync`.
- Ordinary on-foot P2 now has a post-motor distance recovery after retail
  scheduler `0x43C9E0`: immediate at a large divergence and periodically at a
  smaller residual. It keeps native ledge/fall physics active and is excluded
  from ABR/RDV by packet mode plus both local controller modes. This route is
  compiled but **not live-tested**.
- P2 spawn/default mode, camera ownership and remote fire have been exercised;
  movement, torso/weapon aim and authoritative damage/NPC/script behavior are
  still open work. Mooch now sends position plus rotation and all four native
  analog axes to its remote steering tick. Its owner yaw is sampled after the
  native Fly tick, while P1 avoids overwriting that sample during local Fly
  ownership. `Fly_Deactivated` (`0x61000075`) clears only the local owner's
  packet state. While a peer still publishes a live Mooch, a narrow dispatcher
  guard stops only the receiver's exact Mooch request for Deactivated before its
  destructive `Enter`; after the newer zero-owner snapshot arrives, stock
  Deactivated/Respawn is allowed again. Turn and death recovery still require a
  live two-process test.

### Current debug controls

- `F1` works in any foreground process with a live Mooch: it locally selects
  `Fly_Scanning` (`0x61000087`) and injects one answer at each exact native
  input site: the fire-level call `0x005B6B14` / return `0x005B6B19` and the
  `Fly_Active` raw pressed-edge call `0x005B631C` / return `0x005B6321`.
  It has no owner, client, peer-packet or camera/P1 precondition; native
  projectile outcome is **not tested**.
- `F2` uses a registered spawn-definition trigger, `F3` replays an observed event,
  and `F4` uses only the confirmed ComputerBox activation event `0x41080022`.
- `F7` is an experimental pause bypass. It installs focus/minimize hooks for the
  foreground game window. The frame-loop pause-state contract is statically
  approved in `re_cache/RE_CATALOG.md`; its background runtime result is not tested.

The active implementation is not split-screen. The old material below is kept
because it records useful reverse-engineering observations, but it is not a
plan for the current milestone.

## Historical split-screen hypothesis
Engine already has the plumbing for LOCAL multiplayer:
- XInput pads 0..3 (`EXGamePad::StoreJoyStateXInputController, Invalid XInput pad index '%d' (should be 0...3)`).
- `XCameraMode_SecondPad`, `XCameraMode_Third*` camera modes exist.
- Input enumeration string `"2 Player"` + a table of known controllers (Logitech, Twin USB, PSX adaptor…) in `sub_677D95`.
At the time this was written, online was considered out of scope because the
game had no usable netcode. That conclusion is superseded by the current GNS
and Steam-P2P transport in `tools/coop_v2/ServerClient/`.

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

## Superseded status snapshot (2026-08-21)
- **Runtime-confirmed** (read-only scan DLL, `tools/coop_test/`): the single player is `XController_GPig`,
  created at boot, living in an engine object pool with `count=1` at `pool+0x410`. Two player-pointer
  slots exist at `pool+0xE30` (P1) and `pool+0xEAC` (P2); both currently point at the one object.
  A manager (separate heap, e.g. `0x133BF170`) references player/pool/count.
- The "2 Player" string is only in the input layer (`0x677D95`) — no dormant flag found to flip; co-op =
  spawn a real 2nd `XController_GPig` into the `0xEAC` slot + `count=2`.
- The factory is statically invisible (zero vtable xrefs in IDA) → spawn path is being captured at
  runtime via the DLL's safe vtable-method hook (`caller=` lines in the log).

## Superseded open questions
1. **Spawn path**: which engine function allocates/registers a `XController_GPig`? (Being traced from the
   `caller=` addresses captured by the runtime DLL.)
2. **Camera per player**: how `XCameraMode_SecondPad` / `Third` get instantiated + how viewports are set.
3. **Input mirror**: bind player 2 to the same keyboard (per user goal) via `0x67A4C3` per-pad read.

## Superseded phased plan
- **Phase 0 (done)**: unpack data; map input architecture.
- **Phase 1 — find the co-op gate**: locate the "2 Player" mode handling + player-factory + camera-manager. Decompile. Hypothesis: a player-count / co-op-enabled flag exists.
- **Phase 2 — minimal patch**: if a flag gates it, flip it (data/code patch) → test. If not, patch player-factory to spawn 2nd XGPig bound to pad 2, and camera-manager to attach `XCameraMode_SecondPad` + split viewport.
- **Phase 3 — runtime validation**: use the `gforce-tools` x32dbg debug-printer patch on `gforce.exe.unpacked.exe` (Steam-unpacked) for logging; or instrument via a DLL injection / code cave in retail.
- **Phase 4 — polish**: camera framing for 2P (reuse `0x63919B` bbox), HUD, pause/split handling, save compat.

## Historical key addresses (retail GForce.exe, VA=0x400000+fileoff)
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

## Current implementation anchors

- GPig factory: `0x545370`
- GPig array: `0x9128D8`; entity handler: `+0x144`; position: `+0xE8`;
  rotation: `+0xC8`
- Controller: `handler + 0x510`; owner: `+0x18`; current mode: `+0x1C`;
  mode ID: `mode + 0x08`
- Camera manager: `0x915738`; camera handler: `0x515C80`; camera refresh:
  `0x5B03A0`
- Current wire input size: `sizeof(CoopInput) == 336`; it includes Mooch's
  full position/rotation transform, four native analog input axes and the
  ordered F1 debug-fire sequence, but no ABR motor fields. See
  `tools/coop_v2/ServerClient/MTypes.h`.
