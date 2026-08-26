# Контракт полной архитектурной переписи co-op DLL

Этот документ фиксирует **поведение, которое перепись обязана сохранить**, и границы новой архитектуры. Он не добавляет новых игровых механик и не изменяет wire ABI: обе стороны co-op должны оставаться совместимыми только с DLL той же ревизии.

## Неприкосновенные runtime-контракты

| Контракт | Сохранить при переписи | Граница ответственности |
|---|---|---|
| Retail gate | Жёсткая проверка fingerprint проверенного x86 `GForce.exe`, expected bytes перед каждым патчем и восстановление только собственных hooks. | `runtime/` и `retail/patching/` |
| Локальный P1 | Игрок, которым владеет данный процесс, тикает ровно там, где его вызвал EXE, и всегда получает физический input. | `gameplay/player/` |
| Удалённый P2 | P2 получает packet-backed input только внутри scoped native update; после него всегда восстанавливаются DirectInput и global XGamePad. | `input/` и `gameplay/player/` |
| Общая камера | P2 не меняет process-global camera state; P1 остаётся целью камеры после P2 update. | `gameplay/camera/` |
| Mooch | Остаётся единственным shared объектом. Сохраняется только подтверждённая transform/presentation синхронизация, без возвращения неподтверждённого laser replay. | `gameplay/fly/` |
| World triggers | Local P1 proximity отправляет reliable pulse. Peer временно переносит **свой P1** по FIFO и возвращает его; локальные trigger pointer адреса не уходят в сеть. Один pointer не переотправляется до reset уровня. | `world/trigger_pulse/` |
| World sync | Сохраняются сопоставление entity через process-neutral `TriggerKey`, spawn/snapshot, HP synchronization и current F9 diagnostic route. Legacy despawn не должен выдавать себя за native destruction. | `world/` |
| F8 без Steam | Сохраняется ручной standalone IPv4 parser; Steam rich presence вызывается только там, где Steam действительно доступен. | `network/` |
| Логирование | Логи остаются PID-раздельными и edge/event based; запрещено возвращать per-frame packet spam или flush после каждой строки. | `runtime/logging/` |

> `void*` остаётся допустимым только на границе с бинарником игры: в hook callback, trampoline и typed view. Внутри feature-кода адрес не должен передаваться как безымянный `void*` между несвязанными подсистемами.

## Модель retail ABI

Память игры не будет объявляться как выдуманный C++-класс с padding: layout известен лишь частично. Вместо этого вводятся **не владеющие typed views** над подтверждёнными полями. Например, `EntityView` знает только подтверждённые `handler`, `transform` и `trigger`; `ControllerView` — owner и mode; `TriggerView` — family, subtype, spawn definition и transform. Каждый accessor возвращает `bool`/`std::optional`-подобный результат и изолирует SEH внутри узкого memory boundary.

| Старый паттерн | Новый паттерн |
|---|---|
| `reinterpret_cast<BYTE*>(entity) + kEntityPositionOffset` | `retail::EntityView(entity).ReadTransform(out)` |
| `reinterpret_cast<void**>(kGPigEntityArray)[1]` | `retail::Players().Get(PlayerSlot::LocalP1)` |
| Разрозненные `__try` вокруг каждого поля | `retail::TryRead` / `retail::TryWrite` в одном boundary; feature logic работает с обычными typed values. |
| Дублирующиеся ручные E9 trampoline installer-ы | `patching::Detour` с описателем адреса, signature, relocate length и hook entry. |
| Пакетный `if`-каскад | `switch (PacketKind)` с отдельным валидатором exact wire size для каждого packet type. |

## Целевые модули

| Модуль | Назначение | Не имеет права знать |
|---|---|---|
| `retail/` | Адреса, typed views, native function signatures, registry walking, безопасные memory accessors. | Сеть и game rules co-op. |
| `patching/` | Expected-byte gate, IAT hook, raw E9 detour, trampoline lifetime и scoped global overrides. | Семантику P1/P2, packets и triggers. |
| `protocol/` | Упакованные wire structs, `PacketKind`, header init/validation и packet router. | Сырые игровые адреса. |
| `input/` | Local capture, remote snapshot store, action/edge/hold interpretation, keyboard and GamePad scopes. | World registry и spawn logic. |
| `gameplay/player/` | P1/P2 lifecycle, stock tick ordering, spawn snapshot, weapon sync, active-player publish guard. | Socket classes и packet queues. |
| `gameplay/camera/` | Shared camera save/restore, refresh и remote yaw override. | World entity linking. |
| `gameplay/fly/` | Подтверждённый shared Mooch ownership и transform presentation. | Remote laser replay. |
| `world/` | Trigger catalog, world entity links, spawn/snapshot presentation, HP synchronization, trigger pulse FIFO и F9 debug. | Direct socket implementation. |
| `network/` | GNS/Steam transport, role lifecycle, raw packet delivery and worker/game-thread handoff. | Память game entities. |

## Правила переписи

Перепись идёт по маленьким компилируемым срезам. Каждый срез обязан сохранить exact wire layout, calling convention и patch signatures. После изменения runtime-кода выполняется x86 build; установочная DLL заменяется только после того, как `GForce.exe` закрыт. Functional test выполняет пользователь.

Нельзя одновременно менять игровые правила и структуру кода. Неподтверждённые menu connect и remote Fly_Scan replay остаются удалёнными; только их подтверждённые retail constants могут жить в таблице адресов до отдельной задачи.

## Порядок миграции

1. Добавить foundation: typed value types, retail memory boundary, typed entity/controller/trigger views и reusable patch primitives.
2. Перенести пакетный ABI в `protocol/`, оставив `static_assert` размера всех пакетов, и заменить dispatch на `switch`.
3. Вынести remote input snapshot, query semantics и temporary keyboard/gamepad scopes из `CoopNetGame`.
4. Вынести controller routing, spawn snapshot, P2 update bracket, camera state и Mooch presentation из `Player2Module`.
5. Разделить `WorldSync` на catalog/link registry, transport queues, presentation/health и trigger pulse service.
6. Удалить старые wrappers только после замены всех call sites; затем провести runtime-регрессию и создать отдельные чистые коммиты.
