#pragma once

#include <stddef.h>
#include <stdint.h>

namespace coop
{
	namespace gforce
	{
		constexpr uintptr_t kImageBase = 0x00400000u;
		constexpr uint32_t kTimeDateStamp = 0x4A2D1332u;
		constexpr uint32_t kSizeOfImage = 0x006E4000u;
		constexpr uint32_t kCheckSum = 0x00548D48u;
		constexpr uint32_t kEntryPoint = 0x002BAAA0u;
		constexpr int64_t kFileSize = 5525504;

		constexpr uintptr_t kSpawnCall1 = 0x0043F403u;
		constexpr uintptr_t kSpawnCall2 = 0x0043F49Du;
		constexpr uintptr_t kSpawnGPig = 0x00545370u;
		constexpr uintptr_t kSelectMode = 0x004B7050u;
		// StateMachine_SelectState (approx-name) receives (controller, mode_id,
		// force_reselect). Its five-byte prologue is three pushes plus mov esi,ecx,
		// so the existing E9 trampoline can relocate it without a relative fixup.
		constexpr uint8_t kExpectedStateMachineSelectState[] = {
			0x51, 0x53, 0x56, 0x8B, 0xF1
		};
		constexpr uintptr_t kLanguageSelect = 0x004596A0u;
		// Language manager setter (approx-name) receives the manager in ECX and
		// (language, unknown) on the stack, then notifies text (0x4902D0,
		// which stores kTextLanguage and rebuilds strings) and re-selects the
		// audio File_XXX.bin (0x458A40). Its 7-byte prologue has no relative
		// operands, so the E9 trampoline relocates it verbatim.
		constexpr uint8_t kExpectedLanguageSelect[] = {
			0x51, 0x8A, 0x54, 0x24, 0x08, 0x80, 0xFA, 0xFF
		};
		constexpr uintptr_t kDefaultModeActiveStores = 0x005BEAD6u;

		constexpr uintptr_t kGPigUpdateVtableSlot = 0x0070C8A4u;
		// XController_Fly uses the same base Update body as GPig, but a different
		// vtable slot.  Both must reach HookControllerUpdate so the fly's native mode
		// edge and final controller transform can be observed.
		constexpr uintptr_t kFlyUpdateVtableSlot = 0x007180F4u;
		constexpr uintptr_t kOriginalControllerUpdate = 0x005BFBE0u;
		// Exact inner GPig attachment-machine vtable bases. The generic native state
		// dispatcher identifies each family by registered state classes, including
		// branches whose Update is a nullsub. These are class identifiers only; the
		// mod never writes them or selects an inner state directly.
		constexpr uintptr_t kGPigLedgeIdleVtable = 0x0070351Cu;
		constexpr uintptr_t kGPigLedgeIntoVtable = 0x00703574u;
		constexpr uintptr_t kGPigLedgeStrafeEndVtable = 0x007036D4u;
		constexpr uintptr_t kGPigLedgeJumpVtable = 0x007038E4u;
		// XGPigClimbMode is separate from XGPigLedgeMode. Every stock Climb
		// registry contains these two exact child classes, so they are sufficient
		// to classify the whole nested machine without guessing the other children.
		constexpr uintptr_t kGPigClimbDropVtable = 0x006FFCBCu;
		constexpr uintptr_t kGPigClimbJumpVtable = 0x006FFD14u;
		// Supplemental active inner Ledge updates.  The generic observer above is
		// the authoritative attachment lifetime signal; these hooks remain a
		// byte-checked secondary observation for the known concrete update paths.
		constexpr uintptr_t kGPigLedgeIdleUpdateVtableSlot = 0x00703528u;
		constexpr uintptr_t kGPigLedgeIntoUpdateVtableSlot = 0x00703580u;
		constexpr uintptr_t kGPigLedgeStrafeEndUpdateVtableSlot = 0x007036E0u;
		constexpr uintptr_t kGPigLedgeJumpUpdateVtableSlot = 0x007038F0u;
		constexpr uintptr_t kGPigLedgeIdleUpdate = 0x004F09E0u;
		constexpr uintptr_t kGPigLedgeStrafeEndUpdate = 0x004F0BC0u;
		constexpr uintptr_t kGPigLedgeJumpUpdate = 0x004F0CA0u;
		// Every observed concrete Ledge/Climb state passes its owning GPig at this
		// exact field to native code. This is a narrow observer boundary, not a
		// complete Ledge or Climb class layout.
		constexpr size_t kGPigAttachmentStateOwnerEntityOffset = 0x04u;

		constexpr uintptr_t kInputActionQuery = 0x00488A70u;      // is-down (level)
		constexpr uintptr_t kInputActionUpQuery = 0x00488B70u;    // is-up (inverse level)
		constexpr uintptr_t kInputThresholdQuery = 0x00488DC0u;   // hold + threshold
		constexpr uintptr_t kInputAxisQuery = 0x0048B010u;        // analog axis
		constexpr uintptr_t kInputPressedQuery = 0x00488CE0u;     // pressed this frame (rising edge)
		constexpr uintptr_t kInputReleasedQuery = 0x00488C00u;    // released this frame (falling edge)
		constexpr uintptr_t kInputHoldDurationQuery = 0x00488E50u;// hold duration >= threshold
		// XControllerMode_GPig_RDV's update calls this predicate before the block
		// containing its native input queries. A presentation-only P2 can fail the
		// predicate even with a valid RDV task, so co-op may override false only for
		// RemoteP2, only in ABR, and only on a received ABR fire edge.
		constexpr uintptr_t kAbrAttackPredicate = 0x005BC480u;
		constexpr uint8_t kExpectedAbrAttackPredicate[] = {
			0x8B, 0x49, 0x04,                         // mov ecx,[ecx+4]
			0x8B, 0x15, 0x34, 0x56, 0x91, 0x00        // mov edx,[0x00915634]
		};
		// 0x488B00 is the aim-hold query of 0x5BB1D0 (0x5BB321 with float [0x6F26D8],
		// 0x5BB34D with float [0x6F2754]).  Same shape as 0x488DC0 but a different
		// function AND a different argument order: the flags word is pushed before the
		// float, so this is (device, action, flags, threshold).  While it stayed
		// unhooked, P2's aim branch was decided by the local physical mouse instead of
		// the remote snapshot.
		constexpr uintptr_t kInputAimHoldQuery = 0x00488B00u;     // aim hold + threshold
		// Fly_Active polls this separate raw-action family (0x4008xxxx) for several
		// private Fly branches. Its four stack arguments are device, action, flags,
		// and the engine's input-record flag.
		constexpr uintptr_t kInputRawPressedQuery = 0x0048AE10u;
		constexpr uintptr_t kInputRawReleasedQuery = 0x0048AF10u;
		constexpr uintptr_t kInputRawHeldQuery = 0x0048AF90u;
		// User-reported dual Mooch laser: Fly_Active calls raw pressed query
		// 0x48AE10 at 0x005B60F2 for this action. Its exact return site then
		// selects two native objects and changes their stock effect flag. The same
		// raw action is also used by unrelated P1 weapon code, so replay must be
		// restricted to this return address and never be a global input override.
		constexpr uint32_t kFlyDualLaserRawActionId = 0x40080029u;
		constexpr uintptr_t kFlyDualLaserRawPressedQueryReturn = 0x005B60F7u;
		// The exact Fly_Active block then resolves two items from the Mooch
		// Handler's inventory and writes their stock effect field. These are a
		// narrow receiver-presentation contract, not a general inventory API.
		constexpr uintptr_t kFindFlyDualLaserRouteItem = 0x0040CB70u;
		constexpr uintptr_t kResolveFlyDualLaserRouteItem = 0x00593710u;
		// The preceding Fly_Active route first obtains a controller-local
		// XMotorTask_Aim and supplies its target point.  Merely toggling the two
		// inventory item flags leaves that task with P1's stale target/context,
		// which makes the visual effect originate in the wrong place.
		constexpr uintptr_t kGetFlyDualLaserPresentationContext = 0x005B00E0u;
		constexpr uintptr_t kGetFlyDualLaserAimTask = 0x00442300u;
		constexpr uintptr_t kSetFlyDualLaserAimTarget = 0x004B9F30u;
		// XFlyFlyMode_Move normally smooths these four state fields and calls this
		// helper. The helper itself only converts the current angles to a world
		// LookAt target and marks that target active; it neither changes Fly mode nor
		// accesses the shared camera. Co-op uses that narrow terminal step for a
		// presentation-only remote Mooch body direction.
		constexpr uintptr_t kSubmitFlyFlyBodyDirection = 0x005101F0u;
		constexpr uint8_t kExpectedSubmitFlyFlyBodyDirection[] = {
			0x83, 0xEC, 0x5C, 0x53, 0x8B
		};
		// The native laser action is not a standalone fire routine.  It is a raw
		// input branch inside this registered Fly_Active mode.  The co-op layer may
		// call only the mode's update body, while its input hook supplies the one
		// pressed edge; it never selects/enters this mode on a presentation peer.
		constexpr uint32_t kFlyActiveModeId = 0x61000007u;
		constexpr uintptr_t kFlyActiveVTable = 0x007180A4u;
		constexpr uintptr_t kFlyActiveUpdate = 0x005B4C30u;
		constexpr uint8_t kExpectedFlyActiveUpdate[] = {
			0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, 0x83, 0x3D,
			0x50, 0x24, 0x91, 0x00, 0x00
		};
		constexpr uint8_t kExpectedFindFlyDualLaserRouteItem[] = {
			0x53, 0x56, 0x57, 0x8B, 0xF9
		};
		constexpr uint8_t kExpectedResolveFlyDualLaserRouteItem[] = {
			0x53, 0x56, 0x8B, 0x71, 0x14
		};
		constexpr uint8_t kExpectedGetFlyDualLaserPresentationContext[] = {
			0x8B, 0x41, 0x18, 0x85, 0xC0
		};
		constexpr uint8_t kExpectedGetFlyDualLaserAimTask[] = {
			0x6A, 0xFF, 0x68, 0x8B, 0x44
		};
		constexpr uint8_t kExpectedSetFlyDualLaserAimTarget[] = {
			0x8B, 0x44, 0x24, 0x04, 0xD9
		};
		constexpr uint32_t kFlyDualLaserFirstRouteSlot = 0x0E00002Fu;
		constexpr uint32_t kFlyDualLaserSecondRouteSlot = 0x0E00007Cu;
		constexpr uint32_t kFlyDualLaserDefaultItemBase = 0x50000002u;
		constexpr uint32_t kFlyDualLaserAlternateItemBase = 0x50000017u;
		// Return address after Fly_Active's own Mooch-action edge query. This is
		// the native exit path; Darwin's separate entry query stays suppressed.
		constexpr uintptr_t kFlyExitActionQueryReturn = 0x005B63B1u;
		// 0x5BEA00 receives the active XGamePad/input manager as argument #1.  Its
		// inner fire routine (0x5B8760) consumes that same pointer from [esp+4Ch].
		constexpr uintptr_t kDefaultModeUpdate = 0x005BEA00u;
		// Copies XGamePad +0x2774/+0x2780 into the native projectile command.
		constexpr uintptr_t kFireHandler = 0x005B8760u;
		// WeaponAmmoItem's per-shot timer.  Once it consumes the weapon record's
		// +0x67C round counter, it mirrors that value into the shared HUD ammo pool.
		// Hooking here is later than the controller update and is therefore the only
		// point that can preserve P1's pool without racing the stock write.
		constexpr uintptr_t kWeaponAmmoConsume = 0x0059F650u;
		// Handler+0x5A0 is the native two-slot health component; its first value is
		// Handler+0x5A4.  These stock mutators are diagnostic-only hook points.
		// 0x53EC50 replaces a slot value; 0x53ECF0 applies a signed delta.
		constexpr uintptr_t kHealthComponentSet = 0x0053EC50u;
		constexpr uintptr_t kHealthComponentAdd = 0x0053ECF0u;
		// 0x53EDC0 is the clamped subtraction path: it reads component slot N,
		// subtracts its first float argument, and writes the resulting slot value.
		constexpr uintptr_t kHealthComponentSubtract = 0x0053EDC0u;
		constexpr uint8_t kExpectedHealthComponentSet[] = {
			0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04
		};
		constexpr uint8_t kExpectedHealthComponentAdd[] = {
			0xF3, 0x0F, 0x10, 0x54, 0x24, 0x04
		};
		constexpr uint8_t kExpectedHealthComponentSubtract[] = {
			0xF3, 0x0F, 0x10, 0x54, 0x24, 0x04
		};
		constexpr uintptr_t kXGamePadCtor = 0x0048B290u;

		// Retail main menu, verified in IDA 9.1 against XHudMenuMain::BuildMainMenu.
		// The narrow call-site below is immediately after the stock Credits row.  It
		// is intentionally a profile-specific hook, not a generic EngineX UI API.
		constexpr uintptr_t kMainMenuBuild = 0x005EECC0u;
		constexpr uintptr_t kMenuCreditsAddChildCall = 0x005EED92u;
		constexpr uintptr_t kMenuAddChild = 0x005C4850u;
		constexpr uintptr_t kMenuCreateButton = 0x005E8420u;
		constexpr uintptr_t kMenuLabelResolver = 0x00490900u;
		constexpr uintptr_t kGameAllocator = 0x00605919u;
		constexpr uintptr_t kGameStringAssign = 0x006395BAu;
		constexpr uintptr_t kGameStringAssignAnsi = 0x0063982Eu;
		constexpr uintptr_t kGameStringRelease = 0x0064E925u;
		constexpr uintptr_t kMenuCallbackInvoke = 0x005E8AA0u;
		constexpr uint32_t kRetailMenuUintCallbackVtable = 0x0071B098u;
		constexpr uintptr_t kMenuCallbackInvokeVtableSlot = 0x0071B09Cu;
		constexpr uintptr_t kXATextResolverVtableSlot = 0x006FA818u;
		constexpr uintptr_t kTextLanguage = 0x009144F8u;
		// This is a project-private resource key. It is intercepted before retail
		// resource lookup, so it cannot alter the stock Level Select title at
		// 0x43003B3E or any other retail label.
		constexpr uint32_t kMenuConnectLabelResourceId = 0xC0DEC001u;
		constexpr uint32_t kMenuConnectAction = 0xC0DEC002u;
		constexpr uint8_t kExpectedMainMenuBuild[] = {
			0x55, 0x56, 0x57, 0x8B, 0xF1
		};
		constexpr uint8_t kExpectedMenuCreditsAddChildCall[] = {
			0xE8, 0xB9, 0x5A, 0xFD, 0xFF
		};
		constexpr uint8_t kExpectedMenuAddChild[] = {
			0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x2C
		};
		constexpr uint8_t kExpectedMenuCallbackInvoke[] = {
			0x8B, 0xC1, 0x8B, 0x48, 0x04, 0x8B, 0x50, 0x0C,
			0x51, 0x8B, 0x48, 0x08, 0xFF, 0xD2, 0xC3
		};
		constexpr uint8_t kExpectedMenuLabelResolver[] = {
			0x6A, 0xFF, 0x68, 0xB8, 0x5F, 0x6D, 0x00,
			0x64, 0xA1, 0x00, 0x00, 0x00, 0x00
		};
		constexpr uint8_t kExpectedGameStringAssign[] = {
			0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C, 0x8B, 0xF1
		};
		constexpr uint8_t kExpectedGameStringAssignAnsi[] = {
			0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1,
			0x83, 0x26, 0x00
		};
		constexpr uint8_t kExpectedGameStringRelease[] = {
			0x56, 0x8B, 0xF1, 0x8B, 0x06, 0x85, 0xC0, 0x74
		};

		// XLoadSaveManagerPlatform is a process-global object.  The stock Load Game
		// menu passes the selected zero-based row to 0x5F1920, which persists it here
		// before the game flow switches into loading.  It is therefore the selected
		// host save slot, not a guessed DATA file number.
		constexpr uintptr_t kLoadSaveManager = 0x00915B40u;
		constexpr uintptr_t kBeginNativeSaveLoad = 0x005F1920u;
		// Exact validated Load Game menu call. It passes the selected row to the
		// native loader after the stock availability checks.
		constexpr uintptr_t kHostLoadGameCall = 0x005EDC5Du;
		constexpr size_t kLoadSaveSelectedSlotOffset = 0x4BECu;
		constexpr uint32_t kVisibleSaveSlotCount = 5u;
		constexpr uint8_t kExpectedBeginNativeSaveLoad[] = {
			0x8B, 0x44, 0x24, 0x04, 0x56, 0x8B, 0xF1, 0x6A, 0x09,
			0x89, 0x86, 0xEC, 0x4B, 0x00, 0x00
		};
		constexpr uint8_t kExpectedHostLoadGameCall[] = {
			0xE8, 0xBE, 0x3C, 0x00, 0x00
		};
		constexpr uintptr_t kGamePointer = 0x00912784u;
		// The process-wide XGamePad selected by XGamePad::Register at 0x487F10.
		// P1 owns this pointer permanently. P2's pad is constructed but never
		// registered, then passed directly to Default-mode calls instead.
		constexpr uintptr_t kPrimaryGamePad = 0x009905CCu;
		constexpr uintptr_t kActiveEntityA = 0x00912AA4u;
		constexpr uintptr_t kActiveEntityB = 0x00912788u;
		constexpr uintptr_t kGPigEntityArray = 0x009128D8u;
		// Global live-entity registry.  The two intrusive lists below are walked by
		// the game's own targeting code: +0x28 holds generic NPCs and +0x30 holds
		// monster/appliance entities.  Each list node is { prev, next, entity }.
		constexpr uintptr_t kEntityRegistry = 0x00912B50u;
		// Guard only against a corrupted/cyclic intrusive list.  This is not a
		// claimed maximum number of live entities.
		constexpr size_t kEntityRegistryWalkSafetyLimit = 512u;
		// Slot 4 of the same array (0x9128D8 + 4*4): Mooch, the fly.  It is NOT one of
		// the guinea-pig slots 1..3, so P2 in slot 2 does not collide with it.  The fly
		// switch at 0x5BBC80 refuses to run when this is null (0x5BBD00), and reads the
		// fly's handler through [fly + 0x144] at 0x5BBD6B.
		constexpr uintptr_t kFlyEntity = 0x009128E8u;
		// GPig_Mooch::Enter enables this exact state before native Fly_Active::Update
		// can read its raw action branches. The presentation peer keeps the retail
		// flag cleared, preventing receiver-side Fly mode/camera ownership.
		constexpr uintptr_t kFlyActiveStateIndex = 0x009155FCu;
		constexpr size_t kHandlerFlyStateTableOffset = 0x4ECu;
		// Fly_Idle::Update selects Fly_Active when this native state byte is set.
		// It must not be replicated as receiver ownership: receiver presentation
		// writes the controller-local Aim task without selecting this mode.
		constexpr size_t kFlyControlActiveOffset = 0x53u;
		// 96-byte entry at Handler.MotorSystem task-state index
		// [0x009155FC]. `XFlyFlyMode_Move::Update` smooths target yaw/pitch into
		// current yaw/pitch then passes the state to kSubmitFlyFlyBodyDirection.
		// Only these verified fields are exposed; this is not a complete state type.
		constexpr size_t kFlyFlyTargetYawOffset = 0x2Cu;
		constexpr size_t kFlyFlyCurrentYawOffset = 0x30u;
		constexpr size_t kFlyFlyTargetPitchOffset = 0x34u;
		constexpr size_t kFlyFlyCurrentPitchOffset = 0x38u;
		constexpr size_t kFlyFlyDirectionActiveOffset = 0x50u;
		constexpr uintptr_t kCameraManager = 0x00915738u;

		constexpr uintptr_t kKeyboardStateOwner = 0x00AA6580u;
		constexpr uintptr_t kGetCameraHandler = 0x00515C80u;
		constexpr uintptr_t kRefreshGPigCamera = 0x005B03A0u;
		// 0x5BCF30 is the per-frame camera update of XControllerMode_GPig_Default,
		// called at 0x5BEBD3 before 0x5B92A0 (0x5BEBF7) and 0x5BB1D0 (0x5BEC24).
		// __thiscall(mode), no stack arguments, tail-jumps into 0x5B8210.  It decides
		// which state the ONE shared camera should be in — 0x5BD2FC writes the aim id
		// 0x4411000C into [handler+0x9A0], 0x5BD38B/0x5BD3B6/0x5BD3F4 write the follow
		// id 0x44110010 — and then applies it through 0x5B03A0/0x5B0620/0x5B04F0 at
		// 0x5BD4E8.  The two queries that pick aim vs follow (0x5BD33C, 0x5BD36C) read
		// the process-global XGamePad 0x9905CC, NOT the pad argument, so during P2's
		// tick this function drove the single camera from the remote snapshot and left
		// it in follow state for P1's next frame.  P2 owns no camera on this machine,
		// so the whole call is skipped while remote input is active.
		constexpr uintptr_t kGPigCameraUpdate = 0x005BCF30u;
		// 0x52AD20 is the camera yaw getter, __thiscall(handler), result in st(0):
		// [handler+0x4BC] ? -vcall[+0x30]() : -[handler+0x190C].  Every body-turn path
		// reads it — 0x5BBB67 (aim turn) and 0x5B8DB7 (movement yaw = atan2(axis) +
		// this) — and the handler is shared, so unhooked P2 turned towards P1's camera.
		// `CameraHandler::GetCurrentYaw` takes the handler in ECX and returns a float
		// in x87 ST(0); see `re_cache/rdv_camera_contract_dump.txt`.
		constexpr uintptr_t kCameraYawGetter = 0x0052AD20u;
		constexpr size_t kCameraRequestedStateOffset = 0x9A0u;
		constexpr uintptr_t kGetCurrentWeaponId = 0x00544A30u;
		constexpr uintptr_t kSetSelectedWeaponType = 0x005434E0u;
		constexpr uintptr_t kWeaponTypeToItemId = 0x00543520u;
		constexpr uintptr_t kResolveItemById = 0x00472B00u;
		// Ammo HUD at 0x5D616F resolves the active weapon through this shared pool and
		// formats [entry+0x0C] / [entry+0x10] as current/max ammunition.
		constexpr uintptr_t kAmmoPool = 0x00915458u;
		constexpr uintptr_t kResolveWeaponRecord = 0x005933E0u;
		constexpr uintptr_t kResolveAmmoEntry = 0x0049D050u;
		constexpr size_t kWeaponRecordAmmoIdOffset = 0x6C8u;
		constexpr size_t kWeaponRecordRoundCountOffset = 0x67Cu;
		constexpr size_t kAmmoEntryCurrentOffset = 0x0Cu;

		constexpr size_t kEntityHandlerOffset = 0x144u;
		// Retail's root-transform resolver uses this as a validity byte for the
		// cached 4x4 matrix at +0x88. Native writers clear it after changing root
		// position/rotation; the next native reader rebuilds the matrix from them.
		constexpr size_t kEntityTransformCacheValidOffset = 0x7Eu;
		constexpr size_t kEntityRotationOffset = 0xC8u;
		constexpr size_t kEntityPositionOffset = 0xE8u;
		// Entity reference slot used by sub_472B00 resolver; found in ammo trace at
		// 0x5B8A1A where a hit-result entity's [edi+0x658] is passed to sub_472B00
		// to obtain the target entity for distance checks.
		constexpr size_t kEntityResolverSlotOffset = 0x658u;
		// Live NPC/monster entity -> source trigger.  The trigger retains the exact
		// map archetype and spawn data, so cloning it is safer than guessing a subtype
		// from the very large factory switch at 0x423830.
		constexpr size_t kEntityTriggerOffset = 0x14Cu;
		constexpr size_t kEntityRegistryNpcListOffset = 0x28u;
		constexpr size_t kEntityRegistryMonsterListOffset = 0x30u;
		constexpr size_t kIntrusiveListNextOffset = 0x04u;
		constexpr size_t kIntrusiveListValueOffset = 0x08u;
		constexpr size_t kTriggerFamilyOffset = 0x100u;
		constexpr size_t kTriggerSubtypeOffset = 0x104u;
		constexpr size_t kTriggerFlagsOffset = 0x108u;
		// Exact XTrigger_TR_Counter, verified in retail 0x00440D30. Read-only
		// diagnostics: event low16 0x53 decrements, 0x64 resets, others increment;
		// the native threshold check is equality, not >=.
		constexpr uintptr_t kTriggerCounterVtable = 0x006F496Cu;
		constexpr size_t kTriggerCounterValueOffset = 0x137u;
		constexpr size_t kTriggerCounterThresholdOffset = 0x0Cu;
		constexpr size_t kTriggerStateFlagsOffset = 0x110u;
		constexpr size_t kTriggerSpawnIdOffset = 0x130u;
		// The native spawn routine stores its freshly constructed game object here.
		// It is useful for tracing a trigger-to-live-instance relationship, but it is
		// deliberately not a cross-process identifier.
		constexpr size_t kTriggerSpawnedObjectOffset = 0x98u;
		constexpr size_t kTriggerPositionOffset = 0xA4u;
		constexpr size_t kTriggerRotationOffset = 0xB4u;
		constexpr size_t kTriggerSpawnVtableOffset = 0x30u;
		constexpr size_t kTriggerCloneVtableOffset = 0x3Cu;
		// Trigger reference slot used by sub_472B00 resolver; found at line 6728 of
		// ida_trace_npc.txt where trigger[90] = *(trigger + 0x168) is resolved.
		constexpr size_t kTriggerResolverSlotOffset = 0x168u;
		constexpr uint32_t kMonsterTriggerFamily = 0x1E000002u;
		constexpr uint32_t kNpcTriggerFamily = 0x1E00000Cu;
		// Verified computing-centre chain: the source box/activator first receives
		// 0x41080022, then the game creates the downstream computer spawner, which
		// later produces dynamic chip/spider triggers (definition -1).
		constexpr uint32_t kComputerBoxTriggerSubtype = 0x1F000095u;
		constexpr int32_t kComputerBoxTriggerDefinition = 43;
		constexpr int32_t kComputerBoxActivateEvent = 0x41080022;
		constexpr uint32_t kComputerSpawnerTriggerSubtype = 0x1F0000D6u;
		constexpr int32_t kComputerSpawnerTriggerDefinition = 104;
		constexpr uint32_t kTriggerHasSpawnDefinition = 0x04000000u;
		// The two known clone implementations are the generic one and the monster
		// override.  Both preserve the source trigger's map-only spawn definition.
		constexpr uintptr_t kTriggerCloneGeneric = 0x0042E6C0u;
		constexpr uintptr_t kTriggerCloneMonster = 0x0042E9A0u;
		// Common factory for the trigger definitions themselves.  Unlike the later
		// 0x41F220 entry, this sees both map construction and dynamic clone requests.
		constexpr uintptr_t kTriggerFactory = 0x00423830u;
		constexpr uintptr_t kTriggerSpawnFromDefinition = 0x0041F220u;
		// Shared vtable slot +0x1C for the generic NPC/enemy trigger classes.  It is
		// called when a trigger receives a gameplay event, before its specialised
		// handlers create any dynamic children.
		constexpr uintptr_t kTriggerEventDispatcher = 0x0042FAB0u;
		// `sub_41E8B0` forwards contextual 0x41xxxxxx actions through this global
		// seven-listener fan-out. It is separate from TriggerEventDispatcher, so it
		// is diagnostic evidence for buttons/objects that never become a trigger.
		constexpr uintptr_t kGlobalEventForwarder = 0x0046D760u;
		// These temporary diagnostics sit on the neighbouring, separate object-event
		// chain. Static analysis proves the cdecl relay and stdcall forwarder carry
		// contextual object events. The read-only diagnostic resolves an MSVC RTTI
		// class name only when that metadata validates; it never replays an event.
		constexpr uintptr_t kObjectEventRelay = 0x0041E890u;
		constexpr uintptr_t kObjectEventForwarder = 0x0046D6F0u;
		// Exact retail map-trigger vtables observed in the object-event chain.
		// They identify durable progression boundaries, not universal object events.
		constexpr uintptr_t kTriggerObCutsceneVtable = 0x006F3FD4u;
		constexpr uintptr_t kTriggerPlCheckpointVtable = 0x006F3EFCu;
		// `sub_41E890` uses `mov ecx, 0x00912AA8` before calling the object
		// forwarder. This is the literal retail receiver address (not a pointer to
		// dereference); the exact EXE fingerprint makes the fixed address valid.
		constexpr uintptr_t kObjectEventReceiverAddress = 0x00912AA8u;
		constexpr uint8_t kExpectedTriggerFactory[] = {
			0x6A, 0xFF, 0x68, 0x02, 0x39, 0x6D, 0x00,
			0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50
		};
		// `sub_41F220` begins with an SEH frame setup.  These fourteen bytes comprise
		// four complete non-relative instructions and are safe to move to a detour
		// trampoline.  Keep this fingerprint strict: another executable revision must
		// not be traced through a guessed entry point.
		constexpr uint8_t kExpectedTriggerSpawnFromDefinition[] = {
			0x6A, 0xFF, 0x68, 0x16, 0x35, 0x6D, 0x00,
			0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50
		};
		// `sub_42FAB0`: push ebp; push esi; mov esi, ecx; cmp byte ptr [esi+144], 0.
		// The following push edi begins a new instruction, so these eleven bytes are
		// safe to relocate into the generic detour trampoline.
		constexpr uint8_t kExpectedTriggerEventDispatcher[] = {
			0x55, 0x56, 0x8B, 0xF1, 0x80, 0xBE, 0x44, 0x01, 0x00, 0x00, 0x00
		};
		// Runtime-verified `sub_46D760`: push ecx; push ebp;
		// mov ebp,[esp+10h]; push esi; push edi.
		// These eight bytes are complete, non-relative instructions and preserve the
		// incoming global receiver in ECX for the trampoline.
		constexpr uint8_t kExpectedGlobalEventForwarder[] = {
			0x51, 0x55, 0x8B, 0x6C, 0x24, 0x10, 0x56, 0x57
		};
		// Runtime-read, complete non-relative instructions. The relay needs eight
		// bytes because its first two mov instructions are four bytes each; the
		// forwarder has a clean five-byte prologue.
		constexpr uint8_t kExpectedObjectEventRelay[] = {
			0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04
		};
		constexpr uint8_t kExpectedObjectEventForwarder[] = {
			0x56, 0x8B, 0x74, 0x24, 0x08
		};
		// 0x5933E0 walks an inventory list, not the GPig handler itself.  The ammo HUD
		// obtains this exact container through `[handler + 0x514]` at 0x5D60EF before
		// resolving the selected weapon's WeaponAmmoItem record.
		// The GPig motor system is inline in its handler.  The task-state table at
		// handler+0x4EC is the same pointer reached through motor_system+0x2C.
		// These names describe only the factory contract; they do not claim a full
		// XMotorSystem class layout.
		constexpr size_t kHandlerMotorSystemOffset = 0x4C0u;
		constexpr size_t kHandlerMotorTaskStateTableOffset = 0x4ECu;
		constexpr size_t kMotorSystemResourceCountOffset = 0x10u;
		constexpr size_t kMotorSystemResourceTableOffset = 0x14u;
		constexpr size_t kMotorSystemTaskStateTableOffset = 0x2Cu;
		// Defensive bound retained from the old ABR task contract, not a claim that
		// this is the retail table's allocated length.
		constexpr uint32_t kMotorSystemTaskStateSafetyLimit = 64u;
		constexpr size_t kHandlerInventoryOffset = 0x514u;
		constexpr size_t kHandlerControllerOffset = 0x510u;
		// In Fly_Active's confirmed dual-laser block, this handler byte selects
		// 0x50000002/3 versus 0x50000017/18 for the two route items.
		constexpr size_t kHandlerFlyDualLaserItemSetOffset = 0x698u;
		constexpr size_t kFlyDualLaserRouteItemIdOffset = 0x604u;
		constexpr size_t kFlyDualLaserRouteItemActiveOffset = 0x658u;

		// Confirmed on monster subtype 0x1F0000F0: this float changed from 20.0
		// to 10.0 for one ordinary hit, while no controller field showed a matching
		// discrete health decrement.
		constexpr size_t kHandlerHealthOffset = 0x5A4u;
		constexpr size_t kHandlerSelectedWeaponTypeOffset = 0x26E0u;
		constexpr size_t kControllerOwnerOffset = 0x18u;
		constexpr size_t kControllerModeOffset = 0x1Cu;
		constexpr size_t kControllerAccumulatedHitsOffset = 0xA8u;
		constexpr size_t kControllerDeathFlagOffset = 0xACu;
		constexpr size_t kControllerInvulnTimerOffset = 0xC0u;
		constexpr size_t kControllerModeCountOffset = 0x10u;
		constexpr size_t kControllerModeTableOffset = 0x14u;
		constexpr uint32_t kControllerModeSafetyLimit = 64u;
		// Generic StateMachine_SelectState uses the same compact registry layout as
		// a controller, but it is also used by nested motors such as GPig Ledge.
		// Keep separate names so feature code cannot mistake a nested machine for a
		// top-level player controller.
		constexpr size_t kStateMachineModeCountOffset = 0x10u;
		constexpr size_t kStateMachineModeTableOffset = 0x14u;
		constexpr size_t kStateMachineCurrentModeOffset = 0x1Cu;
		constexpr uint32_t kStateMachineModeSafetyLimit = 64u;
// Every registered controller mode stores its owning controller at +0x04.
constexpr size_t kModeControllerOffset = 0x04u;
constexpr size_t kModeIdOffset = 0x08u;
		constexpr size_t kModeConflictMaskOffset = 0x0Cu;
		constexpr size_t kModeUpdateVtableOffset = 0x0Cu;
		// Fly_Active::Enter sets this mode-local flag before the update body. A
		// shadow ability pass temporarily mirrors only this byte and restores it
		// immediately; it does not run Enter or alter the controller's current mode.
		constexpr size_t kFlyActiveModeEnteredOffset = 0x10u;
		constexpr size_t kGameInputDeviceOffset = 0x674u;
		constexpr size_t kCameraTargetControllerOffset = 0x900u;
		constexpr size_t kCameraTargetIdOffset = 0xB8u;

		// There is exactly ONE camera handler in the process.  0x515C80 returns
		// [[0x915738+0x18]+0x144], and 0x915750 (= 0x915738+0x18) is the level
		// singleton, written only at 0x467CF7/0x468677 — it is not a per-player slot,
		// and 0x5B03A0 can only re-point that single handler's follow target.
		// 0x5BB1D0, the aim/weapon state machine reached from
		// XControllerMode_GPig_Default at 0x5BEC24, writes these shared floats:
		//   +0x8B4, +0x8B8    aim-assist accumulators   (0x5BB581, 0x5BB5CB)
		//   +0x988..+0x994    locked target position    (0x5BB809..0x5BB821)
		//   +0x998            owner yaw from 0x534F50   (0x5BB7E3, 0x5BBB43)
		//   +0x99C            aim pitch, or zero        (0x5BB9F2, 0x5BBB52)
		// The camera reads +0x998 and +0x99C back at 0x5206FF/0x52070E.
		constexpr size_t kCameraAimAssistOffset = 0x8B4u;
		constexpr size_t kCameraAimAssistFloats = 2u;
		constexpr size_t kCameraAimYawStateOffset = 0x988u;
		constexpr size_t kCameraAimYawStateFloats = 6u;
		// Fly_Active writes this larger request/apply window before it reaches the
		// raw laser branch: +0x91C, +0x940..+0x94C, +0x970 and +0x9A0..+0x9B5.
		// A shadow ability pass must restore the whole contiguous region so the
		// shared P1 camera cannot visibly snap toward Mooch for one frame.
		constexpr size_t kCameraFlyTransientOffset = 0x91Cu;
		constexpr size_t kCameraFlyTransientBytes = 0x9Cu; // +0x91C..+0x9B7
		// The bl gate of the yaw block at 0x5BBA98 picks what lands in
		// [turn_task+0x10]: if 0x4B6F40(handler+0x498) is the follow state 0x44110010
		// and that state's +0x3C is past [0x8B7824], the body receives its OWN current
		// yaw (0x5BBB89) and stops turning; otherwise it receives the shared camera
		// yaw from 0x52AD20 (0x5BBB67).  Because the handler is shared, P2's stock tick
		// leaves its own turn magnitude in +0x3C and flips that gate for P1.
		// CameraStateMachine::CurrentStateId: returns [current_state+8], or -1 when
		// the inline state machine has no current state. ECX is the state machine.
		constexpr uintptr_t kCameraStateMachineGetId = 0x004B6F40u;
		// CameraStateMachine::FindStateById: ECX is the state machine; one stack
		// argument is the state ID and EAX returns the matching object or null.
		constexpr uintptr_t kCameraStateMachineGetObject = 0x004B70E0u;
		constexpr size_t kCameraStateMachineOffset = 0x498u;
		constexpr size_t kCameraStateTurnOffset = 0x3Cu;
		constexpr uint32_t kCameraFollowStateId = 0x44110010u;
		constexpr uint32_t kCameraAimStateId = 0x4411000Cu;
		constexpr size_t kKeyboardStateBytesOffset = 0x04u;
		constexpr size_t kKeyboardStateSecondaryBytesOffset = 0x204u;
		constexpr size_t kKeyboardStateBytes = 256u;
		constexpr size_t kInputAimOriginOffset = 0x2774u;
		constexpr size_t kInputAimDirectionOffset = 0x2780u;
		constexpr size_t kXGamePadSize = 0x27ACu;

		constexpr uint32_t kGPig1Id = 0x79130001u;
		constexpr uint32_t kGPig2Id = 0x79130002u;
		constexpr uint32_t kDefaultMeleeItemId = 0x50000000u;
		constexpr uint32_t kInactiveModeId = 0x61000000u;
		constexpr uint32_t kDefaultModeId = 0x6100003Bu;
		constexpr uint32_t kDefaultModeConflictMask = 0x00000003u;
		constexpr uint32_t kP2DefaultExclusiveMask = 0x00000001u;
		// One-frame Darwin controller mode selected by the local Mooch-switch action.
		// Its next stock update must run on Darwin before P2 is allowed to tick.
		constexpr uint32_t kMoochSwitchModeId = 0x61000065u;
		// XGPigDeathMode family vtables. A remote P2 must not run these mode updates:
		// their native Respawn path owns checkpoint/start-position restart behavior.
		constexpr uintptr_t kGPigDeathModeVtable = 0x00700EC4u;
		constexpr uintptr_t kGPigDeathModeInactiveVtable = 0x00700F1Cu;
		constexpr uintptr_t kGPigDeathModeActiveVtable = 0x00700F74u;
		constexpr uintptr_t kGPigDeathModeFallVtable = 0x00700FCCu;
		constexpr uintptr_t kGPigDeathModeDeathVtable = 0x00701024u;
		constexpr uintptr_t kGPigDeathModeRespawnVtable = 0x0070107Cu;
		// Controller-owned native ABR vehicle/presentation mode. It selects and
		// guards the separate native path; never treat it as an ordinary on-foot
		// P2 transform or as a separately spawned world object.
		constexpr uint32_t kAbrModeId = 0x6100006Eu;
		// Exact retail XControllerMode_GPig_RDV class. Slot +0x0C is the outer
		// mode update; it invokes the gun helper at 0x005BD5D0 and owns the native
		// movement/camera preparation around it.
		constexpr uintptr_t kAbrModeVtable = 0x00718524u;
		constexpr uintptr_t kAbrModeUpdate = 0x005BDCD0u;
		constexpr uint8_t kExpectedAbrModeUpdate[] = {
			0x83, 0xEC, 0x44, 0x53, 0x55, 0x56, 0x8B, 0xF1
		};
		// The ABR vehicle owns a nested combat machine. The Fire state selects this
		// ID and the registered RTTI class below; neither identifier describes the
		// outer GPig controller mode or an ordinary Darwin weapon fire.
		constexpr uint32_t kGPigCombatRdvFireModeId = 0x61000035u;
		constexpr uintptr_t kGPigCombatRdvFireVtable = 0x0070098Cu;
		// XMotorSystem::EnsureGPigRdvTask. Static dump: ECX is XMotorSystem,
		// the bool argument is stack-owned and the function returns `ret 4`.
		// It indexes [XMotorSystem+0x2C] (= handler+0x4EC) using the engine-owned
		// index at 0x91568C, allocates the confirmed 0x78-byte task, and runs its
		// constructor at 0x4BA980. Do not replace this with a manual allocation or
		// a direct state-table write.
		constexpr uintptr_t kEnsureGPigRdvTask = 0x0040C9F0u;
		// ConfigureGPigRdvTaskForSpawnContext. Static dump: ECX is the settled spawn
		// context and the GPig handler is the sole stack argument. It takes
		// handler+0x4C0, calls kEnsureGPigRdvTask(true), then writes enabled=1 at
		// task+0x30. The remaining context layout is deliberately opaque.
		constexpr uintptr_t kConfigureGPigRdvTask = 0x0043EE20u;
		constexpr uintptr_t kGPigRdvTaskStateIndex = 0x0091568Cu;
		constexpr uintptr_t kGPigRdvTaskVtable = 0x006FC27Cu;
		constexpr size_t kGPigRdvTaskEnabledOffset = 0x30u;
		constexpr uint8_t kGPigRdvTaskEnabledValue = 1u;
		// XMotorTask_RDV constructor initializes all three to 1.0. The native
		// 0x4CA480 update moves current speed toward target by rate*dt, and
		// 0x4CA5C0 returns current speed multiplied by the RDV tuning value.
		constexpr size_t kGPigRdvTaskCurrentSpeedOffset = 0x40u;
		constexpr size_t kGPigRdvTaskTargetSpeedOffset = 0x44u;
		constexpr size_t kGPigRdvTaskAccelerationOffset = 0x48u;
		// XControllerMode_GPig_RDV lazily obtains this separate 44-byte drive task
		// through handler+0x4C0 and engine index 0x009155F8. Retail latches +0x04
		// after the first 0x10000000 edge, which is why the vehicle keeps driving
		// after the movement control is released.
		constexpr uintptr_t kGPigRdvDriveTaskStateIndex = 0x009155F8u;
		constexpr uintptr_t kGetGPigRdvDriveTask = 0x0040CA70u;
		constexpr uintptr_t kGPigRdvDriveTaskVtable = 0x006FC134u;
		constexpr size_t kGPigRdvDriveTaskActiveOffset = 0x04u;
		constexpr size_t kGPigRdvDriveTaskDirectionOffset = 0x10u;
		// XMotorFunction_GPigRDV resource used by the native RDV task.
		constexpr uintptr_t kGPigRdvMotorFunctionVtable = 0x007040DCu;
		constexpr uint32_t kGPigRdvMotorResourceIndex = 11u;
		// Only these two spawn-context fields are checked before the existing native
		// RDV configurator call; the remaining context layout is intentionally opaque.
		constexpr size_t kGPigSpawnContextFlagsOffset = 0x10u;
		constexpr size_t kGPigSpawnContextEntityOffset = 0xE8u;
		constexpr uint32_t kGPigSpawnContextRdvFlag = 0x20000000u;
		// This transient mode is observed around the native Mooch hand-off. The
		// controller's actual Active and Scanning modes are 0x61000007 and
		// 0x61000087; ownership is therefore tracked by the native +0x53 state flag,
		// not by treating either transient mode as the complete lifecycle.
		constexpr uint32_t kFlyOrbitModeId = 0x61000033u;
		// XControllerMode_Fly_Deactivated is registered by 0x5B3F80 with id
		// 0x61000075. It is a native local transition into the Respawn path, not a
		// peer-death signal: only the owning process may publish the zero-owner exit.
		constexpr uint32_t kFlyDeactivatedModeId = 0x61000075u;
		constexpr uint32_t kFirstKeyboardActionId = 0x10000000u;
		constexpr uint32_t kKeyboardActionCount = 0x43u;
		constexpr uint32_t kFireActionId = 0x10000007u;
		// The outer RDV mode uses this logical action to latch drive control. Query
		// its held level separately so custom key bindings remain valid.
		constexpr uint32_t kAbrMoveActionId = 0x10000000u;
		// RDV/ABR polls this separate pressed-edge action at 0x005BDBD8.
		constexpr uint32_t kAbrFireActionId = 0x10000014u;
		constexpr uintptr_t kAbrFirePressedQueryReturn = 0x005BDBDDu;
		// Retail RDV and Default controller modes are registered with mask 3.
		// P2 is presentation-only and must not claim the exclusive owner bit.
		constexpr uint32_t kAbrModeConflictMask = 0x00000003u;
		// The Default-mode second-action query 0x4008000A maps to this ordinary
		// pressed edge.  Its physical bind is intentionally not named here.
		constexpr uint32_t kLedgeReleaseActionId = 0x1000000Du;

		// Actions that must never be driven by the remote snapshot, pinned by INDEX so a
		// rebind cannot reopen them.  The live table dumped from the shipped build
		// (profile 1) is the ground truth here:
		//   n=0x09 -> DIK 0x10 (Q)  - map UI, kept local to each process
		//   n=0x0E -> DIK 0x14 (T)  - the Mooch-mode switch proven in code: 0x5BBC80
		//                             gates on 0x488CE0(pad, dev, 0x1000000E, 1) at
		//                             0x5BBCAC and calls [vtable+0x20](0x61000065) at
		//                             0x5BBD94
		// DIK 0x0F (TAB) is bound to NO action in that table, so night vision is not a
		// logical action at all - it is read straight out of the DirectInput buffer, and
		// the only thing that can close it is the VK_TAB skip in BuildRemoteScanCodeState
		// and HandleGetAsyncKeyState.  Nothing to pin there, and nothing to rebind.
		constexpr uint32_t kMoochActionIndex = 0x0Eu;
		constexpr uint32_t kMapActionIndex = 0x09u;

		constexpr uint8_t kExpectedSha256[32] = {
			0xBF, 0xDB, 0x49, 0x30, 0x33, 0x14, 0xCA, 0x8F,
			0x75, 0xE6, 0xF1, 0x10, 0xAB, 0x75, 0xF4, 0x5B,
			0x64, 0x11, 0x77, 0xAB, 0x33, 0x9D, 0x3E, 0x29,
			0x15, 0x55, 0xA2, 0xA5, 0x3C, 0xB9, 0xE0, 0xC3
		};

		constexpr uint8_t kExpectedSpawnCall1[5] =
		{ 0xE8, 0x68, 0x5F, 0x10, 0x00 };
		constexpr uint8_t kExpectedSpawnCall2[5] =
		{ 0xE8, 0xCE, 0x5E, 0x10, 0x00 };
		constexpr uint8_t kExpectedDefaultModeActiveStores[10] = {
			0xA3, 0xA4, 0x2A, 0x91, 0x00,
			0xA3, 0x88, 0x27, 0x91, 0x00
		};
		constexpr uint8_t kExpectedInputActionQuery[5] =
		{ 0x53, 0x8B, 0x5C, 0x24, 0x0C };
		constexpr uint8_t kExpectedInputActionUpQuery[5] =
		{ 0x53, 0x8B, 0x5C, 0x24, 0x0C };
		constexpr uint8_t kExpectedInputThresholdQuery[5] =
		{ 0x56, 0x8B, 0x74, 0x24, 0x0C };
		constexpr uint8_t kExpectedInputAxisQuery[5] =
		{ 0xA1, 0x0C, 0xD2, 0x8F, 0x00 };
		// Pressed/released edge queries share the same prologue.  The first three whole
		// instructions (push ecx; push ebx; mov ebx,[esp+10h]) span six bytes, so a
		// 5-byte E9 patch must relocate six bytes, not five.
		constexpr uint8_t kExpectedInputPressedQuery[6] =
		{ 0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10 };
		constexpr uint8_t kExpectedInputReleasedQuery[6] =
		{ 0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10 };
		// Hold-duration query prologue (push esi; mov esi,[esp+0Ch]) is a clean 5 bytes.
		constexpr uint8_t kExpectedInputHoldDurationQuery[5] =
		{ 0x56, 0x8B, 0x74, 0x24, 0x0C };
		// 0x488B00 has the same clean 5-byte prologue; [esp+0Ch] after the push is the
		// action word, which the next instruction tests against 0x10000000.
		constexpr uint8_t kExpectedInputAimHoldQuery[5] =
		{ 0x56, 0x8B, 0x74, 0x24, 0x0C };
		// 0x48AE10, 0x48AF10 and 0x48AF90 begin with the complete five-byte
		// `cmp byte ptr [esp+10h], 0` instruction.
		constexpr uint8_t kExpectedInputRawQuery[5] =
		{ 0x80, 0x7C, 0x24, 0x10, 0x00 };
		// 0x5BEA00 starts with an MSVC SEH frame. Its first two complete instructions
		// are seven bytes and have no relative operands, so the trampoline may copy
		// them verbatim.
		constexpr uint8_t kExpectedDefaultModeUpdate[7] =
		{ 0x6A, 0xFF, 0x68, 0x5B, 0x74, 0x6E, 0x00 };
		constexpr uint8_t kExpectedFireHandler[7] =
		{ 0x6A, 0xFF, 0x68, 0x0B, 0x71, 0x6E, 0x00 };
		// `sub esp,8` followed by its complete seven-byte global flag test.  The
		// trampoline must copy both instructions: relocating five bytes would split
		// the `cmp byte ptr [0x912541], 0` instruction.
		constexpr uint8_t kExpectedWeaponAmmoConsume[10] =
		{ 0x83, 0xEC, 0x08, 0x80, 0x3D, 0x41, 0x25, 0x91, 0x00, 0x00 };
		// 0x5BCF30: sub esp,20h + fldz is exactly five bytes and has no relative
		// operand.  The fldz is balanced by the fstp at 0x5BCF3D, so the trampoline may
		// hold it; the skip path never executes either.
constexpr uint8_t kExpectedGPigCameraUpdate[5] =
{ 0x83, 0xEC, 0x20, 0xD9, 0xEE };
		// 0x52AD20: mov eax,ecx + cmp byte ptr [eax+1AB4h],0 is nine bytes.  The
		// relocated cmp sets the flags that the original je at 0x52AD29 consumes, and
		// flags survive the trampoline's jmp back.
		constexpr uint8_t kExpectedCameraYawGetter[9] =
		{ 0x8B, 0xC1, 0x80, 0xB8, 0xB4, 0x1A, 0x00, 0x00, 0x00 };
	}
}
