# coop_v2

Единственная актуальная реализация local co-op для проверенного retail
`GForce.exe`. Canonical-статус, структура и границы состояний — в
`IMPLEMENTATION.md`; адреса, ABI и confidence — в `../../re_cache/RE_CATALOG.md`.
Корневой README — только обзор проекта.

## Canonical docs for agents

Сначала читать `IMPLEMENTATION.md`: это компактная карта модулей, ownership и
границ состояний. Затем читать `../../re_cache/RE_CATALOG.md`: это единственный
каталог retail-регионов, адресов и статусов `approved` / `guess` / `not-tested`.
Этот README сохраняет подробности, исторические объяснения и рецепты; он не
должен использоваться как повод повторно угадывать уже задокументированный ABI.

Перед поиском новых функций прочитать в основном README раздел
«Как находились адреса и как продолжать реверс». Там зафиксированы правила для
IDA, `vftable`, x86 calling convention, rel32, field offsets и expected bytes.

## Состав

- `coop_dll.cpp` — только ABI-экспорты DLL и `DllMain`.
- `coop_application.h/.cpp` — класс верхнего уровня и порядок запуска модулей.
- `coop_runtime.h/.cpp` — `CoopRuntime`, `CoopConfig`, лог и проверка версии EXE;
  `MemoryPatch` отвечает за безопасную запись патчей в память.
- `gforce_constants.h` — адреса, offset, ID и проверочные байты retail EXE.
- `player2.h/.cpp` — slot/spawn/controller routing для P1, remote P2 и Mooch:
  GPig factory, сетевой и debug-спавн P2, оружие и input scopes.
- `shared_camera.h/.cpp` — единственный shared retail camera handler: native
  refresh, безопасный snapshot/restore aim state и local yaw. Он не знает про
  P2 или transport, поэтому будущий P3 не должен дублировать camera-код.
- `ServerClient/` — самостоятельный сетевой слой без зависимостей от X-Ray:
  standalone GNS, Steam P2P, клиент, сервер и `CSteamManager`.
- `menu_connect_hook.h/.cpp` — native-добавка к `XHudMenuMain`: после
  штатного `Credits` создаёт строку `Connect by IP` через retail factory и
  container. Нажатие ставит тот же запрос `CSteamManager`, что и `F8`; здесь
  нет GDI-оверлея, отдельного сокета или нового сетевого протокола.
- `window_hook.h/.cpp` — класс `WindowHook` и оконный режим через D3D9.
- `winmm_proxy.h/.cpp/.def` — класс `WinmmProxy`, ABI-прокси системного WinMM и
  загрузка `coop_dll.dll`. Кроме функций самой игры, прокси экспортирует mixer,
  wave-in и wave-out API, которые импортирует Steam `steamclient.dll`.
- `coop.ini` — устройство P2 и смещение спавна.
- `[window]` в `coop.ini` — экспериментальное перетягиваемое D3D9-окно 1280x720 для
  будущих тестов host/client. Focus/minimize pause постоянно обходится независимо от
  `coop.ini`: DLL сохраняет известный EXE-флаг активности после
  `WM_ACTIVATE`/`WM_KILLFOCUS` и перед D3D9 `Present`; `GetForegroundWindow` и
  `IsIconic` остаются дополнительным узким перехватом. Неактивное окно должно
  продолжать симуляцию и рендер, но его клавиатурный ввод не подменяется.
  Оконный режим отключается `experimental_windowed=0`.
- `GForceCoop.sln` — Visual Studio solution для ручной разработки: `coop_dll`,
  `winmm_proxy`, только целевая платформа Win32. `proxy_smoke` в текущем
  исходном дереве и solution отсутствует; упоминания о нём относятся к старому
  smoke-test эксперименту.
- `debug_actions.h/.cpp` — единственная точка временных F1–F7 и F9 действий. Она
  исполняется только после штатного тика P1 на game thread, а не из сетевого worker.
- `build.bat` — сборка x86 и подготовка runtime DLL.
- `dump_info.cpp` — вспомогательный исходник для анализа. Старый
  `proxy_smoke.cpp` не является частью текущего дерева и не собирается.
- Локальные дизассемблеры, извлечённые игровые данные, базы IDA и результаты сборки
  намеренно не входят в репозиторий. Исходники используют только проверенные
  константы из `gforce_constants.h`.

Состояние P2 и D3D9 теперь принадлежит соответствующим классам, а не глобальным
переменным. В `player2.cpp` свободным оставлен только x86 `__declspec(naked)`
trampoline и его C-bridge: MSVC запрещает `naked` для методов класса.

## Подключение по IP из главного меню

`MenuConnectHook` включается только после fingerprint-проверки exact retail EXE.
Он пропускает весь stock `XHudMenuMain::BuildMainMenu`, затем перехватывает ровно
тот `AddChild`, который добавляет `Credits`: сначала вызывает исходный `AddChild`,
после чего один раз строит следующую native-строку через найденные factory,
callback и container ABI.

Для подписи используется project-private `XAText` ID; hook resolver-а возвращает
`Connect by IP` только для него и передаёт все retail ID исходному resolver-у.
Callback не открывает свой Win32/GDI UI и не трогает transport: он только вызывает
`CSteamManager::RequestIpConnectionPrompt()`. Поэтому `F8` остаётся равнозначным
fallback-путём к тому же IP-диалогу.

Статически проверены entry bytes и ABI; видимость строки и клик в живом retail
меню пока **not-tested**. При несовпадении любого fingerprint hook не ставится,
меню остаётся stock, а `F8` доступен.

## Правило доступа к памяти retail EXE

Raw offsets являются необходимой частью ABI-границы с игрой, но не должны
расползаться по feature-коду. Допустимый слой для `BYTE* + offset`, `void*`,
SEH и native function calls — `retail/`, где адреса и частичные layout-ы
собраны в typed views. Игровые подсистемы должны пользоваться
`EntityView`, `EntityRegistryView`, `ControllerView`, `ModeView`, `MotorSystemView`, `MotorTaskView`,
`SpawnContextView`, `CameraHandlerView`, `CameraStateView`, `InputManagerView`,
`WeaponAmmoItemView`, `AmmoPoolEntryView`, `TriggerView`, `EntitySlotRepository`
и `NativeGameApi`/`TryRead`/`TryWrite`.

Для новых игровых ID и счётчиков используется `std::uint32_t` /
`std::int32_t`, для условий — обычный `bool`; custom-алиасы вроде `NBool` или
`NDword` не вводятся. Нулевой указатель в C++-коде — `nullptr`, не `NULL`.
`BYTE` остаётся только для точных byte-блоков патчей и DirectInput, а `DWORD` —
на границе Win32 (tick/API): там это часть чужого ABI, а не стиль модели игры.

В частности, P2 RDV/ABR factory больше не раскрывает в `player2.cpp` вручную
`handler + 0x4C0`, state table или task bytes: известная часть контракта живёт
в `retail/`. Это не делает ABR синхронизацию подтверждённой — для неё всё ещё
нужен live test двух процессов.

Проверка ABR стоит раньше `RemoteSnapshotInputScope` в P2 controller hook: если
в ABR находится P1 или P2, выполняется только native vehicle tick. Обычный
P2-путь не получает input, camera, weapon или root-transform ownership машины.

Обход live NPC/monster registry также больше не раскрывает узлы `BYTE*` в
`world_sync.cpp` или сети: `EntityRegistryView` читает только подтверждённые
`next`/`entity` и берёт `next` до callback-а. Лимит 512 — предохранитель от
повреждённого или циклического списка, а не предположение о числе сущностей.

То же относится к единственному shared camera handler: target, aim snapshot и
follow-turn вынесены в `CameraHandlerView`/`CameraStateView`. P2 spawn, weapon,
RDV task и controller state dispatch идут через `NativeGameApi`/`ControllerView`,
а не из feature-логики через адресные casts. Только hook/trampoline plumbing
остаётся низкоуровневой границей с проверенными expected bytes.

Cached aim ray (`origin` + `direction`) и native Fly control flag тоже имеют
свои узкие views: сетевой fire-handler не копирует `BYTE*` вручную, а Mooch
меняет только подтверждённый byte активного Fly state через `HandlerView`.

Например, выражение
`reinterpret_cast<BYTE*>(entity) + kEntityPositionOffset` допустимо внутри
реализации retail view, но в `player2.cpp`, `world_sync.cpp` или сетевом коде
считается долгом рефакторинга. Это не означает, что offsets нужно убрать:
нужно централизовать их, проверять доступ и возвращать typed values.

`EntitySlot` описывает шесть ячеек native-таблицы, а не готовый universal player
pool. IDA подтверждает cleanup для `0..5`, но два retail selector-а сознательно
перебирают только P1–P3 (`1..3`); `Mooch` занимает `4`, назначение `5` не
установлено. Поэтому P3 — отдельная будущая задача, а P4/P5 нельзя включать
расширением цикла: сначала нужны factory, destruction, input и camera ownership
для каждого нового controllable slot.

`EntitySlotBinding` лишь разово связывает slot с текущими entity/handler/controller
указателями. Он не даёт P3 права на tick, input или camera: такие права должны
появиться отдельно после live-проверки native lifecycle.

## P2: ledge/fall и outer DeathMode recovery — on-foot only, not live-tested

Проблема не лечится поддельным нажатием Shift/Space и не лечится отключением
физики. Retail `XGPigLedgeMode` сначала делает собственный contact query и
выбирает native state; отключить его означало бы потерять нормальные прыжки,
падение и collision.

До замены remote snapshot сетевой вход отбрасывает `NaN`/`Inf` в P1/Fly
transform, analog axes, aim ray и valid camera yaw: предыдущий конечный snapshot
остаётся рабочим. Повторная проверка перед on-foot transform write чинит уже
неконечный root P2 от свежей конечной цели. Это containment, не отключение physics;
логи `[net-input-reject]`, `[net-transform-recovery]` и non-finite
`[p2-recovery]` нужны для последующей проверки.

Обычный remote P2 получает плавную transform-коррекцию после своего native
controller tick. Затем retail scheduler `0x0043C9E0` ещё раз применяет motor
delta и может утащить P2 в старую ledge/fall позицию. Поэтому после штатного
scheduler pass co-op проверяет только on-foot P2:

- при расхождении `>= 2.5` units записывает свежий сетевой transform сразу;
- при остатке `>= 0.75` units делает один safety-snap не чаще раза в 2 секунды;
- при `>= 3.0` units и только вне Mooch ставит один native logical edge
  `0x1000000D` для следующего P2 input scope;
- оставляет native ledge/fall state machine и физику включёнными;
- полностью пропускает ABR/RDV: по packet mode, controller P2 и controller P1.

`0x1000000D` — подтверждённый retail pressed-edge route из второго namespace
`0x4008000A` к inner `Ledge → Inactive`; это **не** имя физической клавиши.
При сильном расхождении co-op не вызывает Ledge напрямую и не подделывает
Shift/Space: он выдаёт этот один logical edge только в следующем scoped P2 query.
`[p2-ledge-detach] queued ...` означает, что он поставлен, `served ...` — что
его увидел native query; визуальный выход с уступа всё ещё требует live-проверки.

Строка `[p2-recovery]` в `g_force\re_cache\runtime\gforce_coop.log` означает, что recovery действительно
сработал. До двухпроцессного прогона это только статически обоснованный маршрут,
а не заявленное исправление runtime-бага.

Отдельно существует outer `XGPigDeathMode`: это не Ledge substate. Раньше P2 в
нём целиком пропускал native update, чтобы не запускать локальный checkpoint
respawn, но из-за этого мог навсегда остаться в hide/death pose. Теперь, только
если P2 распознан по проверенной DeathMode vtable и peer прислал ненулевой
`Default` snapshot с sequence **новее snapshot на входе P2 в DeathMode**,
вызывается штатный dispatcher `SelectMode(Default)`. Он делает native Exit/Enter
и повторно настраивает P2 Default conflict mask. Не трогаются Ledge, collision,
падение, physics и ABR; stale packet либо peer не в `Default` оставляют
ожидаемую строку `[p2-death-guard]`. `[p2-death-recovery]`
— необходимое runtime-подтверждение; пока это **not-tested**.
Этот guard работает только при реальном remote peer: local P2 от F5 не ждёт
несуществующий сетевой snapshot и проходит свой stock DeathMode flow.

## Тестовый сценарий

1. Агент проверяет, что `GForce.exe` закрыт, и устанавливает сборку.
2. Пользователь запускает игру и проходит вступительную катсцену.
3. Пользователь нажимает `F5` один раз.
4. Проверяются: P2 стоит на полу, камера остаётся за P1, управление P2 работает.
5. Переключить фокус на второе окно или свернуть первое: оба процесса не должны
   замереть, терять simulation tick или переставать рисовать.
6. Анализируется только хвост `E:\G-Force\g_force\re_cache\runtime\gforce_coop.log`: при загрузке ожидается
   `[window] co-op focus-pause hook installed ...`. F7 для включения больше не нужен:
   он только повторно проверяет и печатает состояние уже постоянного bypass.

### Критичные проверки двух процессов

На обеих сторонах должна быть одна свежая DLL-ревизия. В этой ревизии
`WorldObjectEventPacket` имеет 52 байта и добавляет route-поле; смешивание со
старой DLL недопустимо (так же, как старые 332-byte версии `CoopInput`).

| Проверка | Действие | Подтверждение в логах / игре |
| --- | --- | --- |
| P2 ledge/fall | Увести удалённого P2 на уступ или в падение и довести рассинхрон до ~3 м. | Сначала `[p2-ledge-detach] queued logical action=0x1000000D ...`, затем `served logical action=...`; P2 должен штатно отпустить уступ. `[p2-recovery]` остаётся только у on-foot P2; fake Shift/Space и отключения physics нет. `[net-input-reject]` означает, что плохой snapshot сохранён не был. |
| P2 пропал / DeathMode | Дать удалённому P2 попасть в outer DeathMode, пока peer P1 уже снова в `Default`. | После одного более нового packet: `[p2-death-recovery] ... seq=N entry_seq=M`; P2 выходит из hide/death pose. Пока peer не в `Default`, `[p2-death-guard]` ожидаем и не означает crash. |
| Main-menu Connect by IP | В exact retail EXE открыть главное меню и нажать строку сразу после `Credits`. | Пока **not-tested**: сначала ожидаются `[menu-init] ... installed`, затем по одному `[menu] BuildMainMenu observed`, `Credits AddChild seam observed`, `native Connect by IP row added` и `Connect by IP label resolved`. Клик должен дать тот же IP-диалог, что `F8`, и `F8 request queued`. Если ABI не совпал, лог называет конкретный адрес, строки не будет, но `F8` должен остаться. |
| F1 local Mooch laser | В одном foreground-процессе, без клиента и без входа в Q, поставить Муху в кадр и один раз нажать F1. | Ожидается `[debug-F1] native Mooch dual-laser raw button fired target=(...)`. F1 подаёт тот же exact raw edge в shadow `Fly_Active::Update`, но временно ставит origin XGamePad в центр Мухи. После прохода возвращаются камера, включая Fly request/apply window `+0x91C..+0x9B7`, HUD/ownership и сеть не меняются; не должно быть рывка P1-камеры. При несовпадении профиля логируется direct visual fallback. Боевой live-result всё ещё **not-tested**. |
| Лазер и поворот Мухи | Local owner Мухи резко поворачивает её и нажимает штатную атаку, пока receiver остаётся за обычным P1 или в Q. | Owner: `[fly-laser] queued ... post_tick_fly_seq=N`; receiver применяет packet только после transform `N`, затем пишет **полный** `fly_rotation` без подмены одного компонента `camera_yaw` и запускает native `Fly_Active::Update` с лучом из центра Мухи. Режим, камера и HUD receiver не выбираются; direct pulse остаётся fallback. Визуальный/боевой live-result ещё **not-tested**. |
| P2/P3 scanner / HUD | Дать оружие со сканированием remote P2 (или существующему P3), затем проверить P1. | Открытая проблема. Экспериментальный P2/P3 scanner guard удалён: он не исправил общий зелёный HUD. Не считать scanner синхронизированным или изолированным, пока не найден и не проверен настоящий presentation route. |
| Смерть Мухи | Убить/respawn Mooch, пока peer подключён. | Owner публикует `local Fly_Deactivated ... zero-owner exit`; peer получает `remote ownership 1 -> 0`. Если receiver локально пытается войти в `Fly_Deactivated`, пока peer ещё присылает живую Муху, ожидается `[fly-lifecycle ... suppressed receiver Fly_Deactivated ...]`; после zero-owner packet этот guard больше не действует и stock respawn разрешён. Это пока **not-tested** вживую. |
| ABR/RDV | Войти в ABR через F6 при P2, затем подвигаться/повернуть машину. | Нет `[p2-recovery]` и нет обычной P2 camera/weapon/root-transform коррекции в ABR. Реальный vehicle turn остаётся отдельным **not-tested** маршрутом. |
| F2/F3/F4 | На sandbox-level нажать кнопки у подходящих trigger. | Только соответствующие `[debug-F2]`, `[debug-F3]`, `[debug-F4]`; F3 не должен придумывать event code, F4 работает лишь для verified ComputerBox. |
| Реальная активация trigger | Игроком или Мухой реально активировать объект/кнопку, в том числе в одиночной игре. | Сразу после native dispatcher ожидается `[trigger-activation] source=game|fly-local|fly-shadow ... family=... subtype=... definition=... pos=(...)`. Это факт вызова retail event, а не угадывание назначения; `result` — возврат dispatcher. |
| Дверь/панель через карту | На двух процессах с одной свежей DLL игроком или Мухой активировать уже наблюдённую зелёную/зелёно-красную дверь. | Source: один или несколько `[world-object] queued seq=... route=1|2` в порядке native chain. Peer: те же `peer received`, затем `native route result route=...` и `applied`; дверь действительно меняет состояние. Это уже **observed live** для проверенной двери. `direct forwarder skipped noncanonical` не маскировать — приложить строку. `0x41080010` сам по себе не «open door». |
| Вентиляция / динамические мобы | Сначала пройти `XTrigger_MO_Blender`/`Mouse` на host, затем свежим запуском — на client; второй процесс видит место. | На **обоих** окнах должна быть одна свежая DLL: `WorldSpawn` fixed 76 bytes. Client-run: `[world-entity-trigger] client request queued ... native dispatcher continues`, затем `[world-spawn-local] client allowed native spawn ... awaiting host id`, `[world-id] client candidate ... sig=...`; host создаёт canonical entity и посылает `WorldSpawn`; client получает `[world-spawn] ...` и связывает тот же local entity через `[world-link] host id=... match=signature`. Route 3 не запускает dispatcher повторно. Host-run: когда client не активировал trigger сам, остаётся fallback `client invoking native trigger ...` для создания его реплики. Для одного mob id не должно быть `client missing ...` или повторного local spawn. |
| Гонка spawn/kill NPC | На host вызвать mob-trigger и сразу убить созданного моба до того, как client увидит его. | Client может получить HP раньше entity: это допустимо. После `[world-link] client id=...` queued `WorldDamage` применяет последний host HP; убитый моб не остаётся живым. Client не должен отправлять HP обратно: `WorldDamage` — только host → client. Нужен свежий запуск уровня: старые уже созданные retail entity намеренно не удаляются сырым указателем. |
| F9 catalog | После того как уровень загрузился, нажать F9 и приложить блок от `[debug-F9] trigger catalog begin` до `end`. | Логируется каждый ещё живой зарегистрированный factory-trigger с точной position, distance от P1, family/subtype/definition, flags и последним реально увиденным event. `approved` означает только exact ComputerBox; `observed`, `guess` и `unknown` — кандидаты для исследования, а не разрешение их активировать. |

Никогда не возвращать автоматический спавн во время катсцены и не смешивать эту
сборку со старым `coop_test`.

## Visual Studio

Открыть `GForceCoop.sln` в Visual Studio 2022 и собирать `Release | Win32`.
Проект закреплён на `PlatformToolset v143`; результаты попадают в
`build\Release`; для Debug — в `build\Debug`. `build.bat` сначала ищет VS2022
и принимает явный путь через `GFORCE_VCVARSALL`; более новый установленный
компилятор — лишь локальный fallback.

Проект `coop_dll` использует локально установленный x86 SDK Steamworks и
GameNetworkingSockets в папке `SteamWorksSDK` (эта папка не публикуется). Путь
можно переопределить MSBuild-свойством `GnsSdkRoot`.
Runtime-путь задаётся свойством `GnsRuntimeRoot`. В линковку добавлены
`GameNetworkingSockets.lib` и `steam_api.lib`; runtime DLL автоматически копируются
в каталог сборки.

Для запуска к игре понадобятся совместимые Win32 runtime-файлы. По умолчанию
`build.bat` берёт их из `C:\GamesAndSource\SHOC\xr_build\bin\Win32\Release`,
но путь следует переопределять переменной `GFORCE_RUNTIME_ROOT`:
`GameNetworkingSockets.dll`, `steam_api.dll`, `libprotobuf.dll`,
`libcrypto-3.dll` и `abseil_dll.dll`.

`.lib` нужны только при сборке и уже находятся в `SteamWorksSDK`. В каталог игры
они не копируются. Runtime `.dll`, напротив, должны лежать рядом с `GForce.exe`.

`build.bat` вызывает `GForceCoop.sln` через MSBuild, поэтому `.vcxproj` —
единственный список production `.cpp`; результат лежит в `build\Release`.
`build.bat` не перезаписывает запущенную игру. Для явной установки двух собранных
мод-DLL в закрытую копию игры надо передать отдельный target; `coop.ini` при этом
сохраняется как пользовательская настройка:

```bat
set GFORCE_DEPLOY_ROOT=E:\G-Force
build.bat
```

Если `GFORCE_RUNTIME_ROOT` не задан, тот же `GFORCE_DEPLOY_ROOT` используется и
как источник пяти runtime DLL для staging-каталога `build`.

WinMM-прокси нельзя снова сокращать только до экспортов, которые напрямую импортирует
`GForce.exe`: после `SteamAPI_Init` загружается `steamclient.dll`, которому также
нужны `waveOutGetDevCapsW`, `waveOutMessage`, mixer API и wave-in API. Отсутствие
любого из этих экспортов останавливает игру ещё до входа в `main`.

## Временные debug-клавиши

Это диагностические действия над одним строго проверенным retail EXE. Они не являются
пользовательским UI и пишут результат в `g_force\re_cache\runtime\gforce_coop.log`.

Отдельно от клавиш каждый реально прошедший через retail `TriggerEventDispatcher`
event печатается сразу как `[trigger-activation]`. Это работает без соединения:
`source=fly-shadow` означает F1 или receiver-side native Fly shadow pass,
`fly-local` — штатный local Fly event, `game` — всё остальное. Запись сообщает
факт dispatcher-вызова и его точку, но не присваивает неизвестному trigger имя
или право на F4-активацию.

Кнопки и прочие map-objects не обязаны использовать этот dispatcher. Второй,
независимый byte-gated diagnostic hook печатает `[global-event]` для каждого
контекстного `0x41xxxxxx` события, прошедшего через подтверждённый global
seven-listener forwarder. В строке есть receiver/source, event, native result и
позиция P1 в момент действия. Это не координаты source-объекта и не доказательство,
что маршрут покрывает все кнопки: запись нужна, чтобы не спутать отдельный button
callback с proximity-trigger’ом.

Живой тест текущей заблокированной и разблокированной кнопок уже дал отрицательный
факт: при установленном `[netgame] global event diagnostic hook installed` обе не
дали ни `[trigger-activation]`, ни `[global-event]`. Это исключает только эти две
проверенные активации из двух известных downstream-маршрутов, но не обобщается на
все кнопки уровня. Поэтому локальные реальные edge-опросы пишутся раньше:
`[input-edge-local] kind=logical|raw action=... caller=...`. `caller` — точный
адрес retail-кода, запросившего нажатие; `action` — его logical/raw id. Одинаковый
consumer подавляется на одну секунду: некоторые retail raw `pressed` queries возвращают
`true` несколько кадров подряд и иначе засыпают лог одинаковыми строками. Лог ничего
не подменяет, не реплицирует и не называет объект, а служит следующей точкой для
статического разбора. Scoped F1 и receiver-side synthetic Fly pass намеренно не
попадают в этот лог.

Для поиска и синхронизации world-кнопок используются byte-gated relay `0x41E890`
и forwarder `0x46D6F0`. `[object-event]` содержит route, caller, RTTI (если
валидно), vtable и state flags. После успешного native шага каждый валидный
top-level map-template route отправляется надёжно и по порядку: relay — route 1,
независимый direct forwarder — route 2. Relay или forwarder, вложенный в другой
native object route, не дублируется. Direct route допускается только с verified
literal ECX receiver `0x00912AA8` (`mov ecx, imm32`, не разыменованный global);
иначе остаётся лог
`direct forwarder skipped noncanonical`. Peer ищет объект по vtable +
family/subtype + definition + transform-signature, вызывает соответствующий
original route под non-echo scope, а ambiguity/unresolved означает «ничего не
делать», не угадывать адрес.

`XTrigger_MO_Blender` (`family=0x1E000002`) и похожие NPC/monster dispatcher
события не идут через старый generic `WorldTriggerEvent`. Client отправляет request
(route 4) host-у **и продолжает собственный native dispatcher**: так retail создаёт
его local entity в том же месте, где он появился бы без сети. Host создаёт canonical
entity и назначает `world_id`; пришедший `WorldSpawn` связывает уже созданный client
entity по live transform-signature. Activation notice (route 3) **не вызывает
dispatcher на client** — он лишь регистрирует точный trigger и ждёт `WorldSpawn`.
Поэтому request не даёт вторую client-сущность от host. Старые generic entity packets
по-прежнему отбрасываются, чтобы не вернуть известный риск dispatcher replay.

После подключения client **не блокирует** свой local NPC/monster spawn: он записывается
кандидатом без id, после чего `WorldSpawn` host-а назначает ему canonical `world_id`.
Если trigger активировал только host, client использует старый armed fallback. Перед
проверкой signature transform читается заново: factory-time cached transform у
map-trigger может устареть к моменту вентиляционной цепочки. `definition` остается
диагностическим полем: retail может выдать разные значения в двух процессах, а
несколько `MO_Mouse` могут одновременно иметь одинаковые `definition=0` и
`occurrence=1`. При ненулевой signature нет fallback по этому слабому ключу:
ambiguity означает «не привязывать», а не связать случайного моба. Никакие retail
object pointers не удаляются вручную — task/AI/list ссылки движка остаются валидными.
Host остаётся authority для `world_id`, transform и HP; client применяет эту state как
реплику и откатывает свой локальный AI/collision HP вместо отправки его обратно.
`WorldDamage`, пришедший раньше link, остаётся pending и применяется после link,
так что ранняя смерть не превращается в вечного client-моба. Это собранный контракт,
который ещё требует живого теста указанной гонки.

`WorldSpawnPacket` имеет fixed размер 76 bytes. Текущая пара DLL обязана быть
одной ревизии: старый peer получит `[world-sync] rejected WorldSpawn wire size=...;
peer DLL mismatch`, а не тихо создаст неполную реплику.

| Клавиша | Действие | Граница безопасности |
| --- | --- | --- |
| `F1` | Локальная одноразовая проверка dual laser Мухи: берёт transform Мухи и текущий P1 aim, временно центрирует native XGamePad ray и запускает shadow `Fly_Active` с synthetic raw edge. | Не выбирает controller mode и не меняет ownership, scanner/HUD/camera; не использует сеть. Не требует входа в Q или клиента. При peer-owned Мухе это локальный native-проход для проверки receiver-side реакции; direct item pulse используется только при несовпадении runtime-профиля. |
| `F2` | Повторяет native spawn ближайшего зарегистрированного trigger с `kTriggerHasSpawnDefinition`. | Может создать ещё один объект, поэтому это только sandbox/debug. |
| `F3` | Повторяет последний реально наблюдённый native event ближайшего trigger. | Не угадывает event code. |
| `F4` | Посылает `ComputerBox` event `0x41080022` только ближайшему template с subtype `0x1F000095`, definition `43`. | Другие статически найденные `0x41xxxxxx` коды не являются подтверждёнными кнопками и намеренно не вызываются. |
| `F5` | Создаёт local P2 в этом процессе из сохранённого native P1 spawn context. | Не нужен второй процесс; допустимы только P1 Default/ABR. |
| `F6` | Сначала гарантирует `F5`, затем запрашивает native ABR для P1. | Локальная P2 ABR task всё ещё network-only experiment. |
| `F7` | Повторно проверяет и логирует уже постоянный co-op bypass focus/minimize pause. | При запуске hooks ставятся и в fullscreen, и в test-windowed; `Present` также принудительно пишет active state. `test_windowed` теперь меняет только presentation/style. Результат в фоне всё ещё **не проверен вживую**. См. `re_cache/RE_CATALOG.md`. |
| `F9` | Печатает read-only каталог всех ещё живых зарегистрированных trigger-точек. | Не вызывает trigger и не создаёт entity. `approved` — только exact ComputerBox; `observed` — уже виденный native event; `guess` — spawn-template; `unknown` — остальное. Координаты и identity наблюдены, но имя/назначение не угадываются. |

`Fly_Deactivated` (`0x61000075`) — локальный native-переход в `Fly_Respawn`, а не
сетевой флаг смерти. Его `Enter` разрушает локальное visual/task состояние, поэтому
receiver нельзя чинить одним сохранением `m_remote_input` после перехода. Guard на
`StateMachine_SelectState` (`0x004B7050`) перехватывает только запрос этого state
для exact Mooch controller **до** `Exit`/`Enter`, когда local owner отсутствует, а
последний принятый remote input всё ещё несёт
`fly_controlled=1` / ненулевой `fly_transform_sequence`. В этом единственном случае
он возвращает native-success и оставляет текущий presentation state. Он не касается
P2, ABR, другой машины состояний и настоящей смерти локального владельца.

`GPig Scan` — отдельный state `0x6100000C`, зарегистрированный
`XController_GPig`; его `Enter` создаёт native scan-task, а `Update`
продолжает shared scan/camera path. Из-за этого P2/P3 scanner может менять
process-global presentation P1. Попытка блокировать этот state через
`StateMachine_SelectState` и `Scan::Update` не исправила зелёный HUD и намеренно
не входит в эту ревизию. Scanner/HUD остаётся открытой задачей: нужны live-тест и
доказанный route, а не предположение по RTTI или одному mode-id.

Только процесс-владелец очищает **своё** `fly_controlled` и сразу повышает общий
`transform_sequence`; следующий input snapshot несёт `fly_controlled=0` /
`fly_transform_sequence=0`. После принятия именно упорядоченного raw-перехода
`fly_controlled: 1 -> 0` network ingress лишь ставит sequence в private pending
slot: он не вызывает retail-код с socket thread. На следующем tick exact Mooch
игровой поток один раз вызывает штатный `SelectMode(Fly_Deactivated)`, только если
ни local, ни remote owner уже не существуют. Поэтому ранее подавленный ложный
Deactivated не оставляет receiver навечно в старом presentation state. Более новый
remote claim или local hand-off очищает pending до dispatcher. Старый live-packet не
может оживить Муху после принятого выхода.

Для проверки смерти сравни оба `g_force\re_cache\runtime\gforce_coop.log`: у владельца должен быть
`[fly-lifecycle ... local Fly_Deactivated ... input_seq=N ... published ordered zero-owner exit]`,
а у второго процесса — `[fly-packet ... remote ownership 1 -> 0 input_seq=N fly_seq=0]`.
После неё ожидаются `[fly-lifecycle ... queued ordered remote zero-owner input_seq=N ...]`
и `[fly-lifecycle ... consumed ordered remote zero-owner input_seq=N; requested stock
Fly_Deactivated ...]` на receiver. `accepted=1` подтверждает только возврат
dispatcher; окончательный визуальный результат всё ещё требует живого прогона.
`[fly-lifecycle ... suppressed receiver Fly_Deactivated ...]` обязателен только если
на receiver действительно воспроизвёлся прежний локальный ложный transition; его
отсутствие само по себе не означает ошибку. После zero-owner строки suppression уже
не должен удерживать Respawn. Всё это остаётся **not-tested** до двухпроцессного
прогона.

## Historical research archive (noncanonical)

The material below records earlier experiments, hypotheses and traces. It can
help choose a new reverse-engineering question, but it is not a current
implementation contract. The source, `IMPLEMENTATION.md`, and
`../../re_cache/RE_CATALOG.md` win if it differs from them; in particular, do
not revive a route, packet layout or address merely because it appears below.

## Первый сетевой milestone

- Главное меню ничего не хостит и не запускает Steam relay/NAT traversal.
- После загрузки мира первый штатный тик P1 открывает standalone listener
  `0.0.0.0:44139`, Steam P2P listener на виртуальном порту `44140` и публикует
  rich presence. Поэтому загруженный save становится IP- и Steam-host, а меню — нет.
- В этот же момент запускается Steam relay/NAT traversal; до загрузки save Steam
  networking не прогревается.
- `F8` открывает поле IP-подключения. По умолчанию там `127.0.0.1:44139`;
  порт можно не писать — тогда используется `44139`. `Enter` подключает, `Esc`
  отменяет. Последний успешно начатый адрес остаётся значением по умолчанию до
  закрытия игры.
- Focus/minimize pause постоянно отключена для co-op процесса; F7 только выводит
  diagnostic повторной проверки. Полученный Steam rich-presence Join по-прежнему
  закрывает local listener-ы и фиксирует процесс в
  client-режиме, поэтому joiner не становится временным host при загрузке полученного
  `DATA4`.
- После соединения клиент и сервер обмениваются `GFCOOP_HELLO_v1` / 
  `GFCOOP_WELCOME_v1`; строки лога содержат PID процесса.
- После соединения P2 автоматически создаётся на host и client. Спавн выполняется
  на игровом потоке из штатного snapshot/context P1, а не из GNS worker. Автоспавн
  ждёт `Default`-режим P1, одну секунду после соединения и завершённый штатный тик P1,
  чтобы не создавать физический объект во время загрузки либо с устаревшим transform.
- Активное окно отправляет примерно 60 раз в секунду state-пакет: 256-битный snapshot
  Windows virtual-key, 67-bit state уже разрешённых игрой logical actions, transform P1
  и выбранный тип оружия. Пакет также содержит готовый `XGamePad` aim ray P1
  (`origin` + нормализованный `direction`), а не пытается восстановить его из mouse
  delta на другой машине. Logical actions снимаются после штатного mapping биндов P1,
  поэтому получатель не зависит от WASD и даже от совпадения пользовательских биндов.
  Получатель хранит последний полный snapshot до следующего пакета: удержание не является
  одно-кадровым событием. На P2 action и threshold queries возвращают эти сохранённые биты;
  оба DirectInput buffers `0xAA6580 + 4` и `+0x204` остаются резервом для прямого keyboard
  пути EXE. Второй buffer читает ветка hold/edge, включая GPig locomotion. Перед тиком P1
  исходные байты полностью восстанавливаются. Реальные analog axes `0/1` P1 (mouse-look)
  передаются P2 отдельно. Перед update P2 штатный camera refresh получает именно remote
  axes; перед последующим update P1 его camera context вновь восстанавливается.
  P2 становится process-global active GPig только после перевода в `Default`:
  inactive mode `0x5B7D60` иначе сам обновляет P2-камеру и обнуляет turn state
  общего camera handler, отбирая вращение у P1 до первого RMB P2.
  `GetAsyncKeyState` остаётся узким перехватом для прямых VK-проверок EXE. Для сетевого P2
  action profile временно равен `0` (keyboard), потому что profile `1` читает отдельный кэш.
- После штатного тика P2 transform удалённого P1 служит только мягкой целью коррекции
  позиции и root rotation — мотор не должен терять собственные run/turn/jump transitions.
  Это эксперимент client prediction/reconciliation и ещё требует теста на двух окнах.
  Выбранный weapon type передаётся штатному setter P2 только при смене типа. Ранняя запись
  `direction()` перед P2 update была откатана: она регрессировала подтверждённый remote fire.
- Подтверждено: action `fire` удалённого P1 доходит до штатного контроллера P2 и вызывает
  удалённый выстрел. Это не означает отдельную сетевую authority для пули, ammo или сюжета.
  Для NPC уже есть отдельный host-authority путь spawn/transform/HP; его полный
  жизненный цикл и gameplay-реакции всё ещё требуют live-теста.
- Локомоция P2 ещё не подтверждена как результат удалённого movement input. Отдельно
  остаётся torso/weapon aim: кости P2 пока могут брать направление камеры машины-
  получателя. Это другой путь от aim ray выстрела и его нельзя маскировать копированием
  transform/анимаций.

Проверка двух окон:

1. В host-окне загрузить save и дождаться штатного P1: лог должен показать
   `loaded world: IP=ready Steam=ready`.
2. Для LAN во втором окне нажать `F8`; для удалённого подключения выполнить
   обычный Steam Join из второго процесса.
3. P2 должен появиться в обоих процессах автоматически, без `F5` или `F6`.
4. При фокусе client его настроенный `fire` должен вызвать выстрел P2 на host; при фокусе
   host — наоборот. WASD не является требованием.
5. Отдельно проверить, что movement запускает locomotion P2, а torso/оружие P2 направлены
   по принятому look vector. На текущем шаге это ещё известные незакрытые дефекты.
6. В логе ожидаются по одному разу `peer connected`, `network P2 is ready` и
   `GetAsyncKeyState IAT hook installed` и при первом P2 update
   `DirectInput keyboard snapshot override active for P2`. Успешные отправка и чтение
   input-пакетов не логируются; остаются только handshake, смена состояния и ошибки
   транспорта.

## Почему aim P2 не слушается remote look vector

Полный список адресов — в разделе «Проверенные адреса» основного README, разбор по
шагам — в `build/gforce_regions.txt`, раздел `E`. Реализация ниже собрана, но ещё не
проверена пользователем в двух окнах.

- Направление выстрела не берётся из осей ввода. `0x488770` (`__thiscall` на `XGamePad`)
  делает unproject экранного луча по **process-global** матрице камеры `0x0099B410` и
  проекции `0x0099B1D0`, используя экранную позицию прицела из самого XGamePad
  (`+0x279C`/`+0x27A0` для pad, `+0x1FBC`/`+0x1FC0` для мыши, выбор по `+0x278C`).
- Результат кэшируется в экземпляре: origin `+0x2774..0x277C`, direction
  `+0x2780..0x2788`.
- Обработчик выстрела `0x5B8760` копирует этот кэш в команду выстрела как есть
  (`origin → cmd+0x04`, `direction → cmd+0x14`) и дополнительно пишет одно поле
  состояния fire в `[task + 4]`: `0` нет, `1` удержание, `2` отпускание, `3` нажатие.
  Сам input manager приходит в `0x5B8760` аргументом через `[esp+0x4C]`.
- Отсюда: перехват `0x488DC0` или `0x48B010` направление aim изменить не может.
  Подменять нужно два кэшированных vec3 либо позицию прицела вместе с матрицей камеры.
- DLL передаёт оба vec3 в `CoopInput` и перехватывает `0x5B8760`, а не весь
  `Default`-тик. Во время P2 она временно кладёт принятый ray в XGamePad, вызывает
  штатный обработчик выстрела и сразу возвращает старый ray. Поэтому команда выстрела
  получает удалённое направление, а P1 не видит P2 ray между тиками.
- Input manager — класс `XGamePad`: vtable `0x6FA3EC`, размер `0x27AC`, создаётся в XApp
  около `0x45800D` цепочкой `0x605919` (аллокатор) → `0x48B290` (конструктор) →
  `0x487F10` (регистрация). У P2 снова есть отдельный экземпляр, созданный только
  конструктором: `0x487F10` намеренно **не** вызывается, поскольку регистрация пишет pad
  в глобальный `0x9905CC` и ломает P1 camera/input bridge. Внутренний `Default` update
  (`0x5BEA00`) уже принимает pad аргументом, поэтому P2 получает приватный pad только в
  этот вызов; глобальный P1 pad не подменяется даже на один кадр. Матрица камеры всё ещё
  process-global и остаётся отдельной задачей.
- Управление костями находится в update аниматора `0x5340A0` и включается битовой
  маской `[this + 0x1F8]` (`EXHeadTracking` `0x8A9D64`, `EXSpineModifier` `0x8A9D4C`),
  а не в контроллере.
- Открытая гипотеза по хлысту P2, который ведёт себя как зажатый LMB: преамбула melee
  `0x5BC844..0x5BC8E1` читает второе пространство действий `0x4008xxxx`. Прямого
  packet-backed хука для всего этого namespace нет; шесть Fly-ID обрабатываются
  отдельно. Но конкретный `0x4008000A` из `0x5B92A0` статически переводится
  `0x489FD0` в обычный press-edge action `0x1000000D`, поэтому уже проходит через
  существующий packet action stream. Его физический bind и результат в уровне всё
  ещё не проверены.

## Почему тело не разворачивается в режиме прицела

Это отдельный дефект от костей aim: кости берут направление из луча XGamePad, а тело
поворачивает motor task.

Наблюдаемое поведение (со слов пользователя): целится только P1 — P1 не может
повернуться с зажатым ПКМ, пока в игре есть P2; как только оружие достают оба —
поворот с пушкой работает; никто не целится — тело крутится как обычно; при
зажатом ctrl P1 поворачиваться может, но P2 разворачивает в произвольные углы.
Тип оружия P2 значения не имеет.

После сборки от 23.08.2026 (сохранение/восстановление общего handler + хук
`0x488B00`) поведение стало другим: **тело P2 поворачивается туда, куда смотрит
камера, а тело P1 не поворачивается вообще**; pitch у P2 задаётся удалённо
корректно. Причина найдена (`0x5BCF30`, ниже) и устранена: следующая сборка
**проверена пользователем в игре — поворот тела работает у обоих игроков**.

Подтверждено чтением дизассемблера:

- Camera handler в процессе **один**. `0x515C80` — это ровно
  `[[0x915738+0x18]+0x144]`, а `0x915750` (= `0x915738+0x18`) — singleton уровня,
  который пишется только в `0x467CF7`/`0x468677`. Рядом есть индексные варианты
  `0x515CA0` (`+idx*4+0x2C`) и `0x515CC0` (`+idx*4+0x1C`), но `0x5BB1D0`
  использует именно беспараметрический. `0x5B03A0` может лишь переназначить цель
  слежения этого единственного handler, а не выдать P2 свой.
- Поэтому `[turn_task_P2 + 0x10]` получает yaw камеры **P1** — это и есть
  «P2 разворачивает в произвольные углы».

- Целевой yaw тела живёт в `[turn_task + 0x10]`. Turn task — `0x43BCC0(owner + 0x4C0, 1)`,
  тот же объект, что кэш `[owner + 0x4EC][ [0x9155F4] ]`. У него два писателя:
  `0x5B8D60` (движение) пишет `+0x08` скорость, `+0x0C` и `+0x20` yaw от осей, `+0x18`
  текущий yaw; `0x5BB1D0` (прицеливание) пишет `+0x10`.
- `0x5BB1D0` вызывается из `XControllerMode_GPig_Default::Update` `0x5BEB60` по адресу
  `0x5BEC24` как `(arg1 = pad, arg2 = owner + 0x4C0)`. Pad приходит **аргументом**,
  поэтому опрос действий в нём уже per-player — приватный pad P2 туда доходит.
- А camera handler — нет. В блоке `0x5BBA98..0x5BBBA4` вызывается `0x515C80` с
  `ecx = 0x915738`, то есть handler берётся из **процесс-глобального** camera manager
  без аргумента игрока, и `0x52AD20` на нём даёт yaw, который уходит в
  `[turn_task + 0x10]`.
- Ветка выбора: если `0x4B6F40(handler + 0x498) == 0x44110010` и `[state + 0x3C] >
  [0x8B7824]` (камера крутится сама), в `+0x10` идёт собственный yaw сущности из
  `0x534F50`; иначе — yaw камеры.
- Тот же блок **безусловно** пишет в общий handler `[+0x994]`,
  `[+0x998] = 0x534F50(entity)` и `[+0x99C]` (`0x5BB7E3`, `0x5BB821`, `0x5BB9F2`,
  `0x5BBB43`, `0x5BBB52`), а камера читает `+0x998` и `+0x99C` обратно в
  `0x5206FF`/`0x52070E`. Это тот же класс дефекта, что был у inactive-режима
  `0x5B7D60` с `[handler + 0x91C]`/`[+0x920]`.
- **`0x5BCF30` — per-frame camera update режима GPig_Default.** `__thiscall(mode)`,
  без стековых аргументов, вызывается ровно один раз — из `0x5BEBD3` внутри
  `XControllerMode_GPig_Default::Update 0x5BEB60`, **до** `0x5B92A0` (`0x5BEBF7`)
  и `0x5BB1D0` (`0x5BEC24`). Пролог `83 EC 20 D9 EE` — 5 байт. Заканчивается
  `0x5BD506: jmp 0x5B8210` с `ecx = [mode + 4]` (контроллер).
  Функция выбирает состояние **того же единственного** handler:
  `0x5BD2FC` пишет aim-id `0x4411000C` в `[handler + 0x9A0]`,
  `0x5BD38B`/`0x5BD3B6`/`0x5BD3F4` — follow-id `0x44110010`, плюс `+0x9A4`,
  `+0x9B3`, `+0x9B4`, `+0x973`; затем применяет через
  `0x5B03A0`/`0x5B0620`/`0x5B04F0` в `0x5BD4E8..0x5BD4F8` c контроллером
  тикающего игрока.
- Оба опроса, решающих aim vs follow (`0x5BD33C` — `0x488DC0(dev, 0x10000006,
  [0x718510], 1)`, `0x5BD36C` — `0x488DC0(dev, 0x10000011, [0x6F951C], 1)`),
  читают **процесс-глобальный XGamePad `[0x9905CC]`**, а не переданный pad.
  То есть в тике P2 эта функция переводила единственную камеру в follow-состояние
  и переназначала её на контроллер P2 — а `bl`-гейт `0x5BB1D0` на следующем кадре
  P1 видит follow с `[state + 0x3C] > [0x8B7824]` и выдаёт телу P1 его же yaw.
- `0x5B8210` (хвост `0x5BCF30`) — тоже `__thiscall(controller)`: читает действие
  `0x10000001` из `[0x9905CC]`, накапливает/затухает float в `[controller + 0x8C]`
  с потолком `[[0x912784] + 0xBF8]` и рассылает результат в `0x466650(owner +
  0x520, 0, flag, &[controller + 0x8C])`. Общий handler не трогает.

Что делает DLL сейчас (**проверено в игре: поворот тела работает у P1 и у P2**):

- `0x5BCF30` перехвачен и **полностью пропускается**, пока на потоке активен
  remote input: у P2 на этой машине своей камеры нет, и переназначать
  единственный handler на его контроллер нечем. Константа — `kGPigCameraUpdate`,
  ожидаемые байты — `kExpectedGPigCameraUpdate`.
- Из тика P2 убран `RefreshCameraForController(controller)`: это был второй
  вызов `0x5B03A0` с контроллером P2.
- `RestorePlayer1CameraTarget()` теперь вызывается только в тот кадр, где
  реально выполнился `SelectMode(controller, kDefaultModeId)` — единственный
  оставшийся путь, который может сдвинуть камеру на P2. Раньше он гонял
  `0x5B03A0` для P1 каждый кадр уже с физической мышью.
- `0x52AD20` перехвачен: во время remote input он возвращает yaw камеры
  **отправителя** из пакета (`CoopInput::camera_yaw` + `camera_yaw_valid`),
  иначе — оригинальное значение. Пролог — 9 байт (`8B C1` + `80 B8 B4 1A 00 00
  00`), relocate обязан быть 9, потому что `je` в `0x52AD29` читает флаги от
  перенесённого `cmp`. Это закрывает всех читателей сразу: `0x5BBB67`
  (поворот тела при прицеливании) и `0x5B8DB7` (yaw движения = `atan2(оси)` +
  yaw камеры).
- В обычном P1-кадре отправитель публикует yaw через
  `PublishLocalCameraYaw` сразу после собственного тика P1. Во время local
  Mooch P1-публикация пропускается: yaw снимается после native Fly tick, иначе
  в пакет ушёл бы старый угол Darwin вместо угла Мухи.
- На время тика P2 `[state + 0x3C]` follow-состояния принудительно обнуляется,
  чтобы `bl`-гейт `0x5BBA98` ушёл в ветку `0x5BBB67` (yaw камеры → удалённый
  yaw), а не в `0x5BBB89` (собственный yaw). Значение P1 возвращается прежним
  restore.
- `Player2Module` по-прежнему сохраняет перед штатным тиком P2 и восстанавливает
  сразу после `EndRemoteInput` три группы полей общего handler:
  `+0x8B4`/`+0x8B8` (aim-assist, `0x5BB581`/`0x5BB5CB`), `+0x988..+0x99C`
  (позиция цели, yaw, pitch) и `[state+0x3C]` follow-состояния `0x44110010`.
  Константы — `kCameraAimAssistOffset`, `kCameraAimYawStateOffset`,
  `kCameraStateMachineOffset`, `kCameraStateTurnOffset`.
- Перехвачен `0x488B00` — запрос удержания прицела в `0x5BB1D0` (`0x5BB321` для
  `0x10000006`, `0x5BB34D` для `0x10000011`). У него **другой порядок
  аргументов**, чем у `0x488DC0`: `(device, action, flags, threshold)` — flags
  идёт перед float. Пролог `56 8B 74 24 0C` — чистые 5 байт. Пока он был не
  перехвачен, ветка прицела P2 решалась локальной физической мышью, а не
  принятым snapshot.
- `CoopInput` сейчас имеет размер **336 байт**. В нём есть четыре native
  XGamePad-оси (`0..3`): P2 использует `0`/`1`, а локальный `Fly_Active` читает
  все четыре. В пакет также входят sequence/control-поля fly, полный transform
  Мухи (`position` и `rotation`), ABI-reserved `fly_debug_fire_sequence` и
  дополнительные состояния ввода. F1 также не использует это unreliable поле
  и не отправляет свой локальный debug-выстрел в сеть.
  Первый реальный remote-shot теперь идёт отдельным 44-byte fixed
  `FlyAbilityPacket` по reliable-каналу: пока это только dual laser Мухи; packet
  несёт world target, но не указатель, HUD, camera или controller state. В нём
  намеренно нет ABR motor heading: машина имеет отдельный native motor path,
  для которого remote-sync ещё не подтверждён. Поэтому на host и client
  обязательно должна стоять одинаковая DLL-ревизия.
- Input-снапшоты идут по Steam unreliable-каналу. Приёмник принимает только
  строго более новый ненулевой `transform_sequence` (с корректным обходом
  overflow) и сохраняет последний принятый state до следующего. Поэтому
  запоздалый пакет не может откатить P2 или полный transform Мухи, включая её
  `rotation`; ноль остаётся sentinel до первого опубликованного тика P1.

## Какие действия не зеркалятся на P2 (TAB и Q)

Ночное видение (`TAB`) и муха (`Q`) расходуют ресурс той машины, на которой
происходит действие, поэтому воспроизведение нажатия отправителя на приёмнике
тратило один и тот же пул дважды. Результат исключения этих двух действий из
зеркалирования **подтверждён пользователем в игре**: ни `TAB`, ни `Q` больше не
уходят на P2, а `TAB` (ночное/теплак) работает у каждого игрока отдельно и
управление не блокирует.

Как устроен bind (прочитано в `0x488A70`, ground truth):

- `0x488A70(device, action, flags)` для `action` в `[0x10000000, 0x10000043)`
  подменяет `action` на значение бинда `[XGamePad + 0x2668 + n*4]`.
- Далее ветвление по старшим битам значения бинда:
  `& 0x40000000` — маска во втором пространстве `[[0x99B6B0] + 4]`;
  знаковый бит — **DirectInput scan code в младшем байте**, читается как
  `byte [[0xAA6580] + 4 + code]`; иначе — маска кнопок устройства
  `[pad + device*8 + 4]`.
- Перед этим `0x488640(device, binding)` проверяет `[pad + 0x2628 + idx*4] &
  binding` и `!(0.0 >= [pad + 0x2648 + idx*4])`, где `device` 0/2/4/8/0x20
  отображается в `idx` 0..4.
- Дефолты биндов пишет `0x48A3A5`: сначала `rep stos` на 0x43 dword от `0x2668`,
  затем один из трёх профилей по `[pad + 0x278C]` — `0` (маски кнопок, единственный
  профиль, где вообще забинден выстрел `n=0x07`), `1` и `2` (наборы
  `0x80000000 | DIK`, без выстрела). Общий хвост `0x48A75E` пишет `n=0x32..0x39`.

Что делает DLL:

- `CoopNetGame::IsMirrorSuppressedAction` исключает **фиксированные семантические
  индексы**, а не физические клавиши: `0x09` (в живой таблице сидит на `DIK 0x10`
  = `Q`, это и есть вызов мухи) и `0x0E` — переключение GPig в
  `XControllerMode_GPig_Mooch`: `0x5BBC80` гейтится на
  `0x488CE0(pad, dev, 0x1000000E, 1)` в `0x5BBCAC` и вызывает
  `[vtable+0x20](0x61000065)` в `0x5BBD94`. Ребинд такое исключение не ломает,
  потому что клавиша в нём не участвует вообще.
- Исключение применяется на стороне ответа, а не захвата: `GetActiveRemoteAction`,
  `GetActiveRemoteHold` и обе edge-ветки (`0x488CE0`, `0x488C00`) отдают `false`.
  Инверсный запрос `0x488B70` из-за этого отдаёт «отпущено». Формат `CoopInput`
  **сейчас составляет 336 байт**; исключение TAB/Q не добавляет новых полей.
- Оба сырых пути тоже закрыты: `BuildRemoteScanCodeState` не выставляет `VK_TAB`
  и `Q` в подменяемые DirectInput-буферы `+0x04`/`+0x204`, а
  `HandleGetAsyncKeyState` возвращает для них `0`. Входы `0x4008xxxx` уже
  перехвачены, но packet-backed ответы существуют только для шести Fly-ID; для
  `TAB` физический DirectInput-фильтр остаётся единственным работающим механизмом.

Почему исключение по scan-коду пришлось выбросить: клавиши переназначаемы, и
привязка к `DIK` ломается первым же ребиндом. Семантический индекс статикой не
выводится — три профиля `0x48A3A5` отладочные, боевого профиля в EXE нет.
Декодированные таблицы (база `+0x2668`, `n = (disp - 0x2668)/4`):

- профиль `0` (`0x48A627`, маски геймпада): `n=0x01/0x02` `0x4000`, `0x06` `0x100000`,
  `0x09` `0x80000`, `0x0A` `0x40000`, `0x0D` `0x400000`, `0x11` `0x20000`,
  `0x12` `0x10000`, `0x15` `0x400000`, `0x2C` `0x44`, `0x2D` `0x88`
- профиль `1` (`0x48A509`): `n=0x00` `Z`, `0x01` `7`, `0x02..0x04` `A/S/D`,
  `0x06` `0xE0`, `0x09` `Q`, `0x0B` `\`, `0x0D` `0xE1`, `0x17` `0`, `0x1C` `3`,
  `0x1D` `6`, `0x1E` `[`, `0x1F` `U`, `0x3E` `7`, `0x41` `Y`, `0x42` `8`
- профиль `2` (`0x48A3EB`): `n=0x00` `Z`, `0x02..0x04` `K/L/;`, `0x06` `0xE6`,
  `0x09` `Q`, `0x0D` `0xE5`, `0x17` `0`, `0x1C` `Backspace`, `0x1D` `,`, `0x1E` `E`,
  `0x1F` `TAB`, `0x26/0x27` `Numpad2/1`, `0x2C/0x2D` `Numpad2/1`, `0x41` `O`
- общий хвост `0x48A75E`: `n=0x33` `V`, `0x34` `B`, `0x35` `'`, `0x37` `Numpad0`,
  `0x39` `Numpad2`, `0x3A` `Numpad1`, `0x3B` `0x40000020`, `0x3C` `0x40000040`

`A/S/D` против `K/L/;` — это раскладка на двоих за одной клавиатурой, выстрел там не
забинден, так что боевой профиль пишется не здесь; в `GForce.ini` биндов тоже нет.
Косвенное подтверждение, что `0x0D` и `0x0E` — пара особых тумблеров: в switch-геттере
`0x489FE0..0x48A250` все ветки отдают значение бинда `[pad + 0x2668 + n*4]`, и только
две отдают литеральный ID действия — `0x1000000E` в `0x48A019` и `0x1000000D` в
`0x48A0A6`.

Живая таблица (`profile=1`, два запуска, пады `02FBA900` / `02E8A900`) — это ground
truth, и она отличается от всего, что было видно статикой:

- `n=0x09` → `DIK 0x10` = **`Q`**, та самая клавиша мухи → исключён как
  `kFlySummonActionIndex`
- `n=0x0E` → `DIK 0x14` = `T` (не `TAB`!) — доказанный Mooch-тумблер → исключён как
  `kMoochActionIndex`
- `n=0x07` (выстрел) → `MASKNS 0x40000001`, то есть мышь, а не клавиатура: значит
  боевые бинды дописываются уже в рантайме, поверх профиля `1`
- **`DIK 0x0F` (`TAB`) не забинден ни на одно действие.** Ночное видение — вообще не
  логическое действие, оно читается прямо из DirectInput-буфера. Поэтому закрыть его
  может только пропуск `VK_TAB` в `BuildRemoteScanCodeState` и
  `HandleGetAsyncKeyState`; пинить там нечего и ребиндить тоже нечего.

На кадре hand-off `Q`/`T` P2 по-прежнему пропускается, чтобы EXE успел сменить
active entity. После подтверждённого remote ownership receiver не запускает обычный
remote controller tick Мухи и не передаёт ей камеру/HUD. После stock idle tick он
применяет полученный transform, а отдельный reliable laser event может запустить
короткий shadow `Fly_Active::Update` с private XGamePad и exact raw-button edge;
state-machine guard не даёт этому проходу сменить реальный режим. Локальный owner
публикует полный transform после native Fly tick; raw laser edge помечает именно
следующий post-tick `fly_transform_sequence`, поэтому receiver ждёт этот epoch до
shadow pass. Так remote Mooch остаётся presentation-only как controller, но её
laser item/world route получает native возможность обработать выстрел. Receiver
копирует полный native `fly_rotation` без splice одного компонента из
`camera_yaw`: это были разные representation и такая splice давала кривой pose/
направление луча.
Каждый shadow laser pass возвращает также изменяемое `Fly_Active` окно shared
camera `handler+0x91C..+0x9B7`; без него камера P1 дёргается к Мухе на один кадр.
Живой двухпроцессный тест поворота и реакций всё ещё обязателен.

Ещё не закрыто:

- `0x5B8D60` (поворот от движения) читает тот же глобальный yaw камеры на `0x5B8DB7`.
  Полный список читателей `0x52AD20`: `0x4F4E61`, `0x587E59`, `0x5B4E5A`, `0x5B521E`,
  `0x5B55C5`, `0x5B8DB7`, `0x5B92EA`, `0x5BBB67`, `0x5BDE9A`. Хук подменяет yaw
  только на scoped remote P2 tick; peer-owned Муха не получает remote controller tick.
- Ещё одна деталь `0x5B8D60`: `[turn_task + 0x20]` обновляется **только если** ось
  сдвинулась больше чем на `[0x6F645C]` либо выросла квадратичная длина. Если remote
  оси приходят неизменными, прежний целевой yaw сохраняется.
- `0x48AE10` / `0x4008000A` из `0x5B92A0` уже разобран до `0x1000000D` через
  `0x489FD0` и стандартный `0x488CE0` press-edge. Это не даёт имени физической
  клавиши. При расхождении P2 `>=3 м` один native logical edge ставится без
  fake key; проверить нужно `[p2-ledge-detach] queued`, затем `served` и выход
  P2 с уступа в двух процессах.
