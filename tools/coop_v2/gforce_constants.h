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
constexpr uintptr_t kDefaultModeActiveStores = 0x005BEAD6u;
constexpr uintptr_t kGPigUpdateVtableSlot = 0x0070C8A4u;
constexpr uintptr_t kOriginalControllerUpdate = 0x005BFBE0u;
constexpr uintptr_t kInputActionQuery = 0x00488A70u;      // is-down (level)
constexpr uintptr_t kInputActionUpQuery = 0x00488B70u;    // is-up (inverse level)
constexpr uintptr_t kInputThresholdQuery = 0x00488DC0u;   // hold + threshold
constexpr uintptr_t kInputAxisQuery = 0x0048B010u;        // analog axis
constexpr uintptr_t kInputPressedQuery = 0x00488CE0u;     // pressed this frame (rising edge)
constexpr uintptr_t kInputReleasedQuery = 0x00488C00u;    // released this frame (falling edge)
constexpr uintptr_t kInputHoldDurationQuery = 0x00488E50u;// hold duration >= threshold
// 0x488B00 is the aim-hold query of 0x5BB1D0 (0x5BB321 with float [0x6F26D8],
// 0x5BB34D with float [0x6F2754]).  Same shape as 0x488DC0 but a different
// function AND a different argument order: the flags word is pushed before the
// float, so this is (device, action, flags, threshold).  While it stayed
// unhooked, P2's aim branch was decided by the local physical mouse instead of
// the remote snapshot.
constexpr uintptr_t kInputAimHoldQuery = 0x00488B00u;     // aim hold + threshold
// 0x5BEA00 receives the active XGamePad/input manager as argument #1.  Its
// inner fire routine (0x5B8760) consumes that same pointer from [esp+4Ch].
constexpr uintptr_t kDefaultModeUpdate = 0x005BEA00u;
// Copies XGamePad +0x2774/+0x2780 into the native projectile command.
constexpr uintptr_t kFireHandler = 0x005B8760u;
constexpr uintptr_t kXGamePadCtor = 0x0048B290u;

constexpr uintptr_t kGamePointer = 0x00912784u;
// The process-wide XGamePad selected by XGamePad::Register at 0x487F10.
// P1 owns this pointer permanently. P2's pad is constructed but never
// registered, then passed directly to Default-mode calls instead.
constexpr uintptr_t kPrimaryGamePad = 0x009905CCu;
constexpr uintptr_t kActiveEntityA = 0x00912AA4u;
constexpr uintptr_t kActiveEntityB = 0x00912788u;
constexpr uintptr_t kGPigEntityArray = 0x009128D8u;
// Slot 4 of the same array (0x9128D8 + 4*4): Mooch, the fly.  It is NOT one of
// the guinea-pig slots 1..3, so P2 in slot 2 does not collide with it.  The fly
// switch at 0x5BBC80 refuses to run when this is null (0x5BBD00), and reads the
// fly's handler through [fly + 0x144] at 0x5BBD6B.
constexpr uintptr_t kFlyEntity = 0x009128E8u;
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
constexpr uintptr_t kCameraYawGetter = 0x0052AD20u;
constexpr size_t kCameraRequestedStateOffset = 0x9A0u;
constexpr uintptr_t kGetCurrentWeaponId = 0x00544A30u;
constexpr uintptr_t kSetSelectedWeaponType = 0x005434E0u;
constexpr uintptr_t kWeaponTypeToItemId = 0x00543520u;

constexpr size_t kEntityHandlerOffset = 0x144u;
constexpr size_t kEntityRotationOffset = 0xC8u;
constexpr size_t kEntityPositionOffset = 0xE8u;
constexpr size_t kHandlerControllerOffset = 0x510u;
constexpr size_t kHandlerSelectedWeaponTypeOffset = 0x26E0u;
constexpr size_t kControllerOwnerOffset = 0x18u;
constexpr size_t kControllerModeOffset = 0x1Cu;
constexpr size_t kModeIdOffset = 0x08u;
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
// The bl gate of the yaw block at 0x5BBA98 picks what lands in
// [turn_task+0x10]: if 0x4B6F40(handler+0x498) is the follow state 0x44110010
// and that state's +0x3C is past [0x8B7824], the body receives its OWN current
// yaw (0x5BBB89) and stops turning; otherwise it receives the shared camera
// yaw from 0x52AD20 (0x5BBB67).  Because the handler is shared, P2's stock tick
// leaves its own turn magnitude in +0x3C and flips that gate for P1.
constexpr uintptr_t kCameraStateMachineGetId = 0x004B6F40u;
constexpr uintptr_t kCameraStateMachineGetObject = 0x004B70E0u;
constexpr size_t kCameraStateMachineOffset = 0x498u;
constexpr size_t kCameraStateTurnOffset = 0x3Cu;
constexpr uint32_t kCameraFollowStateId = 0x44110010u;
constexpr uint32_t kCameraAimStateId = 0x4411000Cu;
constexpr size_t kKeyboardStateBytesOffset = 0x04u;
constexpr size_t kKeyboardStateSecondaryBytesOffset = 0x204u;
constexpr size_t kKeyboardStateBytes = 256u;
constexpr size_t kInputActionBindingOffset = 0x2668u;
constexpr size_t kInputAimOriginOffset = 0x2774u;
constexpr size_t kInputAimDirectionOffset = 0x2780u;
constexpr size_t kXGamePadSize = 0x27ACu;

constexpr uint32_t kGPig1Id = 0x79130001u;
constexpr uint32_t kGPig2Id = 0x79130002u;
constexpr uint32_t kDefaultMeleeItemId = 0x50000000u;
constexpr uint32_t kInactiveModeId = 0x61000000u;
constexpr uint32_t kDefaultModeId = 0x6100003Bu;
// One-frame Darwin controller mode selected by the local Mooch-switch action.
// Its next stock update must run on Darwin before P2 is allowed to tick.
constexpr uint32_t kMoochSwitchModeId = 0x61000065u;
// Modes of the FLY's own controller, i.e. the one reached through
// [[0x9128E8 + 0x144] + 0x510], not a GPig mode.  Read live from the log: every
// Mooch toggle produced 0x61000033 -> 0x61000034, and 0x61000034 was then lost
// again with no further input, so 0x61000033 is the idle/follow mode the fly
// sits in while Darwin is played and 0x61000034 is the one it holds while the
// fly is the controlled character.
constexpr uint32_t kFlyIdleModeId = 0x61000033u;
constexpr uint32_t kFlyControlledModeId = 0x61000034u;
constexpr uint32_t kFirstKeyboardActionId = 0x10000000u;
constexpr uint32_t kKeyboardActionCount = 0x43u;
constexpr uint32_t kFireActionId = 0x10000007u;

// 0x488A70 resolves a logical action to its binding through
// [XGamePad + kInputActionBindingOffset + index*4] and then dispatches on the
// binding's high bits: 0x40000000 selects the mask namespace at [0x99B6B0 + 4],
// the sign bit selects the DirectInput keyboard buffer ([0xAA6580] + 4 indexed by
// the low byte = DIK scan code), and anything else is a device button mask
// ([pad + device*8 + 4]).  So the low byte of a sign-flagged binding is exactly
// the DIK code the player has to press.
constexpr uint32_t kActionBindingMaskNamespace = 0x40000000u;
constexpr uint32_t kActionBindingKeyboardFlag = 0x80000000u;
constexpr uint32_t kDikTab = 0x0Fu;
constexpr uint32_t kDikQ = 0x10u;
// Actions that must never be driven by the remote snapshot, pinned by INDEX so a
// rebind cannot reopen them.  The live table dumped from the shipped build
// (profile 1) is the ground truth here:
//   n=0x09 -> DIK 0x10 (Q)  - the key the player uses to call the fly
//   n=0x0E -> DIK 0x14 (T)  - the Mooch-mode switch proven in code: 0x5BBC80
//                             gates on 0x488CE0(pad, dev, 0x1000000E, 1) at
//                             0x5BBCAC and calls [vtable+0x20](0x61000065) at
//                             0x5BBD94
// DIK 0x0F (TAB) is bound to NO action in that table, so night vision is not a
// logical action at all - it is read straight out of the DirectInput buffer, and
// the only thing that can close it is the VK_TAB skip in BuildRemoteScanCodeState
// and HandleGetAsyncKeyState.  Nothing to pin there, and nothing to rebind.
constexpr uint32_t kMoochActionIndex = 0x0Eu;
constexpr uint32_t kFlySummonActionIndex = 0x09u;
// [XGamePad + 0x278C] is the profile selector 0x48A3A5 branches on: 0 keeps the
// pad-mask defaults from 0x48A627, 1 and 2 install the two developer keyboard sets
// (0x48A509 / 0x48A3EB).  The shipped build runs profile 1, but the live table has
// far more entries than 0x48A509 writes (fire n=0x07 arrives later as the mouse
// mask 0x40000001), so the real layout still only exists at runtime.
constexpr size_t kActionBindingProfileOffset = 0x278Cu;

constexpr uint8_t kExpectedSha256[32] = {
	0xBF, 0xDB, 0x49, 0x30, 0x33, 0x14, 0xCA, 0x8F,
	0x75, 0xE6, 0xF1, 0x10, 0xAB, 0x75, 0xF4, 0x5B,
	0x64, 0x11, 0x77, 0xAB, 0x33, 0x9D, 0x3E, 0x29,
	0x15, 0x55, 0xA2, 0xA5, 0x3C, 0xB9, 0xE0, 0xC3
};

constexpr uint8_t kExpectedSpawnCall1[5] =
	{0xE8, 0x68, 0x5F, 0x10, 0x00};
constexpr uint8_t kExpectedSpawnCall2[5] =
	{0xE8, 0xCE, 0x5E, 0x10, 0x00};
constexpr uint8_t kExpectedDefaultModeActiveStores[10] = {
	0xA3, 0xA4, 0x2A, 0x91, 0x00,
	0xA3, 0x88, 0x27, 0x91, 0x00
};
constexpr uint8_t kExpectedInputActionQuery[5] =
	{0x53, 0x8B, 0x5C, 0x24, 0x0C};
constexpr uint8_t kExpectedInputActionUpQuery[5] =
	{0x53, 0x8B, 0x5C, 0x24, 0x0C};
constexpr uint8_t kExpectedInputThresholdQuery[5] =
	{0x56, 0x8B, 0x74, 0x24, 0x0C};
constexpr uint8_t kExpectedInputAxisQuery[5] =
	{0xA1, 0x0C, 0xD2, 0x8F, 0x00};
// Pressed/released edge queries share the same prologue.  The first three whole
// instructions (push ecx; push ebx; mov ebx,[esp+10h]) span six bytes, so a
// 5-byte E9 patch must relocate six bytes, not five.
constexpr uint8_t kExpectedInputPressedQuery[6] =
	{0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10};
constexpr uint8_t kExpectedInputReleasedQuery[6] =
	{0x51, 0x53, 0x8B, 0x5C, 0x24, 0x10};
// Hold-duration query prologue (push esi; mov esi,[esp+0Ch]) is a clean 5 bytes.
constexpr uint8_t kExpectedInputHoldDurationQuery[5] =
	{0x56, 0x8B, 0x74, 0x24, 0x0C};
// 0x488B00 has the same clean 5-byte prologue; [esp+0Ch] after the push is the
// action word, which the next instruction tests against 0x10000000.
constexpr uint8_t kExpectedInputAimHoldQuery[5] =
	{0x56, 0x8B, 0x74, 0x24, 0x0C};
// 0x5BEA00 starts with an MSVC SEH frame. Its first two complete instructions
// are seven bytes and have no relative operands, so the trampoline may copy
// them verbatim.
constexpr uint8_t kExpectedDefaultModeUpdate[7] =
	{0x6A, 0xFF, 0x68, 0x5B, 0x74, 0x6E, 0x00};
constexpr uint8_t kExpectedFireHandler[7] =
	{0x6A, 0xFF, 0x68, 0x0B, 0x71, 0x6E, 0x00};
// 0x5BCF30: sub esp,20h + fldz is exactly five bytes and has no relative
// operand.  The fldz is balanced by the fstp at 0x5BCF3D, so the trampoline may
// hold it; the skip path never executes either.
constexpr uint8_t kExpectedGPigCameraUpdate[5] =
	{0x83, 0xEC, 0x20, 0xD9, 0xEE};
// 0x52AD20: mov eax,ecx + cmp byte ptr [eax+1AB4h],0 is nine bytes.  The
// relocated cmp sets the flags that the original je at 0x52AD29 consumes, and
// flags survive the trampoline's jmp back.
constexpr uint8_t kExpectedCameraYawGetter[9] =
	{0x8B, 0xC1, 0x80, 0xB8, 0xB4, 0x1A, 0x00, 0x00, 0x00};
}
}
