# coop_v2 — implementation map

Read this file before changing the mod. It explains ownership and runtime
boundaries. For every retail address, field or inferred function name, use
`../../re_cache/RE_CATALOG.md` instead; it records evidence and whether a fact
is `approved`, a `guess`, or `not-tested`.

## NPC mode sync

Snapshots carry position/rotation/HP but no controller mode, so trigger-less
activation states (saberized lamp dormant `0x4B` → flying `0x20`) never crossed
processes. `WorldMode` (`id + mode`, reliable) broadcasts genuine mode
transitions of host-tracked NPCs on change; the client applies them through
the native dispatcher under SEH. Verified live 2026-09-17: saberized lamp
flies on both screens. AI chatter modes (`0x56`/`0x5A`) also cross; if the peer
AI visibly fights forced modes, add stickiness (apply once per change).

## Scope and executable boundary

- This is a Win32 injected co-op experiment for one fingerprinted retail
  `GForce.exe`, not a general Eurocom EngineX SDK.
- `CoopRuntime::VerifyExecutable` must approve the EXE before any hook is
  installed. A virtual address or field offset is never portable to another
  executable build.
- `gforce_constants.h` holds checked addresses, IDs and entry bytes.
- `retail/` owns raw pointer-plus-offset access, SEH-guarded reads/writes and
  confirmed native calls (`NativeGameApi`), including private `XGamePad`
  construction, native Load Game dispatch, opaque inventory/ammo lookup and the
  paired DirectInput keyboard snapshots. Feature logic uses typed views and
  opaque refs. The small hook/trampoline owners may still manipulate checked
  patch/IAT bytes; that plumbing is not a substitute for a game-object API.

## Runtime structure

```text
winmm.dll proxy
  -> coop_dll.dll / CoopApplication
       -> CoopRuntime (verification, log, safe patches)
       -> MenuConnectHook (native main-menu row -> existing F8 request)
       -> Player2Module (native GPig spawn and controller ticks)
       -> CoopNetGame (packet snapshots, scoped input, hooks)
       -> SharedCameraCoordinator (one retail camera handler)
       -> WorldSync / SaveSync
       -> ServerClient (standalone GNS and Steam P2P)
```

`GForceCoop.sln` owns the production `.cpp` list. Both Win32 projects use the
VS2022 `v143` toolset and C++17 language mode. Build only `Release | Win32`.
Do not add experimental dumps or standalone probes to that solution.

## Connected host save reload

SaveSync hooks the checked entry of `LoadSaveManager_BeginNativeLoad` at
`0x005F1920`, which is shared by both known retail Load Game callers. When an
already connected host selects DATA<n>, the host sends that file reliably before
its own native load starts. The client replaces the same slot and enters the
stock loader from its game thread. The earlier hook at the single menu call
`0x005EDC5D` missed the second retail caller and therefore did not guarantee a
reload in an established session. World pointers are reset immediately before
each native loader runs.

## Progression rally and chat

XTrigger_OB_Cutscene and XTrigger_PL_CheckPoint are the only current
progression-rally candidates. Their exact retail vtables are catalogued; the
cutscene class was also seen on the successful object-event route. After the
original relay, forwarder or trigger dispatcher reports a nonzero result, the
game thread may queue one 60-byte reliable ProgressionRallyPacket. It contains
only a sequence, a reason and the finite P1 root transform—never a retail
pointer, a generic event opcode or a raw trigger identity. The source process
uses that same snapshot to move its P2 presentation; the peer moves local P1
on its next game tick. Duplicate source/event observations are suppressed for
750 ms. Remote object-event replay cannot generate a rally back to its sender.

The rally writer uses retail::EntityView::WriteTransform, including normal
transform-cache invalidation. It does not change controller mode, invent input,
disable physics or write an ABR vehicle root: while either target is in ABR the
packet stays pending. This is narrowly intended to keep a peer with a closing
door/checkpoint or an already-started cutscene from being stranded; it is not a
world-streaming, checkpoint-respawn or universal event-replay solution. Live
two-process proof remains required.

ChatOverlay is a retained non-activating GDI window owned by the game and opened
by F10. Present maintains its position over the client area, while WM_PAINT owns
rendering. The first WS_CHILD replacement was completely hidden by the D3D
surface in live testing. It changes no game D3D render state and needs no Reset
font/resource recovery. Its 216-byte
reliable ChatPacket carries bounded UTF-8 text independently of CoopInput, Fly
abilities and world events. The socket worker only validates and queues text;
P1's game-thread tick collects foreground keyboard characters. While the panel
is visible, local key capture and F1–F9 debug actions are suppressed, so typed
text cannot control remote P2. Esc or F10 hides it. Peer disconnect drops
unsent/incoming chat packets. A received line opens a six-second, non-input,
semi-transparent notification while F10 chat is closed. The retained UI is
build-verified; its behaviour
in both experimental windowed and exclusive-display configurations needs a live
check.

## Native main-menu Connect by IP

`MenuConnectHook` belongs to neither P2, Fly nor ABR/RDV state ownership. After
`VerifyExecutable` accepts the one retail profile, it preserves the whole stock
main-menu builder, observes its live `XHudMenuMain` pointer, and hooks only the
post-`Credits` native `AddChild` call. The original row is added first; then the
stock menu factory, callback ABI and container add-child routine construct one
new row.

The label uses a project-private `XAText` resource ID. Its resolver hook formats
only `Connect by IP` with retail string helpers and forwards every retail label
unchanged. The native callback does exactly one thing:
`CSteamManager::RequestIpConnectionPrompt()`, the same atomic request used by
`F8`. It creates no renderer, does not perform networking, and does not own the
IP dialog. Any byte/vtable mismatch rolls the hooks back and leaves `F8` as the
fallback.

The ABI/entry fingerprints are statically **approved** in
`re_cache/menu_connect_abi.txt`; visible row construction and click delivery are
**not-tested** until a user checks a live main menu.

## Slot discovery versus player support

`retail::EntitySlotRepository` is the single typed path from an already-ticking
controller to the verified native selector slots P1–P3. This is discovery only:
`0x79130003` is statically classified beside P1/P2 as a GPig ID, but no factory
caller or full P3 lifecycle has been traced. The only recovered P1–P3 scan is
inside two virtual `XTrigger_RV_Damage` methods, which is trigger logic rather
than evidence of player creation. `AuxiliaryP3` remains unsupported until its
factory, destruction, input, camera and network ownership have live evidence.
`Player2Module` owns only the P2 spawn/network lifecycle, so locating P3 cannot
accidentally enable it.

The current wire/runtime architecture is deliberately **one remote peer only**:
`CSteamOfflineSocketServer::m_max_players` is `1`, `CoopNetGame` has one
`m_remote_input`, and an input packet carries no sender or player-slot identity.
Do not raise the listener cap to simulate multiplayer; two senders would replace
each other's snapshot and drive the same native P2. Supporting P3 or more people
requires per-peer transport identity, per-slot snapshot ownership and confirmed
native lifecycles before a new server limit is considered.

## Three separate control domains

| Domain | Owner and data | Must not be mixed with |
| --- | --- | --- |
| Ordinary P2 on foot | Native Darwin controller consumes a scoped remote input snapshot. One interpolated root is written before its native update and the identical result is reapplied afterwards, so local physics cannot undo the correction in the same frame. | Fly and ABR vehicle-motor fields. |
| Mooch / Fly | Только owner запускает native Fly state с физическим input и публикует transform и текущий XGamePad aim-ray. Receiver оставляет P1/камеру в обычном состоянии, очищает remote Fly-control flag, применяет root transform, записывает FlyFly target/current yaw+pitch и вызывает его terminal LookAt submitter. | Remote Fly input/controller/camera takeover, Darwin weapon fire, Fly movement axes и обычный P2 controller input. |
| ABR / RDV vehicle | Native `XMotorFunction_GPigRDV` vehicle task/motor owns heading and attached parts. At either ABR boundary, its stock vehicle tick runs; generic P2 input, camera, weapon and smoothed on-foot transform correction stay skipped. The receiver may then write only the finite settled peer root snapshot. | Generic P2 transform recovery, motor-heading guesses or partial root-rotation writes. |

If a packet crosses a mode boundary, prefer skipping one generic correction to
writing a field owned by another domain. In particular, the P2 recovery checks
packet mode, P2 controller mode and P1 controller mode before it touches a
transform. `CoopInput` deliberately carries no ABR motor heading: its `player_mode`
is only a boundary/gate, not a vehicle transform or a license to write the motor.

### Fly lifecycle boundary

`Fly_Deactivated` is a native local transition, not a cross-process death event.
`Fly_Deactivated::Enter` resets local visual and task state, so retaining a
receiver packet *after* that `Enter` cannot restore a live peer presentation.
The narrow `StateMachine_SelectState` (`0x004B7050`) guard therefore intercepts
the target state **before** the dispatcher runs `Exit`/`Enter`. It returns the
dispatcher's normal success result without changing state only when all of these
are true: the target is `Fly_Deactivated` (`0x61000075`), the machine is the
exact `EntitySlot::Mooch` controller, this process is not the local owner, and
the accepted remote input still has nonzero `fly_controlled` plus
`fly_transform_sequence`. It cannot affect P2, ABR, another state machine, or a
local Mooch death. When the owner reaches Deactivated, the original dispatcher
and `ClearLocalFlyOwnershipLocked` remain intact and publish the ordered
zero-owner snapshot. Network ingress records only the raw ownership edge
`fly_controlled: 1 -> 0` and its ordinary input sequence; it does not call retail
code. On the next exact Mooch game-thread tick, if neither side now owns Mooch,
the pending edge is consumed once and `ControllerView::SelectMode(Fly_Deactivated)`
requests the stock receiver transition. A newer peer claim or a local hand-off
clears the pending edge first; a race that arrives after consumption remains safe
because the existing dispatcher guard sees live remote ownership and suppresses
the destructive Enter. This is not a synthetic death or a P2/ABR path: it is the
native Fly lifecycle applied only after the peer's ordered zero-owner state.
The normal retail Fly request first reaches vtable `+0x20` adapter
`0x005BFBA0`; after its native `0x005BFF80` check it directly calls this same
dispatcher. The generic dispatcher itself occupies `XController_Fly` vtable
`+0x10`. In the cached retail image, `Fly_Deactivated::Enter` has only its
mode-vtable `+0x08` data reference and no direct code transfer, so the observed
native request route reaches the guard before that destructive `Enter`. The
dispatcher ABI, route, predicate and destructive-Enter ordering are static
evidence; the queued receiver request and real death/re-entry flow remain
**not live-tested**.

## P2 ledge/fall and outer DeathMode rule

The normal P2 path retains stock collision, Ledge attachment and fall physics.
It does not disable physics, write an inner state, select outer `Default`, snap
the root, or fabricate a physical key. The former post-scheduler reconciler
remains removed: `0x0043C9E0` is an `XTrigger_OB_Conveyor` virtual method, not a
global post-physics scheduler.

After the stock generic `StateMachine_SelectState` call succeeds, the recovery
observer classifies a nested machine from its registered state-vtable set, not
from a reused numeric ID. A machine containing the exact Ledge
Idle/Into/StrafeEnd/Jump vtables is logged as `ledge`; a separate machine with
the exact Climb Drop/Jump child vtables is logged as `climb`. Both use the
observed owner `GPig` reference at `state + 4`. This covers Ledge Strafe/Short
and Climb Drop branches whose own `Update` is a nullsub and which an update-only
observer can miss. A family label is cleared when that same machine selects
`Inactive`; it is diagnostic evidence, not permission to write inner state.

`0x0048AE10` maps second-namespace action `0x4008000A` to ordinary press-edge
`0x1000000D`. During the normal remote-P2 controller tick, co-op offers that
logical edge to the existing `0x00488CE0` input query only if P2 and the peer
snapshot are both in outer `Default`, no local/remote Mooch or ABR path is active,
a finite P2/peer transform divergence stays above 0.5 m for at least 250 ms, and no
real peer edge exists for the same action. The Ledge/Climb observer is diagnostic
only: a stale attachment can itself stop native updates, so it must not veto the
only stock release action known to repair it. The scoped edge is cleared after its
stock tick. There is deliberately no one-shot latch: if P2 remains farther than
0.5 m, a fresh persistence window can offer another release edge every 250 ms.
Stock input decides whether a current state consumes each release; normal transform
smoothing follows as before. The physical binding remains intentionally unknown.

Before an incoming snapshot replaces remote state, the network ingress rejects
`NaN`/`Inf` in P1/Fly transforms, analog axes, aim ray and a valid camera yaw.
The last accepted finite state remains active. The normal P2 transform writer
checks again before its root write. This is containment, not a physics switch.
`[net-input-reject]` and `[net-transform-recovery]` form the diagnostics.
The same finite-only boundary covers `WorldSpawn`/`WorldSnapshot`: host reads and
client ingress reject bad entity transforms before a native root write.

This recovery is build-verified but **not live-tested**. Runtime proof requires
`[p2-attachment-release] queued ...` and `result consumed=1`, followed by a
normal stock exit from the attachment. While the gap persists, another pair after
250 ms is expected rather than suppressed. An optional
`[p2-attachment-observer] family=ledge|climb attachment=1 ...` records the
classified native family; its absence does not suppress recovery.

An outer native `XGPigDeathMode` is separate from that Ledge state machine.
The old P2 guard skipped its stock checkpoint respawn, but could also leave the
remote presentation stuck in that mode’s hidden/death pose. If P2 matches the
checked DeathMode vtable family and the peer has sent a nonzero `Default`
snapshot **newer than P2's DeathMode entry snapshot**,
`ControllerView::SelectMode(Default)` invokes the retail Exit/Enter dispatcher.
`Default` is the registry-confirmed `0x6100003B`, not an inferred replacement
for a Ledge state. P2 then reruns its Default conflict-mask setup; a failed
checked read/write stays uninitialised and is retried (with one throttled
diagnostic) rather than silently claiming completion. A stale packet, peer
Fly/ABR mode, or an inner Ledge state never triggers this route.
The guard applies only while an actual remote peer owns P2; a F5-only local
diagnostic P2 keeps the stock DeathMode flow rather than waiting forever for a
nonexistent snapshot.
It keeps stock collision/fall physics intact. This is also **not live-tested**:
`[p2-death-recovery]` is the runtime proof; `[p2-death-guard]` is an expected
one-packet wait or a wait while the peer is not yet in Default.

## Current confidence and work order

1. Build/static success proves compilation and byte fingerprints only.
2. A two-process live test decides gameplay behavior.
3. Never relabel a `guess` as `approved` after a build alone.
4. When a new reverse-engineered region is used, add its exact evidence to
   `RE_CATALOG.md` before treating it as an implementation contract.

Current unverified routes include remote P2 ledge/fall and outer DeathMode
visibility recovery, standalone F1 dual-laser visual result and live
two-process result of the now-isolated Mooch dual-laser route, Mooch
magnetic-carry route,
retail focus/minimize pause seam, Fly turning/presentation,
peer ABR turning, and P2/P3 scanner/HUD presentation. Do not report any of them
as fixed without a live test.

## Debug keys

`debug_actions.cpp` is the sole F1–F7/F9 dispatcher and owns the F5/F6 local
debug gates; it runs on the game thread after P1's stock tick. The normal
network P2 factory and lifecycle stay in `player2.cpp`. Its tick is outside
`CoopNetGame::GameTick`'s `HasRemotePeer()` early return, so F1 is polled in a
single foreground game even with no client, server, or socket.

The former focus/minimise pause bypass is disabled. Its `FramePauseState` write and
`GetForegroundWindow`/`IsIconic` hooks did not remove retail pause in live use and
could interfere with fullscreen shutdown. `WindowHook` now owns only the opt-in D3D
windowed-presentation path; it never subclasses the game window or alters focus,
minimise or frame-active state. F7 has no co-op action. A real simulation seam
remains an open RE task.

F1 is a local, one-frame dual-laser test rather than the discarded scanner
probe. It reads the current Mooch transform and the foreground P1 XGamePad
aim direction, makes a world target from `P1 ray origin + direction * 100`,
temporarily rewrites the native XGamePad ray so its origin is the current Mooch
center, and invokes the registered `Fly_Active::Update` body. The raw-input
hook supplies one synthetic edge only for the verified Fly laser call site
(`0x40080029` returning to `0x005B60F7`); it does not globally fake the action.
The shadow pass guards state selection and restores the XGamePad ray,
active-entity globals and Fly control flag immediately afterward. Its camera
snapshot includes the full `handler+0x91C..+0x9B7` Fly request/apply window in
addition to the ordinary aim state: `Fly_Active` writes that block before the
laser raw branch, and leaving it behind visibly snaps P1's camera toward Mooch.
It therefore works with no client and without entering Q, provided the level
has a live Mooch, registered `Fly_Active` mode and a valid P1 aim ray. A peer-owned
Mooch remains presentation-only as a controller, but F1 can exercise its local
native ability path without changing owner or peer state. A direct route-item
pulse is retained only as an explicitly logged fallback when the native
fingerprint/registry contract is unavailable. The old `XMotorTask_MoochScan`
experiment remains catalog-only evidence of a misleading RTTI route; it is not
an ability API.

### Mooch dual laser: isolated reliable event

User identifies the two-emitter Mooch attack as its laser. Static analysis
confirms a separate native Fly path: `XControllerMode_Fly_Active::Update` calls
the raw pressed query for `0x40080029` at `0x005B60F2`, returns to
`0x005B60F7`, then iterates two native object ids and sets their stock effect
flag. The same raw action also occurs in a separate P1 WeaponTaser owner
(`0x005882D0`), so a raw-id-wide remote mirror would incorrectly fire P1
weapon code.

`FlyAbilityPacket` is a 44-byte reliable packet with an event sequence, ability
tag, source Fly transform epoch, and three-float world target. On the owner,
the exact `Fly_Active` route reads its current aim ray and constructs that
target before its own raw query. Because the raw query occurs *inside* a Fly
tick and `PublishLocalFlyTransform` follows that tick, the packet carries the
next post-tick transform epoch rather than the previous snapshot. The network
thread only validates and queues the packet. Once the receiver has that matching
Fly transform, its game-thread
Mooch update starts a scoped native pass: it snapshots the current camera
including Fly's request/apply window and active-entity globals, swaps in the
private remote XGamePad, and writes a ray
whose origin is the receiver's current Mooch center and whose direction reaches
the transported target. It then invokes the already-registered
`XControllerMode_Fly_Active::Update` body. The exact raw-input hook returns one
edge at `0x005B60F7`; held/other Fly queries come from the frozen remote input
snapshot. A state-machine guard rejects any transient mode request made during
this shadow update. On return, all scoped camera, active-entity, XGamePad,
remote-input and Fly-control state is restored. The native update itself resolves
the local `Handler -> Inventory` route and arms the two stock laser items, so
the normal item/world reaction chain has a chance to run on the receiver. A
direct `+0x658` route-item pulse remains only as an explicitly logged fallback
for a runtime/profile mismatch. The receiver never selects Fly mode or takes
the local HUD/camera/controller ownership. Independently of a laser event, the
receiver applies the owner's complete native `fly_rotation` unchanged. It does
not splice scalar shared-camera `camera_yaw` into one component: those fields
are distinct native representations, and that splice produced a skewed pose and
laser direction.

The aim-task ABI, item route, packet delivery and ownership boundary are
**approved** static facts. Root `fly_rotation` alone is not the same data as
the native FlyFly body direction. The receiver therefore refreshes the owner
XGamePad ray every Fly tick, calculates the inverse of FlyFly's stock vector
formula (`yaw = atan2(x,z)`, `pitch = -asin(y)`), writes both target/current
angle pairs to its 96-byte state-table entry, marks the direction active, then
calls the byte-checked terminal `XFlyFlyMode_Move` step at `0x005101F0`. That
step creates/updates only the stock controller-local LookAt target; it does not
run a Fly state machine, input query, shared camera/HUD update or ownership
transition. It is safe to use from the guaranteed P1 seam as well as after a
Mooch Idle tick. The heavier scoped `Fly_Active::Update` remains strictly the
one-shot reliable dual-laser replay, not a realtime orientation path. A full `Release|Win32` rebuild passed on
2026-09-09; that proves compilation and matching byte fingerprints, not the
two-process visual result. The magnetic ability remains separate: RTTI names `XFlyCarryMode`,
`XMotorFunction_FlyCarry` and `XMotorTask_Carry` exist, but their current
input edge, target identity and safe replay boundary have not been recovered,
so no carry event is sent.

### GPig scanner / HUD (open)

`XControllerMode_GPig_Scan` is a separate RTTI-named state, registered as
`0x6100000C` by `XController_GPig` at `0x005BE730`. Its `Enter`
(`0x005BC080`) allocates the native scan task, while `Update` (`0x005BC130`)
continues through a shared scan/camera route. These static facts are useful
evidence, but they are not an implementation contract for P2/P3 presentation.
The attempted non-P1 guard did not fix the green shared HUD and is deliberately
absent from this revision. Keep scanner/HUD marked **open** until a two-process
test identifies the actual presentation owner and a replacement is live-tested.

F2/F3/F4 deliberately do not share a generic “activate trigger” path.
`TriggerEventDispatcher` has multiple static low-word event branches, so an event
constant by itself does not define a safe action. F2 uses a spawn-definition
route, F3 only repeats the event actually recorded on that trigger, and F4 checks
the exact verified ComputerBox template before it sends `0x41080022`. Static
`0x41xxxxxx` candidates are evidence to investigate, not keys to expose. For
example, `0x41080021` is statically tied to subtype `0x1F00009E`, but its name,
definition identity and visible effect remain unknown; it is deliberately not an
F4 action.

F9 is the read-only companion to those actions: after map load it prints every
still-live registered factory trigger with its observed identity, position,
distance from P1, flags and last observed event. The only `approved` catalogue
entry is the exact ComputerBox identity. `observed`, `guess` and `unknown` are
explicitly non-authoritative investigation labels and never dispatch an event.

The event hook also logs every retail `TriggerEventDispatcher` call immediately
after the native dispatcher returns, even without a host/client connection. Its
`[trigger-activation]` line carries the observed trigger identity and position,
the exact event and dispatcher result, and a narrow source label: `fly-shadow`
for F1/receiver native Fly passes, `fly-local` for a local controlled Fly event,
otherwise `game`. The label reports route provenance only; it is not a guessed
button name or a success claim about a downstream gameplay reaction.

Map buttons and other interactive objects can bypass `TriggerEventDispatcher`.
The separate byte-gated `GlobalEventForwarder` diagnostic at `0x0046D760` records
only contextual `0x41xxxxxx` fan-out calls as `[global-event]`, including raw
receiver/source pointers, event, native result and P1's position at that instant.
It performs no replay, packet send or memory write beyond its reversible detour.
The P1 coordinate is a test-location aid, not a claimed source-object transform;
this route is intentionally documented as partial until live button tests prove
which callbacks use it.

One live test is now a useful negative result rather than an inference: with the
global diagnostic successfully installed, the currently tested locked and unlocked
buttons emitted neither `[trigger-activation]` nor `[global-event]`. They therefore
bypass both observed downstream routes in that test. This does not classify every
button in the level. To locate their earlier input route, the existing logical
`0x00488CE0` and raw `0x0048AE10` rising-edge hooks record local, non-synthetic
native successes as `[input-edge-local]`. Each record contains the action id and
the exact caller return address, plus the P1 position only as a test-location aid.
It neither changes the result nor sends/replays any packet. F1/receiver Fly native
passes are intentionally excluded because their edges are synthesized by the DLL.

The live green/green-red panel test hit cdecl relay `0x0041E890` and object
forwarder `0x0046D6F0`. It observed `0x41080010`, then a door-chain containing
`0x41080004`, `0x4108007D` and `0x410800A9`. The previous receiver successfully
received and matched the first Door packet and its native relay returned `1`, but
the door was still visibly locked. That proves packet delivery and map matching;
it does **not** prove that the one root call reproduces the whole scripted chain.

`WorldObjectEventPacket` is now a reliable, ordered native-route packet. Route 1
replays the relay; route 2 replays a direct forwarder call. Every successful,
contextual top-level route whose source is a registered non-NPC/non-monster map
template is queued in local order. A relay or forwarder reached from inside another
native object route is intentionally not sent again, because that exact child call
is already made by the remote parent; a later independent relay/forwarder call is
retained. For safety a direct-forwarder packet is
accepted only when its receiver equals the verified literal retail ECX address
`0x00912AA8` (`mov ecx, imm32`, not a dereferenced global).
The peer resolves source by vtable + family/subtype + definition + transform
signature (with only unique fallbacks), calls the matching original route under a
non-echo scope, and logs `unresolved`, `native route fault`, or `applied` rather
than guessing an object. There is no time coalescing of valid object routes.

This is a qualified map-event route, not a claim that arbitrary `0x41xxxxxx`
engine calls are safe to send. It is compiled, but the repaired door chain remains
**not-tested live**.

The current Mooch door run is **observed** as `caller=0x0044C6A3`,
`class=XTrigger_CO_Door`, `event=0x41080010`, `state_flags=0x51`. Static analysis
places that caller in `sub_44BF70`, which tests an object radius against live actors
and emits an enter/leave-style event. Therefore `0x41080010` is not a universal
"open door" command. An unrelated ventilation run already produced the same event
from `caller=0x0040BBB1`, `class=XTrigger_OB_Static`; identity, not the event value,
selects a synchronized root object.

The ventilation trace observed `XTrigger_MO_Blender` and `XTrigger_MO_Mouse` through
the generic map dispatcher (`0x42FAB0`). No `WorldSpawn` trace was present, so that
trace alone is evidence of a map event, **not** proof of a newly created dynamic
mob. `WorldTriggerEvent` still rejects generic NPC/monster dispatcher packets: that
old path could replay the dispatcher on a client that had already run it locally.

NPC/monster map events now use separate routes in `WorldObjectEventPacket`: a client
requests activation with route 4 and continues through its own native dispatcher, so
retail creates its local entity through the normal map path. The host creates the
canonical entity and sends route 3 plus reliable `WorldSpawn`; route 3 intentionally
does **not** replay the dispatcher on the client. The received `WorldSpawn` links the
already-created local entity to the host `world_id` by signature. Thus the client has
one retail entity rather than a manually created replica plus a replay duplicate.

After a peer connects, `HandleTriggerSpawnFromDefinition` first reads the checked
trigger identity. A client allows its own NPC/monster native spawn and records it
without an id; the later host `WorldSpawn` binds it by the exact local `trigger +
family + subtype` live transform signature. The resolver re-reads that transform
rather than trusting the factory-time cached value, because map triggers can change it
before the ventilation chain fires. `definition_id` is deliberately not part of that
match: retail may assign it differently in each process, and separate
`XTrigger_MO_Mouse` templates have been observed with the same `definition=0` and
`occurrence=1`. A non-zero transform signature is mandatory for matching; there is no
fallback to the weaker key when it is absent or ambiguous. An armed direct native spawn
remains only as fallback when the host activated a trigger that the client never
locally ran. There is deliberately no raw pointer deletion: retail AI/task/intrusive-
list owners may still reference a locally created object.

`WorldSpawnPacket` now contains that `trigger_signature` and has a fixed 76-byte
wire layout. Both processes must run the same revision; an old 72-byte peer packet is
rejected with an explicit DLL-mismatch diagnostic rather than decoded against the new
layout.

The host publishes initial and changed HP through reliable `WorldDamage`. Client
replicas never send their locally simulated HP upstream; if client AI/collision
changes HP between samples, the last host-applied value is restored. Damage that
arrives before the replica link stays pending and is applied as soon as the matching
`world_id` appears, including a host kill that races a delayed client spawn. This is
a compiled lifecycle contract; live proof still requires a fresh-level spawn/kill
race test and confirmation that one NPC appears on each process without autonomous
client movement.

## Spawn regression investigation — 2026-09-11

User reports closed spawn boxes in **both** windows. The pre-fix runtime log
contains host WorldSpawn IDs 1..13 and client links; it does not prove that the
boxes in the screenshot were activated. Do not describe this as a global failure
of client spawning or restore the removed client-spawn ban.

User clarified two separate issues: spawn boxes already failed in a single-player
run before today's change (whether that run loaded the DLL is not established);
key-card insertion previously worked but now opens the peer door without marking
the card inserted. Do not merge these symptoms or assume a clean-vanilla failure.

The attempted synchronous replay suppression was withdrawn after this key-card
regression report. HandleTriggerEvent, route-4 dispatch and legacy trigger replay
retain their pre-experiment networking policy. The older relay/forwarder non-echo
scope remains unchanged. Repeated events are observations, not proof that every
nested event is redundant. Key-card recovery after withdrawal is **not-tested**.
Local spawns, WorldSpawn binding, P2 and Fly behavior were not changed here.

The log has repeated Counter events on both peers; whether that blocks these
boxes is still **guess/not-tested**, not a confirmed fix.
Read-only [world-counter] records exact Counter value before/after, threshold,
state flags and native/peer-replay origin. F9 adds [world-counter-catalog] for
untouched counters as well. The exact vtable gates these reads; no guessed
counter value or activation event is written. See RE_CATALOG for field evidence.

Test from a checkpoint before the activation (an already changed counter is not
repaired retroactively): F9 near the boxes, approach the trigger, F9 again if no
spawn. Keep the resulting gforce_coop.log next to the game executable. No F2 forced spawn is
needed, since it would bypass the scenario being diagnosed.

## Open gameplay work ledger — 2026-09-13

This is planning evidence, not an ABI, function or packet contract. It records
user-observed co-op failures and the next RE proof required before source changes.
An entry marked **observation** is reproducible evidence from play; **hypothesis**
is explicitly not an implementation instruction.

| Area | Evidence | Safe next RE boundary |
| --- | --- | --- |
| Death / checkpoint rollback | **Observation:** a death can place a player at the beginning of a level after world progress has closed doors, forcing a load. | Trace native death, checkpoint selection, save/load and level-transition routes separately. The existing outer-DeathMode presentation guard must not be presented as a checkpoint fix. |
| Cutscene deaths | **Observation:** some cutscene exits kill one or both peers. | The narrow progression rally can reunite players when the exact cutscene trigger succeeds, but it is not a death fix. Capture both process logs across the transition; do not reuse P2 attachment release or force Default as a generic cure. |
| Join after prior trigger/event activity | **Priority observation:** a late-joining peer needs durable consequences of prior progression: doors/shutters, counters, spawned/despawned objects and relevant progress state. | Recover a catalog of persistent root-object/cell state and an explicit snapshot/replay policy. Never blindly replay all historic events: transient hits, cutscenes and non-idempotent spawns can duplicate entities or repeat presentation. |
| Progression doors/shutters | **Observation:** some objects block only the client from reachable level regions. | Treat each root object identity and its complete relay/forwarder chain as distinct. A known card-door route is evidence for that object only; `0x41080010` is not a semantic open-door opcode. |
| Saberization and Saberizer HUD | **Solved for activation:** the saberized lamp flies on both screens via NPC mode sync (below). Saberizer is `type=0x40050007`/`item=0x50000003`; fire input replicates and P2 shoots visibly. The green scan overlay driver is still unknown (`0x5B8C20` never runs in steady state, P2 never enters Scan `0x6100000C`); the removed hook is negative evidence. | Recover gameplay state/owner separately from presentation/HUD selection. |
| Whip world hits | **Observation:** particles appear, while boxes/grilles do not react. | Recover one stock hit receiver or object event for each target family before networking it. Visual particle emission is non-authoritative and cannot prove a hit. |
| Mooch versus laser mines | **Hypothesis:** tripwire/mines react through an ordinary target hit or trigger receiver, not a direct mine-disable call. | Test local F1 and normal Mooch attack against one identified mine; log source Fly, target identity, hit/event route and native result before choosing replication. |
| Dynamic physics / key-card | **Observation:** card insertion can replicate without making the card itself a shared physical object. | Design only after discovering stable identity and native ownership/lifetime: pickup, carry, transform, drop, destruction and late link must have explicit authority. Never network raw addresses. |
| NPC death, animation, attacks and damage | **Observation:** a client can receive and locally kill a dynamic “toaster” before a distant host creates its canonical copy and assigns `world_id`; normally linked NPC fights can still carry client damage. | Investigate damage/death loss or reconciliation **before late link**, not a blanket absence of client damage. Fresh-level tests: host/client trigger, delayed linking, kill-before-link, attack animation, incoming and outgoing damage. Do not infer global NPC sync from a single successful kill. |
| World streaming / unloaded level areas | **Observation:** an alternative client route can unload parts of an apparently linear level in that process. | Recover native streaming-cell/zone transition and its relation to local loaded state versus shared progression. A rally may reunite peers at a confirmed cutscene/checkpoint, but must not be a generic streaming fix. |
| Chat | **Build state:** F10 opens a bounded retained non-activating GDI window owned by the game; transport uses a separate reliable UTF-8 packet. The old direct-DC panel flashed, while the first WS_CHILD replacement was completely hidden by D3D. | Two-process text delivery, Cyrillic layout, stable redraw in windowed/exclusive display, connection loss and message limits. Keep it independent from game action edges. |

Recommended evidence order is: late-join persistent-state policy, death/cutscene
recovery, concrete blocked progression objects and physics card, Saberization/HUD,
whip and Mooch mine interactions, then streaming transitions, NPC lifecycle
regressions and chat design. Each new live result must record level/checkpoint,
host/client role, runtime PID log excerpts, object identity and the exact visible
result. Update `RE_CATALOG.md` with exact static evidence before promoting a
candidate from `guess` to `approved`.

## Current ABR state — RDV root-transform test

The global `WorldObjectEvent` rollback was falsified: the ABR fixed-point crash
persisted while the rollback regressed remote door/card effects. Object-event
queue and ingress are restored. Any older claim that ABR suppresses all world
traffic is historical experiment text, not active runtime behavior.

P1 publishes its settled post-ABR root in the existing `CoopInput` snapshot.
The receiver applies that complete finite root before and after the stock ABR
controller tick, separately from ordinary P2 interpolation. **Live test
2026-09-16:** `0x10000014` remote press edges reached both receivers, and the
RDV task was created/configured, but no remote ABR shot was visible. The exact
`GForce.exe` path explains the missing pressed-query log: the RDV helper at
`0x005BD5D0` calls predicate `0x005BC480` at `0x005BD5FD`; a false result exits
at `0x005BD604`, before the native `0x10000014` query at `0x005BDBD8`.

The new receiver first checks that *both* local P1 and the remote owner are in
ABR and that the verified P2 RDV task is configured. Only then it changes P2's
registered ABR conflict mask `3 -> 2` (dropping the exclusive active-owner bit)
and uses the stock state dispatcher to enter P2 ABR from Default. Death, Ledge,
cutscene and other P2 modes are not overridden. The P2 ABR tick receives only
the remote `0x10000014` press/held/release input. The local P1 camera fields and
active-entity pair are restored after that tick. If both owners later report
Default, P2 leaves ABR through the same stock dispatcher. This is a guarded
native-mode test, **not yet a confirmed remote beam fix**. The next two-process
test now also has a narrow predicate override: only a fresh received ABR Fire
edge, only while the remote-input scope is active, and only when the predicate's
owning controller is exactly RemoteP2 in outer ABR mode may turn its false into
true. It does not call a projectile, modify inventory, or bypass the predicate
for P1. The expected log sequence is
`[abr-fire] allowing RemoteP2 ABR attack block ...`, followed by
`[abr-fire] native ABR pressed query accepted`; a shot on screen is the actual
success criterion. This override is **not-tested**. The former fixed-point ABR
crash was not reproduced after
root-transform correction; the old trace had P2 health `50 -> 0` just before
local trigger events `0x410800E2/E3`, without a recovered semantic for E2/E3.

## Game language override

The retail EXE picks `File_XXX.bin` by its own language mechanic (`0x458A40`,
single call site `0x459716`) but always asks for `\data\File_RUS.000`
(`0x6F858C` via `0x458402`); no `File_USA.000` string exists. On a non-RUS
language the index and the blob disagree (10 shared hashes of ~1760), so voices
go silent. A CreateFileA/W mirror was tried first and then removed: the live
log proved the engine resolves the blob itself once the language is selected
natively (no `File_RUS.000` request appears in a USA boot), so intercepting
every file open was pure overhead.

`GForce.ini [language] language` with a 3-letter code drives the game's own
language selector (`0x4596A0`) through an E9 hook, so text (via `0x4902D0`,
which stores `kTextLanguage` and rebuilds strings), audio requests and the
`File_XXX.bin` choice switch together natively. `auto`/`off` never override.
Verified live 2026-09-16: `game language overridden: 0x0F -> 0x00` with
audible English voices.

## Build and deploy

With the game closed, run from `tools/coop_v2`:

```powershell
$env:GFORCE_DEPLOY_ROOT = 'E:\G-Force'
cmd.exe /d /c build.bat
```

`build.bat` uses an MSBuild `Rebuild`, so every production `.cpp` in both
projects is compiled before deployment. The expected result is
`BUILD_OK: GForceCoop.sln Release|Win32` followed by `DEPLOY_OK`. This verifies
compilation and copies the two mod DLLs; it does not launch the game or validate
gameplay.
