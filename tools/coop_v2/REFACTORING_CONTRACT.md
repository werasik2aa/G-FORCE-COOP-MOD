# Контракт полной архитектурной переписи co-op DLL

> **Supplementary design reference, not the current source map.** For a change
> in this tree, `IMPLEMENTATION.md`, `../../re_cache/RE_CATALOG.md` and the
> checked source win. This document may describe a future module layout; it
> does not prove a retail ABI, enable P3, or authorise an unimplemented move.

Этот документ фиксирует **поведение, которое перепись обязана сохранить**, и границы новой архитектуры. Wire ABI точен внутри одной ревизии: намеренное расширение пакета требует обновить `static_assert`, валидатор и обе DLL одновременно. Обе стороны co-op должны оставаться совместимыми только с DLL той же ревизии.

## Неприкосновенные runtime-контракты

| Контракт | Сохранить при переписи | Граница ответственности |
|---|---|---|
| Retail gate | Жёсткая проверка fingerprint проверенного x86 `GForce.exe`, expected bytes перед каждым патчем и восстановление только собственных hooks. | `runtime/` и `retail/patching/` |
| Локальный P1 | Игрок, которым владеет данный процесс, тикает ровно там, где его вызвал EXE, и всегда получает физический input. | `gameplay/player/` |
| Удалённый P2 | P2 получает packet-backed input только внутри scoped native update; после него всегда восстанавливаются DirectInput и global XGamePad. Во время scope query читают один `m_active_remote_input`, а не свежий worker-пакет. Этот scope никогда не включает peer-owned Fly controller. | `input/` и `gameplay/player/` |
| Input snapshot | Input transport остаётся unreliable, но receiver заменяет state только строго более новым ненулевым `transform_sequence` по signed wrap-around правилу. Запоздалый snapshot не имеет права откатывать P2/Mooch transform, rotation или held input. | `protocol/` и `input/` |
| Общая камера | P2 не меняет process-global camera state; P1 остаётся целью камеры после P2 update. | `gameplay/camera/` |
| Mooch | Остаётся единственным shared объектом. Сохраняется transform/presentation синхронизация без remote Fly controller/input/camera takeover: receiver копирует полный native `position` и `rotation` owner-а, без splice `camera_yaw`. Только local owner тикает native Fly и публикует transform сразу после него. Каждая прямая запись через `EntityView::WriteTransform` затем очищает retail cache-valid byte `entity+0x7E`, поэтому следующий native reader пересобирает cached matrix `+0x88` из root `+0xC8/+0xE8`. Receiver очищает remote active-control flag и применяет snapshot перед laser shadow pass; он не выбирает Fly mode и не забирает HUD/camera. `Fly_Deactivated` имеет authority только у local owner: он публикует новый zero-owner snapshot. Dual laser — отдельный reliable event: receiver временно подставляет ray из центра локальной копии Мухи и запускает зарегистрированный `Fly_Active::Update` с exact raw-button edge. Shadow pass возвращает также полный Fly camera request/apply window `handler+0x91C..+0x9B7`, иначе один кадр уводит камеру P1 к Мухе. Direct item pulse допускается только как явно залогированный fallback. Carry остаётся future route. | `gameplay/fly/` |
| World triggers | Local P1 proximity отправляет reliable pulse. Peer временно переносит **свой P1** по FIFO и возвращает его; локальные trigger pointer адреса не уходят в сеть. Один pointer не переотправляется до reset уровня. | `world/trigger_pulse/` |
| World sync | Сохраняются сопоставление entity через process-neutral `TriggerKey`, spawn/snapshot, HP synchronization и read-only F9 trigger catalog. F9 не активирует trigger: он печатает live identity/точки, причём `approved` допустим только для exact ComputerBox. Legacy despawn не должен выдавать себя за native destruction. | `world/` |
| F8 без Steam | Сохраняется ручной standalone IPv4 parser; Steam rich presence вызывается только там, где Steam действительно доступен. | `network/` |
| Логирование | Логи остаются PID-раздельными и edge/event based; запрещено возвращать per-frame packet spam или flush после каждой строки. | `runtime/logging/` |

> `void*` остаётся допустимым только на границе с бинарником игры: в hook callback, trampoline и typed view. Внутри feature-кода адрес не должен передаваться как безымянный `void*` между несвязанными подсистемами.

Это же правило относится к raw memory access. Прямой `BYTE* + offset` не
является ошибкой сам по себе: retail layout известен только частично, поэтому
offsets нужны внутри `retail/`. Ошибкой считается использование такого доступа
в feature-модуле вместо typed view. При миграции разрешённый путь выглядит так:
`retail::TryRead/TryWrite` → typed view → обычный typed value в gameplay-коде.
Так offsets остаются централизованными и проверяемыми, а не исчезают ценой
новых угадываемых полных C++-классов.

## Модель retail ABI

Память игры не будет объявляться как выдуманный C++-класс с padding: layout известен лишь частично. Вместо этого вводятся **не владеющие typed views** над подтверждёнными полями. Например, `EntityView` знает только подтверждённые `handler`, `transform` и `trigger`; `ControllerView` — owner и mode; `TriggerView` — family, subtype, spawn definition и transform. Каждый accessor возвращает `bool`/`std::optional`-подобный результат и изолирует SEH внутри узкого memory boundary.

| Старый паттерн | Новый паттерн |
|---|---|
| `reinterpret_cast<BYTE*>(entity) + kEntityPositionOffset` | `retail::EntityView(entity).ReadTransform(out)` |
| `reinterpret_cast<void**>(kGPigEntityArray)[1]` | `retail::EntitySlotRepository().Get(EntitySlot::LocalP1, out)` |
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
| `gameplay/fly/` | Shared Mooch ownership/transform presentation и reliable двух-item laser pulse без HUD/camera takeover. | Magnetic carry target/replay. |
| `world/` | Trigger catalog, world entity links, spawn/snapshot presentation, HP synchronization, trigger pulse FIFO и F9 debug. | Direct socket implementation. |
| `network/` | GNS/Steam transport, role lifecycle, raw packet delivery and worker/game-thread handoff. | Память game entities. |

## Правила переписи

Перепись идёт по маленьким компилируемым срезам. Каждый срез обязан сохранить calling convention и patch signatures; wire layout меняется только намеренно, с обновлением exact-size validation и синхронным развёртыванием обеих сторон. После изменения runtime-кода выполняется x86 build; установочная DLL заменяется только после того, как `GForce.exe` закрыт. Functional test выполняет пользователь.

Нельзя одновременно менять игровые правила и структуру кода. Неподтверждённый
Fly projectile не считается готовой механикой: remote Fly_Scan replay удалён,
поскольку он превращал peer P1/камеру в Муху. Пока не найден native spawn/effect
route, нельзя подменять его чужим Fly controller tick или считать transform
репликацию доказательством синхронизации выстрела.

## Порядок миграции

1. Добавить foundation: typed value types, retail memory boundary, typed entity/controller/trigger views и reusable patch primitives.
2. Перенести пакетный ABI в `protocol/`, оставив `static_assert` размера всех пакетов, и заменить dispatch на `switch`.
3. Вынести remote input snapshot, query semantics и temporary keyboard/gamepad scopes из `CoopNetGame`.
4. Вынести controller routing, spawn snapshot, P2 update bracket, camera state и Mooch presentation из `Player2Module`.
5. Разделить `WorldSync` на catalog/link registry, transport queues, presentation/health и trigger pulse service.
6. Удалить старые wrappers только после замены всех call sites; затем провести runtime-регрессию и создать отдельные чистые коммиты.
