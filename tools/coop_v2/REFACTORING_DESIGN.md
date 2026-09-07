# Технический дизайн переписи co-op DLL

> **Supplementary design reference, not the current source map.** For a change
> in this tree, `IMPLEMENTATION.md`, `../../re_cache/RE_CATALOG.md` and the
> checked source win. This document may describe a future module layout; it
> does not prove a retail ABI, enable P3, or authorise an unimplemented move.

## Принцип: ABI views, а не выдуманные полные классы

У retail EXE нет доступных C++ headers, а все layout известны только частично. Поэтому `struct GPig { ... padding ... }` был бы ложной моделью и создавал бы новую угадайку. Новый слой использует маленькие **не владеющие views**, которые знают только подтверждённые поля и native calls. Каждый view хранит адрес экземпляра, но не владеет памятью и не предполагает размер класса.

```text
Native hook / packet ingress
        ↓
retail::EntityRef / ControllerRef / TriggerRef / GamePadRef
        ↓
feature services: player, input, camera, fly, world
        ↓
protocol packet builders and transport facade
```

## Базовые value-типы

| Тип | Инвариант | Использование |
|---|---|---|
| `Address` | `std::uintptr_t`; адрес валиден только в данном процессе. | Внутри `retail` и `patching`, но не в wire packets. |
| `Vec3` | Три float без неявного `w`. | Aim ray, distance, wire data из трёх координат. |
| `Vec4` | Четыре float, exact x86 ABI поля EXE. | Entity/trigger position и rotation. |
| `AimRay` | `{ Vec3 origin; Vec3 direction; }`, ровно шесть retail float. | Временная подмена aim ray на точном synchronous fire call. |
| `Transform` | `{ Vec4 position; Vec4 rotation; }`. | Read/write transform без двух несвязанных float-массивов. |
| `EntityRef` | Не-null process-local address; доступ только через `EntityView`. | P1, P2, NPC, dynamic entity. |
| `EntitySlotBinding` | Momentary `{ slot, entity, handler, controller }` from the six-entry table; not an ownership/lifecycle claim. | Slot-aware P1/P2 checks without P2-specific pointer plumbing; P3 remains unenabled. |
| `EntityRegistryView` | Один global registry с двумя подтверждёнными intrusive-list; callback получает только `EntityRef`. | Поиск live entity и host/client world enumeration без `BYTE*` в feature-коде. |
| `ControllerRef` | Не-null process-local address; owner и current mode подтверждены accessors. | Controller update и camera refresh. |
| `ModeRef` | Частичный native controller-mode object: vtable, ID и conflict mask доступны через `ModeView`. | Death-mode guard и P2 Default arbitration без raw casts. |
| `MotorSystemRef` | Inline-область motor system внутри GPig handler; известны только resource/task tables. | Проверка и stock factory `XMotorTask_RDV` без `handler + offset` в `player2.cpp`. |
| `MotorResourceRef` / `MotorTaskRef` | Process-local table entries; доступны лишь vtable и для RDV task флаг enabled. | Проверка existing/created native RDV task. |
| `SpawnContextRef` | Непрозрачный аргумент stock GPig factory; доступны только flags и active entity. | Предусловия перед stock RDV configurator. |
| `CameraHandlerRef` / `CameraStateRef` | Единственный process-local camera handler и выбранный native state object; известны target, aim snapshots и follow-turn. | `SharedCameraCoordinator` изолирует общий camera state вокруг remote-controller tick, не привязывая это к P2. |
| `InputManagerRef` | Непрозрачный native input manager; доступны только два подтверждённых aim-ray блока через `InputManagerView`. | Захват P1 ray и короткая подмена P2 ray при fire handler. |
| `WeaponAmmoItemRef` / `AmmoPoolEntryRef` | Непрозрачные native inventory record и shared-pool entry; известны только count/id поля из stock consume/HUD. | Безопасное сохранение P2 ammo и P1 shared HUD pool вокруг stock shot. |
| `TriggerRef` | Не-null process-local address; family/subtype/definition/transform читаются через `TriggerView`. | Templates, spawn hooks, trigger pulse. |
| `EntitySlot` | Шесть native-ячееек: `P1 = 1`, `P2 = 2`, `P3 = 3`, `Mooch = 4`; slot `5` пока не классифицирован. | Вместо integer slot magic без ложного обещания 6 игроков. |
| `PacketKind` | `enum class std::uint32_t`, exact existing wire IDs. | Typed router и validation. |

## Memory boundary

`retail/retail_memory.h` становится **единственным** местом с `__try` для
простого чтения/записи fixed offsets. Наружу он отдаёт `bool` и typed value
через output parameter. Feature-код не должен содержать
`reinterpret_cast<BYTE*> + offset` или ловить SEH при обычном field access.
Raw offsets остаются необходимыми внутри `retail/`; цель — не удалить их, а
изолировать в одном ABI boundary.

| Низкоуровневый primitive | Назначение |
|---|---|
| `TryRead<T>(Address, T&)` | Копирует scalar или trivially-copyable value из проверяемого адреса. |
| `TryWrite<T>(Address, const T&)` | Записывает scalar или trivially-copyable value в проверяемый адрес. |
| `TryReadPointer(Address, Address&)` | Явно читает x86 pointer из поля/глобала. |
| `TryCall(Fn, Args...)` | Оставляется только для native calls, которые действительно могут fault; не применяется как замена логике ошибок. |

SEH остаётся вокруг трёх классов реальных рисков: dereference live game object, вызов unknown-lifetime native function и patch write. Убираются `__try` вокруг проверок, алгоритмов и нормального control flow.

## Retail view API

```cpp
namespace coop::retail {

struct Transform { Vec4 position; Vec4 rotation; };

enum class EntitySlot : std::uint16_t {
    Slot0 = 0, LocalP1 = 1, RemoteP2 = 2, AuxiliaryP3 = 3,
    Mooch = 4, Unclassified5 = 5, None = 0xFFFF
};

class EntityView final {
public:
    explicit EntityView(EntityRef entity);
    bool ReadTransform(Transform& value) const;
    bool WriteTransform(const Transform& value) const;
    bool Handler(HandlerRef& handler) const;
    bool Trigger(TriggerRef& trigger) const;
};

class EntityRegistryView final {
public:
    // false у callback означает успешную раннюю остановку.
    template<typename Visitor>
    bool VisitLiveEntities(std::size_t per_list_limit, Visitor visitor) const;
};

class ControllerView final {
public:
    explicit ControllerView(ControllerRef controller);
    bool Owner(HandlerRef& handler) const;
    bool ModeId(ModeId& mode) const;
    bool SelectMode(ModeId mode, bool force_reselect = false) const;
};

class HandlerView final {
public:
    explicit HandlerView(HandlerRef handler);
    bool Controller(ControllerRef& controller) const;
    bool HealthComponent(HealthComponentRef& component) const;
    bool MotorSystem(MotorSystemRef& system) const;
    bool SetFlyControlActive(std::uint32_t state, bool active) const;
};

class InputManagerView final {
public:
    explicit InputManagerView(InputManagerRef input);
    bool ReadAimRay(AimRay& value) const;
    bool WriteAimRay(const AimRay& value) const;
};

class WeaponAmmoItemView final {
public:
    explicit WeaponAmmoItemView(WeaponAmmoItemRef item);
    bool RoundCount(std::uint32_t& value) const;
    bool SetRoundCount(std::uint32_t value) const;
    bool AmmoId(std::uint32_t& value) const;
};

class AmmoPoolEntryView final {
public:
    explicit AmmoPoolEntryView(AmmoPoolEntryRef entry);
    bool CurrentAmount(std::uint32_t& value) const;
    bool SetCurrentAmount(std::uint32_t value) const;
};

class MotorSystemView final {
public:
    explicit MotorSystemView(MotorSystemRef system);
    bool ResourceAt(std::uint32_t index, MotorResourceRef& resource) const;
    bool TaskAt(std::uint32_t state, MotorTaskRef& task) const;
};

class MotorTaskView final {
public:
    explicit MotorTaskView(MotorTaskRef task);
    bool VTable(Address& value) const;
    bool RdvEnabled(std::uint8_t& value) const;
};

class SpawnContextView final {
public:
    explicit SpawnContextView(SpawnContextRef context);
    bool Flags(std::uint32_t& value) const;
    bool ActiveEntity(EntityRef& entity) const;
};

class CameraHandlerView final {
public:
    explicit CameraHandlerView(CameraHandlerRef handler);
    bool TargetControllerId(std::uint32_t& value) const;
    bool ReadAimAssist(CameraAimAssistState& value) const;
    bool WriteAimAssist(const CameraAimAssistState& value) const;
    bool ReadAimYawState(CameraAimYawState& value) const;
    bool WriteAimYawState(const CameraAimYawState& value) const;
};

class HealthComponentView final {
public:
    explicit HealthComponentView(HealthComponentRef component);
    bool ReadSlot(std::uint32_t slot, float& value) const;
};

class TriggerView final {
public:
    explicit TriggerView(TriggerRef trigger);
    bool Identity(TriggerIdentity& value) const;
    bool ReadTransform(Transform& value) const;
    bool SpawnedEntity(EntityRef& entity) const;
    bool DispatchEvent(EventCode event) const;
};

class EntitySlotRepository final {
public:
    bool Get(EntitySlot slot, EntityRef& entity) const;
    bool GetByIndex(std::uint32_t slot, EntityRef& entity) const;
    bool GetHandler(EntitySlot slot, HandlerRef& handler) const;
    bool GetController(EntitySlot slot, ControllerRef& controller) const;
};

} // namespace coop::retail
```

`TriggerIdentity` is process-neutral only after it has been copied into protocol data. It contains family, subtype and definition ID; pointer address and local occurrence counter stay inside local world registry.

## Protocol and dispatch

The existing `PacketHeader` field layout and every existing packet `sizeof` assertion remain unchanged. `PacketHeader` becomes a plain wire header and does not mix transport convenience methods with feature semantics.

```cpp
bool ProtocolRouter::Route(const void* data, std::uint32_t size) {
    PacketView view(data, size);
    if (!view.ReadHeader(header) || !view.HasExactDeclaredSize()) return false;

    switch (static_cast<PacketKind>(header.packet_id)) {
    case PacketKind::Input:              return input_.Accept(view);
    case PacketKind::WorldSpawn:         return world_.AcceptSpawn(view);
    case PacketKind::WorldSnapshot:      return world_.AcceptSnapshot(view);
    case PacketKind::WorldReady:         return world_.AcceptReady(view);
    case PacketKind::WorldTriggerEvent:  return world_.AcceptTriggerEvent(view);
    case PacketKind::WorldDamage:        return world_.AcceptDamage(view);
    case PacketKind::TriggerP1Teleport:  return world_.AcceptTriggerPulse(view);
    default:                             return false;
    }
}
```

Every branch has `ExactPacket<T>(view, packet)` validation before copying a packet. A packet can be accepted at worker-thread ingress only by copying wire bytes into the destination queue. No packet handler may dereference a retail pointer or call a game function on the socket worker.

Input is a latest-state stream on an unreliable lane, not an event queue. Its
nonzero `transform_sequence` is monotonic at the sender and the receiver
accepts only a newer value using signed wrap-around comparison. It must retain
the last accepted snapshot between packets; a delayed packet must never roll
back P2 or Mooch position/rotation. The zero value is reserved for the
pre-first-transform sentinel.

Mooch has a second monotonic `fly_transform_sequence` for its live transform.
The lifecycle authority remains the whole ordered input snapshot: when the
local owner enters `Fly_Deactivated`, it publishes a newer zero-owner snapshot.
The receiver never infers peer death from its own `Fly_Deactivated` mode because
that native transition can happen independently in each process; it accepts the
newer zero-owner state and rejects older live snapshots by `transform_sequence`.

## Scope guards instead of manual restore ladders

The old P2 update uses booleans and duplicated cleanup in normal and exception paths. It becomes nested RAII scopes whose destructors restore only the state they actually acquired.

| Scope | Acquires | Restores |
|---|---|---|
| `RemoteSnapshotInputScope` | One frozen snapshot, edge state, all four XGamePad analog axes, Fly raw actions and DirectInput keyboard buffers for remote P2 or the remote-owned shared Fly. | Both keyboard buffers and thread-local remote-input marker. |
| `PrimaryGamePadScope` | Temporary replacement of global P1 pad when Mooch requires isolation. | Original P1 pad. |
| `SharedCameraStateScope` | Shared camera values that P2 stock tick may overwrite. | Exact saved camera fields. |
| `TransformPulseScope` | Local P1 original transform while trigger pulse FIFO is active. | Original transform after last queued pulse. |
| `PatchSet` | Multiple expected-byte-gated patch handles. | Only patches still pointing at this DLL. |

These scopes must be stack-only and non-copyable. They remove most `goto`-style cleanup and make a failed setup leave the game unchanged.

## Dependency rules

`network` calls the protocol router. `protocol` feeds `input` and `world` queues. `player` requests a `RemoteSnapshotInputScope` for remote-native ticks but does not inspect socket state directly. `world` requests packet emission through an abstract `PacketSender`, but never references `MClient`, `MServer` or `SteamManager`. `retail` has no dependency on protocol, network or gameplay.

The only intended singleton-like composition root remains `CoopApplication`. It owns concrete services in startup order. Existing singleton entry points remain as thin compatibility facades during migration and are deleted only after all old call sites have moved.

## Migration acceptance criteria

A migrated module is accepted only when it retains its exact native function signatures and packet sizes, compiles under current `cl.exe /W4 /EHsc`, has no raw `BYTE* + k...Offset` outside `retail/`, has no broad `catch (...)` around ordinary registry walking, and has no feature-level `__try` that can be replaced by a typed view method. Build success is required before the next module moves.
