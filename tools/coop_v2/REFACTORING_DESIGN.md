# Технический дизайн переписи co-op DLL

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
| `Transform` | `{ Vec4 position; Vec4 rotation; }`. | Read/write transform без двух несвязанных float-массивов. |
| `EntityRef` | Не-null process-local address; доступ только через `EntityView`. | P1, P2, NPC, dynamic entity. |
| `ControllerRef` | Не-null process-local address; owner и current mode подтверждены accessors. | Controller update и camera refresh. |
| `TriggerRef` | Не-null process-local address; family/subtype/definition/transform читаются через `TriggerView`. | Templates, spawn hooks, trigger pulse. |
| `PlayerSlot` | `LocalP1 = 1`, `RemoteP2 = 2`, `AuxiliaryP3 = 3`, `Mooch = 4`. | Вместо integer slot magic. |
| `PacketKind` | `enum class std::uint32_t`, exact existing wire IDs. | Typed router и validation. |

## Memory boundary

`retail/memory.h` становится **единственным** местом с `__try` для простого чтения/записи fixed offsets. Наружу он отдаёт `bool` и typed value через output parameter. Feature-код не должен содержать `reinterpret_cast<BYTE*> + offset` или ловить SEH при обычном field access.

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

enum class PlayerSlot : std::uint32_t { LocalP1 = 1, RemoteP2 = 2, AuxiliaryP3 = 3, Mooch = 4 };

class EntityView final {
public:
    explicit EntityView(EntityRef entity);
    bool ReadTransform(Transform& value) const;
    bool WriteTransform(const Transform& value) const;
    bool Handler(HandlerRef& handler) const;
    bool Trigger(TriggerRef& trigger) const;
};

class ControllerView final {
public:
    explicit ControllerView(ControllerRef controller);
    bool Owner(HandlerRef& handler) const;
    bool ModeId(ModeId& mode) const;
    bool SelectMode(ModeId mode, bool force_reselect = false) const;
};

class TriggerView final {
public:
    explicit TriggerView(TriggerRef trigger);
    bool Identity(TriggerIdentity& value) const;
    bool ReadTransform(Transform& value) const;
    bool SpawnedEntity(EntityRef& entity) const;
    bool DispatchEvent(EventCode event) const;
};

class PlayerRepository final {
public:
    bool Get(PlayerSlot slot, EntityRef& entity) const;
    bool GetController(PlayerSlot slot, ControllerRef& controller) const;
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

## Scope guards instead of manual restore ladders

The old P2 update uses booleans and duplicated cleanup in normal and exception paths. It becomes nested RAII scopes whose destructors restore only the state they actually acquired.

| Scope | Acquires | Restores |
|---|---|---|
| `RemoteInputScope` | Snapshot, edge state and DirectInput keyboard buffers. | Both keyboard buffers and thread-local remote-input marker. |
| `PrimaryGamePadScope` | Temporary replacement of global P1 pad when Mooch requires isolation. | Original P1 pad. |
| `SharedCameraStateScope` | Shared camera values that P2 stock tick may overwrite. | Exact saved camera fields. |
| `TransformPulseScope` | Local P1 original transform while trigger pulse FIFO is active. | Original transform after last queued pulse. |
| `PatchSet` | Multiple expected-byte-gated patch handles. | Only patches still pointing at this DLL. |

These scopes must be stack-only and non-copyable. They remove most `goto`-style cleanup and make a failed setup leave the game unchanged.

## Dependency rules

`network` calls the protocol router. `protocol` feeds `input` and `world` queues. `player` requests a `RemoteInputScope` but does not inspect socket state directly. `world` requests packet emission through an abstract `PacketSender`, but never references `MClient`, `MServer` or `SteamManager`. `retail` has no dependency on protocol, network or gameplay.

The only intended singleton-like composition root remains `CoopApplication`. It owns concrete services in startup order. Existing singleton entry points remain as thin compatibility facades during migration and are deleted only after all old call sites have moved.

## Migration acceptance criteria

A migrated module is accepted only when it retains its exact native function signatures and packet sizes, compiles under current `cl.exe /W4 /EHsc`, has no raw `BYTE* + k...Offset` outside `retail/`, has no broad `catch (...)` around ordinary registry walking, and has no feature-level `__try` that can be replaced by a typed view method. Build success is required before the next module moves.
