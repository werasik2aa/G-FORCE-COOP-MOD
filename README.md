# G-FORCE COOP MOD

An experimental, fan-made multiplayer co-op mod for the 2009 PC game
**Disney G-Force**. The original game is a third-person action-platformer based
on the film, where players control Darwin and use the companion fly Mooch for
special traversal and interaction sequences.

This project explores how that single-player game can be experienced together.
It adds an experimental second player and multiplayer transport support for
both Steam P2P and local/IP connections. The aim is a playable co-op experiment,
not an official replacement for the original game.

## Current scope

- Experimental two-player co-op.
- Steam P2P transport and local/IP networking.
- Separate player input, spawning, camera handling and gameplay-state sync.
- A WinMM proxy loader used to inject the mod into the original game.

The project is work in progress. Behaviour can change and some parts of the
original single-player game are still being researched.

## Implementation status

The current implementation is an injected x86 DLL for one verified retail
build of `GForce.exe`; it is not the game's source code or a portable engine.
The active co-op path is a networked local/IP or Steam-P2P experiment: each
process owns its local P1 and presents the peer as P2. Split-screen is not the
current target.

The current wire layout is defined by
`tools/coop_v2/ServerClient/MTypes.h`; `CoopInput` is currently 336 bytes and
includes Mooch's complete position/rotation transform, all four native Fly
analog axes, and the ordered F1 debug-fire sequence. Both peers must use the
same DLL revision. The Visual Studio solution currently contains `coop_dll`
and `winmm_proxy`. Older references to `proxy_smoke` and the old
split-screen/player-pool hypothesis are historical and are not build targets
or current implementation requirements.

For implementation work, read `tools/coop_v2/IMPLEMENTATION.md` first. It is
the compact map of modules, ownership boundaries and test status. Read
`re_cache/RE_CATALOG.md` second for every retail address, inferred name and
ABI: it marks each claim as `approved`, `guess` or `not-tested`, and labels
project-invented labels as `approx-name`. The longer `AGENTS.md`,
`tools/coop_v2/README.md` and `REFACTORING_*.md` files preserve routing,
test recipes or historical design evidence; they do not override those two
canonical documents.

Raw game-memory offsets are unavoidable at the retail ABI boundary, but new
feature code must access them through the typed views and `TryRead`/`TryWrite`
helpers in `tools/coop_v2/retail/`. Scattered `BYTE* + offset` expressions are
refactoring debt, not the intended coding style.

## Networking dependencies

The transport layer uses Valve's
[GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)
and Steam P2P through the Steamworks SDK. SDK headers, libraries and runtime
files are intentionally not committed: obtain them separately and place them
in the local build environment before compiling.

## Repository contents

- `tools/coop_v2/` — C++ source, Visual Studio solution and build scripts.
- `tools/coop_v2/ServerClient/` — networking and Steam P2P implementation.
- `docs/` — archive-format notes and the original co-op research plan. The
  original split-screen plan is retained as historical research.
- `AGENTS.md` — technical research notes and reverse-engineering context for
  contributors and AI agents.

The repository deliberately does **not** include the original game, extracted
assets, binaries, SDK binaries, IDE caches, raw string dumps or raw
disassembly output. You need a legitimate copy of Disney G-Force to use the
mod.

## AI-assisted experiment

This is a fun-made research and experimentation project. Development has been
assisted by Cloudy, ChatGPT and other AI tools for analysis, documentation and
implementation. Human testing and project decisions remain essential.

## Disclaimer

G-FORCE COOP MOD is an unofficial fan project and is not affiliated with,
endorsed by or supported by Disney, Eurocom, Steam or the original game's
rights holders. All game trademarks and assets belong to their respective
owners.
