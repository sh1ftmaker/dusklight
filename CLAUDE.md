# CLAUDE.md — Dusk Online (branch `dusk-online`)

Context for future sessions working on the **online multiplayer** layer of this
Dusklight fork. Read this first; it captures architecture, the hard-won puppet
details, build steps, and what's planned next.

> This repo is `Desktop/Dusk Online/Dusk Source/` — a fork (`origin` =
> `sh1ftmaker/dusklight`, `upstream` = `TwilitRealm/dusklight`). All online work
> lives on branch **`dusk-online`**; `main` is a clean mirror of `upstream/main`
> (v1.3.1-54). The parent folder also has `../PLAN.md` (broader roadmap, untracked
> by this repo) and `../test-two-players.bat` (local 2-player launcher).

Dusklight is a **native C/C++ reimplementation** of Twilight Princess (zeldaret/tp
decomp, rendered through Aurora). There is no ISO/DOL/PPC patching — every hook is
a named C++ symbol.

---

## Build & run (Windows)

This machine has a PATH entry containing `&` that breaks `cmd.exe`. **Always**
sanitize PATH before any vcvars/MSVC build:

```powershell
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notmatch '&' }) -join ';'
$root  = "C:\Users\shift\Desktop\Dusk Online\Dusk Source"
$build = "$root\build\windows-msvc-relwithdebinfo"
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && cmake -S `"$root`" -B `"$build`" && cmake --build `"$build`" --target dusklight"
```

- Re-run the `cmake -S … -B …` configure step only when sources are added/removed
  (the file list is `files.cmake`); otherwise just `--build`.
- Output: `build/windows-msvc-relwithdebinfo/dusklight.exe`. Close running
  instances before building or the linker can't overwrite the exe.
- `build/` is gitignored. The `clang`/IDE "file not found / unknown type"
  diagnostics on these files are **language-server noise** (missing include paths);
  the MSVC/Ninja build is the source of truth.

**Launch (env vars):** `DUSK_ONLINE_MODE=off|host|client`, `DUSK_ONLINE_PORT`
(7777), `DUSK_ONLINE_HOST` (client), `DUSK_ONLINE_NAME`,
`DUSK_ONLINE_PUPPET_OFFSET` (debug lateral offset so puppets don't overlap your
own Link). Local two-player test: `../test-two-players.bat` (disc baked in). The
`../online-input.ps1` + `../drive-to-gameplay.ps1` scripts drive instances into
gameplay via synthetic UDP input for autonomous testing.

---

## Architecture

**Host-authoritative state replication** (NOT input lockstep). The host is the
source of truth; clients render puppets and mirror world state. Determinism across
heterogeneous native CPUs makes lockstep impractical (the integer RNG state is
cross-platform-stable and used as a desync checksum, but float-driven sim branches
drift) — so authority sidesteps it.

**Transport + bus** (`src/dusk/online.cpp`, `include/dusk/online.h`):
- Host/client TCP over winsock on a dedicated IO thread.
- Framed messages `[op:u8][len:u32][bytes]`; a handler registry
  (`register_handler(op, fn)` / `send_message(op, data, len)`).
- N-player table (`kMaxPlayers = 16`), player identity (name/color), pose API.
- Core opcodes < 32 (`kOpHello`, `kOpWelcome`, `kOpPlayerState=16`, `kOpPose=17`);
  feature modules use opcodes ≥ `kFirstUserOpcode = 32`.

**Threading rule:** bus handlers run on the IO thread; they must lock their own
state. Anything touching engine/game state defers to a game-thread `frame_update`/
`poll` (see how snapshot/savesync queue then apply).

### Code layout
```
include/dusk/online.h            core API, constants, opcodes, PlayerState
include/dusk/online_*.h          per-module public headers
src/dusk/online.cpp              transport + bus + player table + identity
src/dusk/online/chat.cpp         text chat            (op 32)
src/dusk/online/desync.cpp       RNG-checksum desync detector (op 33)
src/dusk/online/snapshot.cpp     full dSv_save_c blob on join (op 34/35)
src/dusk/online/voice.cpp        voice chat           (op 36)
src/dusk/online/savesync.cpp     live flag/quest-item sync (op 37)
src/dusk/online/puppet.cpp       remote-player puppet rendering (no opcode)
src/dusk/online/ui.cpp           ImGui panels + per-frame module pump
src/dusk/test_input.cpp          synthetic UDP pad injector (DEV ONLY)
```

### Opcode registry (keep updated when adding modules)
| op | name | module |
|----|------|--------|
| 32 | kOpChat | chat |
| 33 | kOpChecksum | desync |
| 34/35 | kOpSnapshotReq/Snapshot | snapshot |
| 36 | kOpVoice | voice |
| 37 | kOpSaveSync | savesync |

---

## Engine hook points (edits to decompiled/original files)

These are the **only** edits to faithfully-decompiled game files — kept thin on
purpose (PR-mergeability). All real logic lives in `src/dusk/`.

- `src/m_Do/m_Do_main.cpp` — per-frame game-thread pump: `savesync::frame_update()`,
  `ui::frame_update()` (which runs snapshot poll + voice update), and
  `desync::submit_local()` after each sim tick. Also `online::init()`/`shutdown()`.
- `src/m_Do/m_Do_controller_pad.cpp` — publish local pad (`set_local_input`) and
  inject remote pad into a second slot.
- `src/m_Do/m_Do_graphic.cpp` — publish local transform (`set_local_transform`).
- `src/SSystem/SComponent/c_math.cpp/.h` — additive `cM_getRndState`/
  `cM_setRndState` accessors for the desync checksum + snapshot RNG restore.
- `src/d/actor/d_a_alink.cpp` — puppet hooks ONLY (~27 lines): the three Link joint
  callbacks call `puppet::body_joint_hook` / `head_joint_hook`; `draw()` calls
  `puppet::update_and_draw(this, createThunk, mpLinkModel, mpLinkHatModel,
  mpLinkHandModel, mpLinkFaceModel, checkWolf())`. The `createThunk` is a captureless
  lambda forwarding to `daAlink_c::initModelEnv` (so the module needs no access to
  the actor type and stays decoupled).
- `src/m_Do/m_Do_machine.cpp`, `src/dusk/ui/prelaunch.cpp`,
  `src/dusk/imgui/ImGuiConsole.cpp` — dev/test scaffolding (autoplay + console);
  **separate from the feature**, candidates to drop before upstreaming.

---

## The puppet system (`src/dusk/online/puppet.cpp`) — read before touching rendering

A remote player is rendered by cloning the LOCAL player's Link models and driving
them from the streamed pose. Hard-won facts:

1. **A human Link is 4 models** (`d_a_alink_wolf.inc`): body `al.bmd`
   (`mpLinkModel`), head+hair `al_head.bmd` (`mpLinkHatModel`), hands `al_hands.bmd`
   (`mpLinkHandModel`), face `al_face.bmd` (`mpLinkFaceModel`). Drawing only the
   body gives a headless/handless puppet — all four are created and drawn.
2. **Sub-models attach to body joints**: face + head → body joint 4 (head); hands →
   body base + joints 9 and 0xE. Because the body joints are pose-synced, the
   sub-models follow. Their OWN joints (hair sway, fingers) render at bind pose for
   now (not yet streamed).
3. **Warp material**: Link's head material uses the `WARP_TEX` (Midna teleport
   "black cube dissolve"). The puppet MUST be created via
   `daAlink_c::initModelEnv` (which runs `dRes_info_c::on/offWarpMaterial` + diff-flag
   `0x2000400`); a raw `mDoExt_J3DModel__create` renders the head as black blocks.
   That's why creation goes through the actor's thunk.
4. **Joint callbacks are on shared model data.** During a puppet `calc()`,
   `s_puppetCalc` is set so `body_joint_hook` injects the streamed world matrices
   (and `head_joint_hook` suppresses the local hair physics that would otherwise
   corrupt both the puppet and the local player). Pose injection happens INSIDE
   `calc()` so envelope skinning, normal/bump mtx, and the PC frame-interp snapshot
   are all consistent — never overwrite `getAnmMtx` after `calc()`.
5. **Shading**: replicate the real Link exactly — `settingTevStruct(isWolf?9:10)`
   then zero the additive TEV color registers (the effect of `initTevCustomColor`:
   `mLightInf.a`, `TevColor.rgb`, `TevKColor.r`/`.b`). A non-zero additive TevColor
   washes the model out (glowing limbs). Uses a LOCAL `dKy_tevstr_c`, not the
   actor's member.
6. **Shadow**: the puppet has no collision, so raycast the ground under its streamed
   pos (`dComIfG_Bgsp().GroundCross`) for the projection plane, then
   `dComIfGd_setShadow(body)` + `addRealShadow(head/hands/face)`. Guard with
   `groundH != -G_CM3D_F_INF`.

`kMaxJoints = 80` caps streamed body joints (human form uses ~35). If logs show
`puppet skeleton TRUNCATED`, raise it in `include/dusk/online.h`.

---

## World/inventory sync (`src/dusk/online/savesync.cpp`)

Host-authoritative live sync of the global quest state, scoped to avoid clobbering
per-player vitals. Synced regions only: `dSv_event_c::mEvent[256]` (story flags),
`dSv_player_get_item_c::mItemFlags` (main-quest "got" flags), `dSv_player_item_c`
(item slots). NOT health/rupees/counts. Host diffs each frame and broadcasts on
change (op 37); clients apply. `snapshot.cpp` still does a one-time FULL
`dSv_save_c` copy on join.

**Open design decision:** inventory is currently *shared* (client mirrors host).
Per-player inventory + client→host upstream are future work.

---

## Conventions & gotchas
- Git on this machine warns LF→CRLF on the dusk files; harmless.
- Inbound network data writes into save memory — every payload is magic+version
  checked before apply (snapshot/savesync). Keep that invariant for any new state.
- Logging via `DuskLog.info/warn` (`dusk/logging.h`). Search logs for `[online]`,
  `[savesync]`, `[snapshot]`.
- Wolf form: `mpLinkHatModel/HandModel/FaceModel` are NULL; the body model holds
  everything, so sub-model creation is skipped (guarded by NULL).

---

## Roadmap — what's planned next

**A. PR-readiness (to upstream cleanly into TwilitRealm/dusklight):**
1. ✅ Extract puppet rendering out of `d_a_alink.cpp` into `online/puppet.cpp`
   (decomp footprint 254→27 lines). DONE.
2. Gate everything behind a CMake `option(DUSK_ONLINE ...)` and wrap the thin
   decomp-file hooks in `#if DUSK_ONLINE`, so original files are byte-identical when
   off.
3. **Cross-platform transport** (blocker for a PC/Android/iOS repo): abstract the
   socket layer with platform backends like `src/dusk/http/` does
   (winhttp/android/url_session/no_backend) — BSD sockets for POSIX, winsock for
   Windows.
4. Split into a reviewable PR stack: (1) core transport+bus+`c_math` accessors,
   (2) engine publish hooks, (3) puppet rendering, (4) feature modules.
5. Drop the dev scaffolding (`test_input`, autoplay, prelaunch/console edits) from
   the feature PRs.
6. Move `DUSK_ONLINE_*` env vars into the project's config/settings system; add
   `docs/online.md`; open an RFC issue before the big PR.

**B. Features (gameplay):**
- **Enemy/boss sync** (`../PLAN.md` §3, the headline): host-authoritative actor
  replication — host simulates enemies, clients suppress enemy AI and drive them
  from streamed state (pos/angle/`health`@0x562/anim/action). Needs stable net IDs +
  spawn/despawn replication + damage/death attribution + boss phases. Same pattern
  as the player puppet, scaled to `fopAc_ac_c` actors.
- **Sub-model joint streaming**: stream hat/hand joint matrices so hair sway and
  finger articulation sync (currently bind pose).
- **Per-player inventory** + client→host save upstream (vs the current shared model).
- N>2 players need a host relay (transport is currently 2-peer).
```
