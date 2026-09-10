# Changelogs

## 3.6.10-pre — Fonts, Menu, Sniper/AI, and Foreign Model Fixes

### Game fonts and menu

- Fixed editor font loading and fallback behavior so game/editor fonts remain
  available across startup and level changes.
- Restored retail-style pause-menu font rendering and corrected menu layout,
  hit testing, modal input, and spinner alignment.
- Fixed menu save/autosave interactions so active property edits are committed
  before save operations.

### Sniper and AI rendering

- Fixed AI/sniper model pose rendering by supporting skinned replacement meshes,
  authored AI rotations, recursive AI task discovery, and related graph targets.
- Added safer animation fallback and per-AI playback controls so eligible AI do
  not remain static when a stand animation is absent.
- Preserved task-ID and AI-script resolution rules while adding regression
  coverage for nested HumanAI and animation paths.

### Models, textures, and editor stability

- Fixed cross-level model and texture resolution, including ATTA dependencies
  and stale archive-index behavior.
- Added exact source-level provenance for foreign model imports and rejected
  ambiguous automatic source selection.
- Added staged RES/DAT/MTP publication, differing-entry replacement, material
  slot validation, and rollback-safe import checks.
- Hardened OpenGL buffer-state cleanup, F11 framing, visual-integrity evidence,
  weather/loading behavior, and large-level lightmap handling.

### Verification and packaging

- Added focused model-source and spline regression tests plus disposable WMI
  Session 1 E2E hash/mutation checks.
- Release candidate binary ZIP includes `igi1ed.exe`, test binary, runtime DLLs,
  configuration, version metadata, and SHA-256 manifest.
- Known release gates and the complete future-fix procedure are recorded in
  [`docs/ISSUE_FIXES.md`](docs/ISSUE_FIXES.md).

## Unreleased — Foreign model source provenance

- Added exact source-level provenance for cross-level model imports.
- Rejects ambiguous automatic imports when same IDs have different mesh or
  texture bundles; explicit source selection is required.
- Stages and validates model RES, texture RES, DAT, and regenerated MTP output
  before atomic publication, replacing stale differing RES entries.
- Added regression coverage and a disposable WMI Session 1 E2E harness with
  hash-verified model/texture/mapping checks.
- Release remains gated by the full-suite failures and the required 30 MB
  runtime-memory check; see [`docs/ISSUE_FIXES.md`](docs/ISSUE_FIXES.md).

## Unreleased — Editor workflow E2E (`e2e-editor-harness`)

- **Fixed AMD OpenGL Driver Crash (`0x6ED690D2` / `0x444` Access Violation)**:
  - Resolved `igi1ed.exe - Application Error: The instruction at 0x000000006ED690D2 referenced memory at 0x0000000000000444. The memory could not be read.`
  - Root cause: in AMD `atioglxx.dll`, issuing fixed-function immediate-mode primitives (`glBegin(GL_TRIANGLES)`, `glBegin(GL_QUADS)`, `glVertex4f`, `glVertex2i`) while `GL_ARRAY_BUFFER` remained bound caused the driver to assume vertex attributes were in a buffer object rather than client memory, dereferencing a null pointer at offset `0x444`.
  - Resolution: systematically enforced unbinding of VAO (`glBindVertexArray(0)`), VBO (`glBindBuffer(GL_ARRAY_BUFFER, 0)`), EBO (`glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)`), and vertex attribute arrays (`glDisableVertexAttribArray(0..3)`) across all skinned mesh pipelines (`DrawSkinnedMesh`, `DrawAnimSkeleton`), geometry visualizers (`InitSphereMesh`, `AddCrossMesh`, `AddCubeMesh`, `InitSelectionBox`, `DrawSelectionBox`, `DrawAttachedMesh`), submesh drawing (`renderModel`, `BuildMeshFromGeometry`), 3D graph/HUD overlays, custom cursor, and loading progress overlays.
- **Added Full Object Category System (`AI`, `Buildings`, `RigidObjects`, `Vehicles`)**:
  - Implemented discrete object categorization across `e2e_live_test.cmd` (and `tests_run.cmd`), `Run-SmartTest.ps1`, and `Invoke-SmartNativeCaptureSession.ps1`.
  - Added dedicated CLI flags: `--ai` (`-ai`), `--building` (`--buildings`), `--rigid` (`--rigidobjects`), `--vehicle` (`--vehicles`), and `--category <name>`.
  - Added `--distinct-categories` sampling to automatically select 1 object from each distinct category present in the level, validating all 4 categories in a single interactive session.
  - Added `--distinct-types` sampling to guarantee every selected object has a unique type.
- **Resolved AI Model Rotation Schema & Transform Evidence**:
  - Supported single-axis yaw/gamma authored rotations (`HumanSoldier`, `HumanPlayer`) in candidate enumeration, preventing them from being falsely skipped.
  - Updated `SmartModelEvidence.ps1` to handle dynamic horizontal-only rotation overrides for AI types while preserving strict position, model ID, and multi-texture verification.
- **Interactive WMI Session 1 Automation & Process Hardening**:
  - Standardized on WMI `Win32_Process.Create` to reliably spawn visible editor windows in the user's interactive desktop (Session 1).
  - Replaced legacy external process termination with native PowerShell `Stop-Process -Force -ErrorAction SilentlyContinue`, eliminating `NativeCommandError` exceptions.
  - Standardized 4 GB Large Address Aware (VAS) flag on Release binaries (`0x0122`).
- **Comprehensive E2E Testing Documentation**:
  - Created [`docs/LIVE_E2E_TESTING.md`](docs/LIVE_E2E_TESTING.md) documenting full CLI usage, category specifications, level archetypes, dry-run plans, and evidence artifact inspection.

- Added a bounded three-object trial (Building L1, Car L2, EditRigidObj L3),
  ten saved-camera views each, and required DAT texture identity/load checks.
  All 30 fresh-log transform and texture checks passed; visual acceptance
  remains unverified. No renderer changes or all-level expansion.
- Added modular `Invoke-SmartVerificationMatrix.ps1` and resumable
  `Invoke-SmartLevelModelBatch.ps1`: select one or more levels or all 14,
  select all tasks or Buildings/RigidObjects/Vehicles/AI categories, limit
  object count, prepare-only, resume, and bounded retry. Level 1 preflight
  prepared 474/474 renderable instances; the full live run exposed an editor
  cleanup failure and is not claimed as complete.
- Hardened pilot recovery so a failed capture force-closes its own lingering
  editor process before restoring QED files. An elevated one-object retry
  passed all ten views, all ten model-evidence checks, and byte-verified QED
  restoration.
- Added configurable five-second OpenGL teardown/startup cooldowns between
  saved-camera cycles. The evidence checker now accepts both `[TEX] ResCache`
  and the deployed editor's `[TEX Native] Loaded textureId` records, while
  treating duplicate identical transforms as consistent evidence. WER recorded
  repeated `igi1ed.exe` access violations in AMD `atioglxx.dll`; those GPU
  driver crashes remain reported as verification failures, not model passes.
- Added DPI-aware full-client screenshots, fresh-log model transform and
  texture-assignment checks, and separately labelled terrain-hidden diagnostic
  captures. Verified ten watchtower views plus an underside diagnostic;
  individual material identity and automated visual acceptance remain pending.
- Added a pilot-only smart live testing generator and fail-closed evidence
  scaffold; corrected duplicate horizontal orbit mappings with a regression
  test. The first watchtower capture exposed an F11 graph-target mismatch and
  is rejected; all-object expansion remains gated on pilot acceptance.
- Added mesh-derived saved-camera pilot captures to bypass that F11 mismatch,
  with ten planned views and QED file backup/hash restoration. All ten captures
  completed; occlusion and live material/transform verification remain open.
- Added a visible Windows editor E2E runner with WMI launch, native input,
  process/log checks, screenshots, image metrics, and JSON/Markdown reports.
- Added mutation-gated file snapshots and hash-verified restoration for
  reversible workflow tests, plus validation of scenario inputs.
- Added corpus-derived workflow inventories and all-level editor-control,
  terrain-shortcut, placed-object orbit, and asset/model-resolution scenarios.
- Fixed model-ID scalar unrolling, bare-MEF LOD discovery, and a validation
  output leak that incorrectly entered the scenario list.
- Added draft graph metadata, node-edit assertions, and AI/animation workflow
  coverage across levels 1–14. Input dispatch alone is not playback evidence.
- Added a focused graph smoke manifest: one populated, named non-cutscene
  graph per level, chosen by node count; cutscene and unknown-area graphs are
  excluded. This checks opening/focusing graphs, not editing or save/reload.
- Validation: all 14 non-cutscene graph smoke runs completed on 2026-09-05,
  with per-scenario graph-load logs and screenshots. Level 1's F11 camera
  appeared beneath terrain; smoke success does not establish usable editing.
  Graph-node selection/edit/save/reload and animation/patrol live validation
  remain incomplete; the no-mistakes CLI is unavailable locally.

## Unreleased — MCP game-data integration

- Added the Win32 `igi_mcp.exe` headless MCP server with stateless
  `2026-07-28` JSON-RPC over stdio and authenticated loopback HTTP.
- Added project/level resources and game-data tools for task/object snapshots,
  persistent transform/model/type/parameter edits, AI and weapon fields,
  mission objectives, and graph/terrain/lightmap/asset inspection.
- Added revision guards, dry-run QSC/QVM validation, transactional backups,
  rollback, reparse-point checks, bounded requests, and redacted structured
  errors.
- Explicitly reject unsupported structural, graph, terrain, lightmap, script
  file, converter, and asset-packing writes instead of guessing or invoking
  arbitrary commands.
- Hardened MCP review boundaries: `level_validate` now matches its advertised
  level argument, weapon-loadout item schemas enforce their object shape,
  anonymous task IDs are shared between snapshots and mutations, unknown task
  layouts are rejected, type-specific object indices are reported, and
  structurally invalid JSON-RPC requests return a null-id error.

## 3.6.9-pre — Logging and Configuration Fixes

- Added the missing editable `editor/qed/qedconfig.qsc` source with `QEDLogs(TRUE)` and the correct `missions/location0` default path.
- Fixed the embedded config artifact's stale `location1` path.
- Made `igi1ed.log` fall back to `%LOCALAPPDATA%\Project IGI` or `%TEMP%` when the executable directory is not writable (for example, under `Program Files`).
- Flush the first log entry immediately so startup failures are captured before an early exit.
- Fixed `QEDSaveConfigOnExit` parsing so it cannot toggle logging.
- Added a regression test covering log-file creation and flushing.

## 3.6.8-pre — Fix "VirModel not available" for Cross-Family ATTA Dependencies

### 🐛 Bug Fixes
- **Fixed in-game `VirModel "235_01_1" not available` warning when adding foreign models.** `AddModelToLevelRes` (the pipeline that packs a foreign model's `.mef`/textures into a level's `.res`/`.dat`/`.mtp` when it's added via the editor) only ever scanned for other models sharing the same leading numeric prefix (e.g. adding `420_01_1` also swept in `420_04_1`). It had no mechanism to detect that a swept-in model's `.mef` could itself contain an `ATTA` sub-model reference into a *completely different* prefix family (`420_04_1` → `235_01_1`). That dependency was silently never packed, so the game reported the model as an unresolved virtual model at runtime even though it rendered fine in the editor.
- **Fixed ATTA sub-models from other levels not rendering in the editor viewport.** `LoadAttachmentsRecursive` looked up sub-model bytes via the in-memory `FindMeshData` ResCache *before* calling `FindModelFile`, but it's `FindModelFile`'s lazy cross-level `.res` indexing side effect that actually populates that cache for models living in another level's archive — so the retry never happened and the lookup failed even though the indexing had just succeeded.

### 🔧 Technical
- `source/renderer/renderer_objects_atta.cpp`:
  - `LoadAttachmentsRecursive`: both the cached (`existingIt`) and fresh ATTA-parsing code paths now retry `FindMeshData(modelId)` *after* `FindModelFile` runs, so its lazy cross-level index load is actually used.
  - `AddModelToLevelRes`: after the existing same-prefix family scan, a new worklist-based pass parses each family model's `.mef` for `ATTA` records and recursively resolves any sub-model referencing a different numeric prefix via `GetOrExtractMefTemp`, adding it to `familyModels` so it flows through the existing `.res`/`.dat`/`.mtp` packing steps automatically.

### ✅ Preserved
- Same-family (same-prefix) model resolution behavior unchanged.
- No changes to `.dat`/`.mtp` binary formats — new dependencies are packed using the exact existing `DAT_AddModel` / `igi1conv dat to-mtp` mechanics.

---

## 3.6.7-pre — Fog Intensity (0-200%) + Lightmap Crash Fixes

### ✨ New Feature
- **Fog Intensity spinner in Terrain Options**, `Fog Intensity  [-] [N%] [+]`, under the existing Fog checkbox in the pause menu's expanded "Terrain Options" section. Value is a **percentage, 0-200%, step 10, defaults to 200%** (maximum thickness) — persists to `qedconfig.qsc` as `FogIntensity` and scales fog distance live, no level reload needed.
- 100% = the level's original fog distance, 0% = 2x distance (thinnest), 200% = 0.05x distance (thickest, clamped so the fog shader's `far - near` never hits zero). Pure CPU-side distance scaling — no shader changes.

### 🐛 Bug Fixes
- **Fixed fog scale inverted at low percentages.** An earlier build's `100/percent` formula made the (then-)default 20% setting push fog *five times farther away* instead of thickening it, making fog effectively invisible. Replaced with a linear scale anchored at 100% = unchanged, and changed the shipped default to 200% (thickest) enabled by default.
- **Fixed pause-menu mouse hit-test offset.** `Input_OnMouse`'s local `menu_h` had drifted out of sync with the actual rendered menu height in `renderer_draw.cpp` after the Fog Intensity row was added, misaligning every click target below it by ~19px. Both now agree on `menu_h = 676`.
- **Fixed Lightmaps checkbox silently doing nothing on large levels.** `CalculateLightmapsForAllObjects()` had a hardcoded `targets.size() > 350` guard that skipped the *entire* bulk lightmap bake on any level with more than 350 Building/EditRigidObj/EditObj targets (e.g. level1 = 481) — so toggling "Lightmaps" ON in the pause menu did nothing at all on those levels. The guard's stated rationale (concurrent `CreateProcess` resource exhaustion) didn't match the code, which shells `igi1conv.exe` synchronously and closes all process/thread handles correctly. Removed the guard entirely so all objects are processed regardless of level size.
- **Fixed crash on very large levels (e.g. level 13, 1023 targets) during bulk lightmap bake.** `igi1ed.exe` is a 32-bit process; baking lightmaps for 1000+ objects (many with 70-100+ `.olm` textures each) could exhaust its virtual address space and crash. The existing per-object `bad_alloc` catch logged and broke out of the loop correctly, but by then the process was already so close to its address-space ceiling that an unrelated allocation elsewhere crashed it moments later. `CalculateLightmapsForAllObjects()` now checks `GlobalMemoryStatusEx().ullAvailVirtual` before each object and stops the bake gracefully (logged, no crash) with 300MB of headroom still free, instead of racing toward the first failure.

### 🔧 Technical
- `source/config.h` / `source/config.cpp`: `fogIntensity` field, default 200, clamped 0-200, loaded/saved as `FogIntensity` / `QEDFogIntensity`.
- `source/renderer/renderer.h` / `.cpp`: `Renderer` keeps the level-computed fog color/far as a base (`fog_base_color_`/`fog_base_far_`) and re-derives the effective UBO/objects/skydome fog via `ApplyFogIntensity()` whenever the base (level load) or intensity (pause-menu spinner) changes. New `SetFogIntensity(int)`.
- `source/app.h` / `source/app.cpp`: `App::SetFogIntensity` wrapper; applied from saved config on `Init()`.
- `source/renderer/renderer_draw.cpp`: Spinner row drawn under the Fog checkbox when Terrain Options is expanded, showing `N%`; menu height bumped to 676.
- `source/app_input_mouse.cpp`: Hit-test + `-`/`+` click handling for the new row (±10 per click, clamped 0-200), corrected `menu_h`.
- `source/app_editor.cpp`: Removed the `targets.size() > 350` bulk-skip guard; added a `GlobalMemoryStatusEx` virtual-address-space check inside the per-object bake loop; quieted expected benign lightmap-resolve misses (no bindings/no placement/no model) to INFO instead of WARNING.

### ✅ Preserved
- Existing Fog enable/disable checkbox and exact v3.6.0-pre fog code paths untouched.
- Helicopter rotor animation unchanged.

---

## 3.6.5-pre — Stability: Level 13 / Heavy Lightmap Crash Fix + Polish

### 🐛 Bug Fixes
- **Fixed "Error loading level 13: bad allocation" crash.** Heavy levels (e.g. level 13) with many small .olm lightmaps could exhaust the 32-bit address space during auto lightmap upload at LoadLevel when `enableLightmaps` was ON. 
  - Guarded the auto-bulk lightmap path (`LoadLevel`) with try/catch around `CalculateLightmapsForAllObjects()`: on `std::bad_alloc` we now disable lightmaps for the session and continue loading the level cleanly; other exceptions are logged as warnings.
  - Hardened `CalculateLightmapsForAllObjects()` to catch per-object allocation/resource failures and skip remaining objects instead of aborting the whole load.
- **Reduced log spam from expected lightmap misses.** "no bindings found", "no placement of", and "--model is required" failures from `lightmap resolve` are now logged at INFO (not ERR) because many objects legitimately have no baked lightmap.

### 🔧 Technical
- `source/app_level.cpp`: Wrapped auto-lightmap call in LoadLevel with bad_alloc + general exception guards.
- `source/app_editor.cpp`: Per-object try/catch in bulk calculate; quiet benign resolve misses.
- `source/utils_igi1conv.cpp`: Quiet benign lightmap-resolve igi1conv failures.

### ✨ Polish / UX
- **Pause-menu Fog toggle visible in Terrain Options.** Added a matching `[X] Fog` / `[ ] Fog` row inside the expanded "Terrain Options" section of the Escape (pause) menu. Clicking toggles the existing terrain fog flag (DRAW_TERRAIN_OPT_FOG), calls SetFogEnabled, and persists `Config::enableFog`. Matches the exact v3.6.0-pre fog implementation paths; no new fog state, intensity spinner, or shader changes.

### ✅ Preserved
- Helicopter main-rotor animation fix (pitch animates on fixed hub; yaw/roll locked).
- Exact v3.6.0-pre terrain-only fog implementation (no new UI or code).

### 🔧 Technical
- `source/renderer/renderer.h`: Added `terrain_draw_options_` to `task_tree_view_params_s` for pause rendering.
- `source/app.cpp`: Populate `terrain_draw_options_` in both paused and live task_tree_view construction.
- `source/renderer/renderer_draw.cpp`: Layout Fog row when `pause_terrain_expanded_`; bumped menu height.
- `source/app_input_mouse.cpp`: Hit-test + handler for the new Fog row inside expanded Terrain Options (calls ToggleTerrainDrawOption + SetFogEnabled + Config save).
- `source/app_level.cpp`: Wrapped AI animation auto-import (heavy parallel igi1conv work) in try/catch (bad_alloc / exception) so OOM/resource exhaustion on heavy levels does not abort the whole LoadLevel.

---

## 3.6.0-pre — Animations, Music, Lightmapping & More

### ✨ Major Highlights

- **🤖 AI Auto-Playing Animations** — Every eligible AI animates automatically at level load, all in parallel across worker threads. Resolution tries Stand Animation → AI-script actions → PatrolPath clips, with a bone-hierarchy default fallback so no AI stays static. Per-AI play/pause control via the property panel.

- **🔫 AI Held Weapons** — Soldiers now hold their assigned weapon in-hand, attached to the "right hand" bone so it moves with the animation. Cutscene AI are excluded. Live weapon-id refresh when changing weapon in the TaskTree.

- **🎵 Level Music + Escape-Menu Toggle** — Level-specific `.wav` auto-plays on load with seamless looping. New `[X] Music` checkbox in the pause menu starts/stops playback live. Pause-aware: music pauses/resumes with the editor.

- **💡 Full Lightmap Bake-Apply Pipeline** — Select an object, click **Calculate Light Mapping**, and the editor bakes a ray-traced lightmap (real sun/gamma) and applies it live. Features live modulation slider, dynamic sun recalculation, per-taskId binding, clean default look.

- **🎮 Escape-Menu Fog Controls** — Fog On/Off toggle + Fog Intensity spinner (0–200%) in Terrain Options, with visible effective-far distance effect.

- **🖥️ Developer Mode & Automated Screenshots** — `--developer-mode` flag enables headless command polling (`add-model`, `capture-model`, `set-fog`) for fully automated multi-level screenshot testing.

- **🔧 Cross-Level Foreign Model Extraction** — Fixed `.mef`/`.tex` extraction for models from other levels, cross-level `.res` fallback, and in-memory index rebuild — no more `429_02_1` fatal errors.

---

## 3.5.4-pre — Fog Controls & Foreign Model Extraction Fixes

### ✨ New Features
- **Fog Intensity control (0–200%).** A new spinner row in Terrain Options adjusts `QEDFogIntensity` live via `Renderer::SetFogIntensity()`, which writes `g_fog_intensity` into the terrain fog UBO. The shader uses it to scale `effective_far = g_fog_far / intensity`, making the fog band expand/contract visibly at typical view distances. Default: 10%.
- **Fog On/Off toggle.** A `[X] Fog` / `[ ] Fog` checkbox in Terrain Options enables/disables fog rendering immediately. Toggling from OFF at 0% intensity auto-jumps to 100% so the user sees an effect.
- **Default config saner defaults.** `enableFog=TRUE`, `fogIntensity=10`, `musicEnabled=TRUE`, `enableLightmaps=FALSE` — the editor now starts with fog visible at a mild level, music playing, and lightmaps off (matching the clean developer-look default).
- **Debug commands for headless testing.** `add-model model=<id>` loads and adds a foreign model; `set-fog val=<0..200>` sets intensity; `capture-model level=<n> model=<id>` captures 10 screenshots around the model. Commands read from `editor/tools/debug-command.txt` polled every 200ms.
- **Multi-level test harness.** `run_test_levels.ps1` runs levels 1, 3, 10, 8 in sequence, each loading a model, adding it as foreign, and capturing 10 screenshots.

### 🐛 Bug Fixes
- **Fixed: `Failed to load resource: 'LOCAL:textures/429_02_1.tex'` fatal on foreign model addition.** The texture-gathering loop only called `FindTextureFile` (disk scan), missing textures that exist only inside another level's `.res`. Added a `FindTextureData()` fallback that lazy-loads the host level's `.res` and extracts missing `.tex` siblings to `editor/textures/level<N>/` before packing them into the current level's `textures.res`.
- **Fixed: Foreign model `.mef` files extracted to `content/models/level<N>/` (a path the renderer never reads).** Changed loose-file copy destination from `content/` to `editor/` for both models and textures, matching the existing `editor/` convention from commit `7f08808`.
- **Fixed: In-memory `.res` indexes stale after `AddEntriesToRes`.** After packing new model/texture entries into the level's `.res`, the in-memory `res_model_indexes_` / `res_tex_indexes_` maps were not rebuilt, so the renderer couldn't find newly-added assets by name without a level reload. Now calls `RebuildIndex()` after every `AddEntriesToRes`.
- **Fixed: Fog intensity did not change visually.** The *deployed* shader at `assets/editor/shaders/{41,45}/terrain_fog.frag` was stale — it lacked the `g_fog_intensity` UBO field and always used full intensity. Updated both source and deployed shaders to read `g_fog_intensity` for effective-far math. Also fixed the `ubo_fog_s` struct layout by adding `intensity_` (with correct std140 padding for the vec4-aligned UBO).
- **Fixed: Family disk scan found ≤1 member for foreign models from other levels.** Added cross-level `.res` lazy-load: if the disk scan of the model's home level fails, load the home level's `.res` in-memory, find all `.mef` / `.tex` siblings in the same directory entry, and extract them to the editor content dir before packing into the current level.

### 🔧 Technical
- `source/renderer/renderer.cpp` / `renderer.h`: Added `SetFogIntensity()`, `WriteFogUBO()`, `ubo_fog_s::intensity_`.
- `source/renderer/renderer_objects_atta.cpp`: `AddModelToLevelRes` — cross-level `.res` lazy-load, family sibling extraction, texture `FindTextureData()` fallback, in-memory index rebuild.
- `source/renderer/renderer_draw.cpp` / `source/app_input_mouse.cpp`: Pause menu Terrain Options — Fog On/Off toggle + Fog Intensity spinner.
- `source/config.cpp` / `config.h`: `fogIntensity` (int, default 10), `enableFog`, `musicEnabled`, `enableLightmaps`.
- `source/debug_command_manager.cpp`: New `add-model`, `set-fog`, `capture-model` debug commands.
- `shaders/{32,41,45}/terrain_fog.frag` + `assets/editor/shaders/{41,45}/terrain_fog.frag`: `g_fog_intensity` UBO and effective-far math.
- `run_test_levels.ps1`: Multi-level automated test script.

---

## 3.5.3-pre — Developer Mode & Automated Screenshots

### ✨ New Features
- **Developer mode (`--developer-mode`).** A new CLI flag that enables a background thread polling `editor/tools/debug-command.txt` for commands. Commands are parsed and queued for execution on the main thread each frame, enabling automated testing without user input.
- **Automated model screenshot pipeline.** `capture-model level=<n> model=<id>` orbits a free camera around the model at 6 cardinal exterior angles (0°/60°/120°/180°/240°/300°) and 4 interior angles (0°/90°/180°/270°) at eye height, saving PNGs directly to `IGI1_ROOT/screenshots/`. The camera is positioned via live coordinate injection into the renderer's viewport floats.
- **Debug command set expanded:**
  - `goto level=<n>` — load a specific level (same as Escape-menu Select Level).
  - `add-model model=<id>` — add a foreign model from another level.
  - `capture-model level=<n> model=<id> [x=<n> y=<n> z=<n>]` — full screenshot sweep with optional manual position override.
  - `set-fog val=<0..200>` — set fog intensity percentage.
  - `wireframe` / `draw-parts` / `delete` — toggle rendering modes and delete selected object.

### 🔧 Technical
- `source/debug_command_manager.{h,cpp}`: New `DebugCommandManager` class with a watcher thread, command queue, and parser for `key=value` arguments. The main app calls `ProcessCommandQueue()` once per frame to dispatch queued commands with full editor state access.
- `source/app_editor.cpp`: `ProcessCommandQueue` dispatches each command type to the appropriate `App` method (e.g., `AddModelToLevelRes`, `Capture360Screenshots`, `SetFogIntensityPct`).
- `source/app_input.cpp`: `Capture360Screenshots` positions the free camera at computed orbit points, forces redraws, and saves framebuffer reads to PNG.
- `source/app.h`: Exposes `SetDeveloperMode(bool)` and `IsDeveloperMode()`; the polling thread is only spawned when `--developer-mode` is passed and disabled in release builds.
- Commands are idempotent — repeated `capture-model` calls produce separate numbered screenshots without corrupting level state.

---

## 3.5.2-pre — Lightmap Bake-Apply Pipeline

### ✨ New Features
- **Full lightmap bake-apply pipeline.** Select an object in the 3D viewport, click **Calculate Light Mapping** in its property panel, and the editor bakes a lightmap texture for it and applies it live. The pipeline: object-selection → UV2 extraction → sun-direction resolve → ray-traced occlusion → PNG encode → GL texture upload → per-taskId binding in the object fragment shader.
- **Real sun/gamma lighting.** Lightmap calculation respects the level's actual sun vector (read from `TerrainSun` in `objects.qvm`) and applies gamma correction, matching the warmth and direction of in-game lighting.
- **Live lightmap modulation.** A slider in the Escape-menu Terrain Options section modulates lightmap brightness in real-time — no re-bake required. The UBO `g_lightmap_modulation` uniform is updated instantly.
- **Fast load, clean default.** Levels load with a clean unlit look by default (`enableLightmaps=FALSE`). Toggling lightmaps on renders the baked atlas immediately. Lookup by taskId avoids stale/wrong textures from previous level loads.
- **Dynamic sun recalculation.** Changing the sun direction in the property panel triggers an automatic lightmap recalculation for all baked objects, keeping lighting consistent.
- **Lightmap button widget.** A new `LightmapButton` widget kind renders in the property panel when a bakeable object is selected, providing a "Calculate Light Mapping" click target with hover/active states matching the existing UI widget style.

### 🐛 Bug Fixes
- **Fixed: Lightmap overlay rendered over ATTA proxy UI elements.** The calculation overlay was competing with the existing ATTA proxy UI overlay. Added proper layering so ATTA proxy circles are drawn on top of lightmap previsualization.
- **Fixed: `EnsureLightmapsUnpacked` silently wrote 0 files.** The unpack function iterated the level's `.olm` resource but the loop guard was comparing against an uninitialized count. Fixed index bounds and added file-existence logging.
- **Fixed: `taskId=-1` collision cross-contaminated objects.** Objects without an explicit taskId (defaulting to -1) all shared the same lightmap slot, causing one object's bake to overwrite another's. Now non-unique taskIds fall back to a per-object hash key.
- **Fixed: ATTA proxy objects included in batch-calculate.** Proxies have no renderable mesh and their UV2 data is garbage. The batch-calculate loop now skips all objects whose modelId resolves to an ATTA proxy (`modelType == 2`).
- **Fixed: Indoor objects received outdoor-bright lighting.** Objects inside buildings now get a dimmer indoor fallback light value instead of the full outdoor sun, preventing blown-out interiors.
- **Fixed: Lightmap warmth mismatch with game.** The overbright rendering pass was desaturating colors. Adjusted the tone-mapping curve to match the game's warmer, more saturated lightmap appearance.
- **Fixed: Light and sky colors not reflected in lightmap.** The `TerrainAmbientLight` and `TerrainSunLight` colors from `objects.qvm` were parsed but not passed to the lightmap calculator. Now both are forwarded to `ComputeLightmapForObject` and affect the baked result.

### 🔧 Technical
- `source/app_editor.cpp`: `CalculateLightmapForSelectedObject` orchestrates the full pipeline — UV2 extraction, sun direction resolve, `igi1conv.exe` invocation for ray-traced occlusion, result readback, transfer to the renderer. Uses `std::async` for the heavy compute step so the UI stays responsive.
- `source/renderer/renderer_objects.cpp`: `BindObjectLightmap(taskId)` binds the per-taskId GL texture for the active object shader. `GetLightmapTexture(taskId)` lazily creates/returns the atlas entry.
- `source/renderer/renderer_draw.cpp`: The Escape-menu Terrain Options row for lightmap modulation. `SetLightmapModulation(value)` writes the UBO uniform.
- `source/renderer/graph_writer.cpp`: Extended to read/write `TerrainAmbientLight` and `TerrainSunLight` from `objects.qvm`.
- `source/renderer/olm_texture.cpp`: `.olm` → OpenGL texture loader. Handles the padded 32-bit-per-pixel format the game uses for lightmap atlases.
- `source/level/level_objects.cpp`: Exposes the current level's `objects.qsc` path for UV2 extraction and sun vector readback.
- `shaders/{41,45}/object.frag`: New `g_lightmap_modulation` UBO uniform, `g_lightmap_enabled` toggle, and the per-taskId sampler binding `g_lightmap_sampler`.

---

## 3.5.1-pre — AI Held Weapons & Escape-Menu Music Toggle

### ✨ New Features
- **AI soldiers now hold their assigned weapon in-hand.** Each `HumanSoldier`-family task in `objects.qvm` carries a `Gun*` weapon child whose `WEAPON_ID_*` enum is resolved to the same render model used by `GunPickup`/`AmmoPickup` (via the existing `ResolvePickupModelId`). The weapon mesh is attached to the soldier's **"right hand"** bone (located by name in the parsed MEF bone list) so it moves with the hand as the animation plays. Static/paused AI hold the weapon at the rest-pose hand position.
  - **Cutscene / single-node AI are excluded.** If the soldier's `HumanAI` child references an AIGraph with ≤1 node, or whose name contains "cutscene", no weapon is attached — these are scripted cutscene actors, not combat AI. (Verified: level-1 task 1666 "Jones" with graph 2 / 1 node is correctly skipped.)
  - **Live weapon-id refresh.** Changing a soldier's (or its weapon child's) Weapon ID in the TaskTree property panel re-resolves and updates the held weapon immediately in the editor — no reload required (`LevelObjects::ResolveSoldierWeapon`).
  - Populates the previously-unused `primaryWeapon` / `graphId` / `graphName` AI tooltip fields as a side effect.
- **Escape-menu Music on/off toggle.** A new `[X] Music` / `[ ] Music` checkbox row in the pause menu (between Model Search and Terrain Options) starts/stops level music live via `App::ToggleMusic()`; the checkbox reflects current playback state.

### 🐛 Bug Fixes
- **Fixed: attached weapons were invisible.** `DrawAttachedMesh` used the object shader but never bound the shared Matrices UBO, so the vertex shader had no `u_mvp` (Proj·View·GlobalScale) and the weapon rendered off-screen. Now binds the UBO like the main scene draw.
- **Weapon orientation correction.** Weapon meshes are authored barrel-along-+Y; attached at the hand they came out vertical/upside-down. A fixed local correction (90° tilt to horizontal, horizontal flip to correct aim, then a 180° roll about the computed barrel axis to set it right-side-up) orients them like a held rifle without affecting position or aim.

### 🔧 Technical
- **Parser** (`level_objects.cpp`): new `HumanAI` (graph id), `Gun*` weapon-child (weapon enum), and extended `AIGraph` (node count) capture; new `LevelObject` fields `weaponModelId`, `graphNodeCount`, `aiGraphTaskId`, `weaponEnumId`.
- **`Renderer_Objects::DrawAttachedMesh`** + `Renderer` forwarders, and `GetOrLoadSkinGeometry` exposed for hand-bone lookup.
- Per-model "right hand" bone index cached in `App::handBoneIndexCache_` to avoid per-frame name scans.

## 3.5.0-pre — Auto-Playing AI Animations in Parallel & Level Music

### ✨ New Features
- **Every AI animates automatically at level load — all in parallel.** The animation system now resolves every eligible AI's animation simultaneously across worker threads instead of only auto-playing those whose `standAnimation` field matches a real clip. For each AI, resolution tries Stand Animation → AI-script `AIAction_PlayAnimation` ids → PatrolPath predefined animations (in that order); the first match plays. If no animation is referenced, the AI falls back to the bone hierarchy's default clip (lowest `animId`), so no AI stays static by accident. Resolving can be heavy (spawning `igi1conv.exe` per AI to decompile `.qvm` scripts), so phase 1 imports all bone hierarchies sequentially, phase 2 spawns per-AI resolution across `std::async` worker threads, and phase 3 merges results back on the main thread — preventing data races while keeping the level-load latency low (~2s for 55 AI).
- **Auto-played animations loop continuously.** Non-looping clips (whose BEF `tp_flag = 0`) now force-loop when auto-played, so every AI keeps animating instead of freezing after one cycle. This is editor-only behaviour (`AnimPlayback::forceLoop` flag); manually-toggled animations respect their clip's own loop flag.
- **Parallel skinned-mesh rendering.** The static mesh skip list changed from a single object index to an `unordered_set<int>`, allowing all auto-playing AI to have their animated mesh drawn in parallel each frame instead of just one. Their individual animation state (playing/paused) is respected independently.
- **Per-AI animation on/off control.** Each auto-played AI can be individually paused/resumed via the property panel's Animation Control section, without affecting other AI — clicking the animation toggle button for one AI pauses only that AI; others keep looping. Toggling off reverts that AI to its static mesh.
- **Level music playback with auto-loop.** `PlayLevelMusic` loads the level-specific `.wav` (or game default `game_music.wav` if not overridden in `qedconfig.qsc`), plays it via Windows `mciSendString`, and `CheckMusicLoop` restarts it when it finishes (MCI "repeat" is unreliable for waveaudio, so manual restart ensures looping). Music respects pause mode — it pauses when the editor pauses and resumes when the user un-pauses.

### 🐛 Bug Fixes
- **Fixed: Only 4 of 29 AI were auto-playing on level 1.** Root cause: most AI don't have a `standAnimation` value; their animation comes from their AI script's `AIAction_PlayAnimation` or PatrolPath commands instead. The old auto-play only checked `standAnimation`, so it missed 23 AI. Now uses `GetOrComputeAnimationIds` to try all three sources in priority order, and falls back to the default clip if none resolve.
- **Fixed: Auto-played non-looping clips froze after one cycle.** Clips were clamping `currentTimeMs` and setting `playing = false` at duration end. Added `AnimPlayback::forceLoop` so auto-play ignores the clip's `tp_flag` and keeps cycling forever.
- **Fixed: Data race in `MakeTempPath` RNG.** The `static std::mt19937_64` was shared across concurrent worker threads spawned during animation resolution. Changed to `thread_local` so each thread has its own RNG state without synchronization.

### 🔧 Technical
- **Parallel animation resolution at level load (Phase 1-3):**
  - **Phase 1 (main thread, sequential):** Import every distinct bone hierarchy's animation set before any worker thread touches the registry (prevents concurrent writes to the same cache map).
  - **Phase 2 (worker threads, parallel):** Each AI gets a `std::async` task that computes its animation ids, tries each, and returns the first matching clip (read-only against registry and level objects).
  - **Phase 3 (main thread, sequential):** Merge results into `animPlaybacks_` and `animIdsCache_` — no concurrent writers, just sequential map inserts.
- **New API:** `AnimationRegistry::GetDefaultClip(boneHierarchy)` returns the lowest-`animId` clip for fallback. `App::ComputeAnimationIdsForObject(objIndex)` is the thread-safe cache-free helper used by parallel resolution.

### 🎵 Music Integration
- **Level music auto-plays on load** (via new `PlayLevelMusic(level_no)` call in `LoadLevel`).
- **Per-level music override** in `qedconfig.qsc`: `QEDLevelMusic(level_no, "custom_music.wav")` sets a custom `.wav` for that level; unset levels use `game_music.wav`.
- **Auto-loop via `CheckMusicLoop()`** called every frame: detects when the track finishes and restarts it (MCI workaround).
- **Pause-aware:** `StopLevelMusic()` called on pause; `PlayLevelMusic()` resumes on un-pause.

## 3.4.1-pre — Live Graph Sync, Exact-ID Find & AI Script Notepad

### 🐛 Bug Fixes
- **Fixed: AIGraph task move did not live-update the F7 graph overlay.** When the user moved the AIGraph task via the TaskTree property panel, the 2D position pad / Z slider, or Undo / Redo, the AIGraph task's world pos was updated but the renderer's `graph_overlay_offset_` stayed at the F7-press-time value — so the 3D nodes and edges drifted away from the task. Added `Renderer::SetGraphOverlayOffset` + `App::SyncGraphOverlayOffsetFromAIGraph`; the helper is a no-op unless the overlay is visible AND the moving task's `taskId` matches the overlay's `taskId`, so moving other objects does nothing and toggling F7 off suppresses the work. Wired into `CommitPropTextEdit`, `ApplyPropPositionDrag`, `Undo`, `Redo`, and `SaveAndReloadObjects`.
- **Fixed: `TaskFindByTaskID` matched "7" against "73", "700", every "7" inside any id.** Task IDs are pixel-perfect identifiers, so the search was changed from `find()` substring to exact case-insensitive equality in both the live-while-typing path (`app_input_keyboard.cpp`) and the `TaskFindAgain` / Ctrl+Shift+F next-match path (`app_input.cpp`). Other find modes (`TextInTask`, `ByNote`, `TaskNameTypeId`) keep their substring behaviour.
- **Fixed: AI Script editor's Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+A / Ctrl+Z / Ctrl+Y were no-ops.** GLUT sends the ASCII control-character code for `Ctrl+letter` (1=SOH/A, 3=ETX/C, 0x18=CAN/X, 0x16=SYN/V, 0x19=EM/Y, 0x1A=SUB/Z) — not the letter itself. The handler was checking `'c' = 99` etc. and never matched. Now checks the control code (primary) and the letter code (fallback), matching the pattern the existing `Ctrl+N` / `Ctrl+O` handlers use.
- **Fixed: AI Script editor selection highlight drew "way above" the text box.** The blue band quad used top-down `w.y1` Y directly but OpenGL is bottom-up; every other UI quad in the renderer uses `gl_y() = vh - sy`. Single-line and multi-line highlight passes now wrap their vertices in `gl_y()` so the band sits exactly on the selected characters.

### ✨ New Features
- **AI Script editor — full notepad surface, scoped to the AI Script text field only.** The shortcuts and mouse selection are gated on `prop_text_edit_field_ == kAIScriptTextField` (via `App::IsAIScriptTextFocused()`) so other property text fields, the find bar, `Ctrl+N` task picker, save bindings, and `Ctrl+H` continue to work unchanged.
  - `Ctrl+A` Select all
  - `Ctrl+C` / `Ctrl+X` / `Ctrl+V` Copy / cut / paste via Windows clipboard (`Utils::SetClipboardText` / `GetClipboardText`)
  - `Ctrl+Z` Undo (AI-script-local stack, capped at 100 entries)
  - `Ctrl+Y` / `Ctrl+Shift+Z` Redo
  - `Shift+Left/Right/Up/Down/Home/End` Extend selection
  - `Ctrl+Home` / `Ctrl+End` Start / end of buffer
  - `Backspace` / `Delete` / `Enter` / printable typing — replace the active selection (Notepad behaviour)
  - **Mouse**: click places caret and clears selection; `Shift+Click` extends from existing anchor; `Click+Drag` is live drag-selection with auto-scroll at the box edges; clicking elsewhere drops the selection.
- **AI Script editor — local undo/redo.** `App::AiTextEdit` struct snapshots the full text + caret + anchor before each mutating op (insert, paste, cut, delete, replace) so `Ctrl+Z` / `Ctrl+Y` are O(1) and byte-perfect. New edits clear the redo stack (standard editor semantics). The undo/redo writes stay in the live buffer; only `Ctrl+S` / `Ctrl+W` (existing save bindings) commit to the `.qvm` on disk.
- **Selection rendering.** New `prop_text_sel_anchor_` / `prop_text_sel_focus_` ints mirrored into the renderer's `task_tree_view`. A translucent blue band is drawn per visual line, beneath the text so characters stay readable.

### ⌨️ New Keybindings (AI Script editor only)
| Event | Binding | Action |
|-------|---------|--------|
| `AiScriptSelectAll` | `Ctrl+A` | Select all in the AI Script text |
| `AiScriptCopy` | `Ctrl+C` | Copy selection (or whole buffer) to clipboard |
| `AiScriptCut` | `Ctrl+X` | Cut selection (or whole buffer) to clipboard |
| `AiScriptPaste` | `Ctrl+V` | Paste from clipboard at caret |
| `AiScriptUndo` | `Ctrl+Z` | Undo last AI Script edit |
| `AiScriptRedo` | `Ctrl+Y` / `Ctrl+Shift+Z` | Redo last undone edit |
| `AiScriptExtendSel` | `Shift+Left/Right/Up/Down/Home/End` | Extend selection (no auto-commit) |
| `AiScriptDragSel` | Click+Drag in AI Script text | Live drag-selection with edge auto-scroll |

---

## 3.4.0-pre — igi1conv-Only Parsers Migration

### 🛠️ Refactor
- **Removed `source/parsers/` folder entirely.** Every file-conversion call in the editor now goes through the bundled `igi1conv.exe` (v1.7.0, located at `editor/tools/igi1conv/`). The new `source/utils_igi1conv.{h,cpp}` shared runner is the single spawner used by every consumer; previously the editor spawned `igi1conv` only for `dat to-mtp` via a private static in `renderer_objects_atta.cpp`.
- **Single source of truth for asset conversion.** No more in-process duplicate code for formats the bundled CLI already handles — `dat export`, `mtp dump`, `qvm decompile`, `qsc compile`, `res list/extract/append`, `graph export`, `tex to-png/to-tga`, `mef export`, `fnt export`, `terrain export-lmp/export-ctr`.
- **In-process loaders kept only where no CLI subcommand can supply the runtime data the editor needs every frame:**
  - `mef_native` → `source/renderer/` (raw `ParsedGeometry` for GL upload)
  - `fnt_parser` → `source/renderer/` (per-glyph UV/advance for HUD draw)
  - `qsc_lexer`, `qsc_parser` → `source/level/` (AI script token/AST walk for app_editor)
  - `terrain_files` → `source/level/` (LMP/CTR runtime mesh loaders)
  - `qvm_parser`, `qvm_compiler`, `qvm_decompiler` → `source/level/` (used by `verify_level_core`, `--run-tests`, and the qvm roundtrip gtest)
- **Writer classes relocated to consumer folders** (read side replaced by `igi1conv`; write side kept in C++ because no CLI subcommand covers it): `dat_writer`, `graph_writer`, `mtp_writer`, `tex_writer`, `res_writer`, `res_compiler`.
- **Dead code removed:** `mef_parser` (ASCII MEF parser — never invoked), `mef_exporter` (dead helper), `mtp_tool` (the old `mtp_decoder.exe` runner — already superseded by `igi1conv dat to-mtp`).

### 📦 New API
- **`igi1conv::` namespace** in `source/utils_igi1conv.h` exposes high-level wrappers for every subcommand the editor needs:
  `ResList`, `ResExtract`, `ResAppend`, `ResPack`, `DatExportJson`, `DatToMtp`, `MtpDumpJson`, `MtpInfo`, `GraphExportJson`, `GraphInfo`, `TexDecode`, `TexToPng`, `TexToTga`, `TexInfo`, `QvmDecompile`, `QvmInfo`, `QvmDisasm`, `QscCompile`, `QscValidate`, `FntExportPng`, `FntInfo`, `TerrainExportLmp`, `TerrainExportCtr`, `TerrainInfo`, `MefExportObj`, `MefInfo`.
- Each helper constructs the right `igi1conv <cmd> ...` invocation, captures exit code, returns the produced path or stdout text, and logs the run through the existing `Logger::Get()` pipeline.
- `MakeTempPath(suffix)` is exposed for callers that need a stable scratch path under the system temp dir.

### 🐛 Bug Fixes
- **No behavioural change to user-visible editor flow.** All 288 gtest cases run against the relocated parsers: 286 pass, 2 pre-existing writer byte-roundtrip failures (`GraphParserTest.WriteUnchangedIsByteIdentical`, `MtpWriterTest.PreservesUntouchedChunksByteForByte`) carry over from `feature/graph-editor` and are unrelated to this migration.
- **The editor's runtime in-process AI-script edit pipeline** (qvm decompile → qsc edit → qvm recompile → re-validate) is unchanged; only the file paths the parsers live at changed.

### 🔧 Build / Toolchain
- `CMakeLists.txt`: dropped the `file(GLOB SOURCES_PARSERS "source/parsers/*.*")` line and the `target_include_directories(... source/parsers ...)` entry. The `igi_tests` `target_sources` list updated to point at the new locations (`source/renderer/{dat,graph,res,tex}_writer.cpp`, `source/level/{mtp_writer,qsc_lexer,qsc_parser,qvm_parser,qvm_compiler,qvm_decompiler,terrain_files}.cpp`, `source/renderer/{fnt_parser,res_compiler}.cpp`). The `SKIP_PRECOMPILE_HEADERS` block was rewritten with the same new locations.
- `assets/editor/tools/igi1conv/` ships with v1.7.0; the `cmake/fetch_igi1conv.cmake` step still pulls the latest release from GitHub (v1.6.0 was the last "pinned" release, the editor overwrites it with the locally committed v1.7.0 when offline or unchanged).

### 📁 File-level change summary
| Moved from `source/parsers/` | → | New location |
|---|---|---|
| `mef_native.{h,cpp}` | → | `source/renderer/mef_native.{h,cpp}` |
| `fnt_parser.{h,cpp}` | → | `source/renderer/fnt_parser.{h,cpp}` |
| `qsc_lexer.{h,cpp}` | → | `source/level/qsc_lexer.{h,cpp}` |
| `qsc_parser.{h,cpp}` | → | `source/level/qsc_parser.{h,cpp}` |
| `qvm_parser.{h,cpp}` | → | `source/level/qvm_parser.{h,cpp}` |
| `qvm_compiler.{h,cpp}` | → | `source/level/qvm_compiler.{h,cpp}` |
| `qvm_decompiler.{h,cpp}` | → | `source/level/qvm_decompiler.{h,cpp}` |
| `terrain_files.{h,cpp}` | → | `source/level/terrain_files.{h,cpp}` |
| `dat_parser.{h,cpp}` | → | `source/renderer/dat_writer.{h,cpp}` |
| `graph_parser.{h,cpp}` | → | `source/renderer/graph_writer.{h,cpp}` |
| `mtp_parser.{h,cpp}` | → | `source/level/mtp_writer.{h,cpp}` |
| `tex_parser.{h,cpp}` | → | `source/renderer/tex_writer.{h,cpp}` |
| `res_parser.{h,cpp}` | → | `source/renderer/res_writer.{h,cpp}` |
| `res_compiler.{h,cpp}` | → | `source/renderer/res_compiler.{h,cpp}` |
| `mef_parser.{h,cpp}` | → | **deleted (dead)** |
| `mef_exporter.{h,cpp}` | → | **deleted (dead)** |
| `mtp_tool.{h,cpp}` | → | **deleted (dead)** |

---

## 3.3.0-pre — Auto-Save, Unified Undo/Redo & AI Script Hotkey Support

### ✨ New Features
- **Auto-Save System**: New pause-menu "Auto Save" row that displays current state ("Save Enable" / "Save Disable") and an interval spinner with `-`/`+` buttons (10s steps, 10s–3600s range). Toggle with `Ctrl+Shift+A`, increase/decrease interval with `Ctrl+Shift+]` / `Ctrl+Shift+[`. When enabled, the editor auto-saves the current level at the configured interval.
- **SaveState (Ctrl+W) & SaveObjectFile (Ctrl+S) read from qedkeybindings.qsc**: All save hotkeys are now dispatched via `DispatchEventBindings` using bindings loaded from `editor\qed\qedkeybindings.qsc` — no hardcoded keys.
- **AI Script Editor hotkey support**: `Ctrl+W` and `Ctrl+S` now commit the in-flight AI script edit and save the `.qvm` even when the AI script textbox is focused. The text editor pre-checks save bindings and commits the edit before letting the key reach the dispatcher.
- **Pause-menu Save button works while AI textbox is focused**: ESC closing the property panel now commits the AI script edit instead of discarding it.

### 🐛 Bug Fixes
- **Fixed: AI Script not saved via Ctrl+W / Ctrl+S**: The text editor block was intercepting all keys while the AI textbox was focused, so `Ctrl+W` was being inserted as a printable character instead of triggering `SaveState`. Now the editor checks for save hotkeys first, commits the AI edit, and lets the binding fire.
- **Fixed: ESC discarded AI script edits**: Pressing ESC while editing AI script text no longer discards the in-flight edit — it commits `prop_text_buf_` to `ai_script_text_` and sets `ai_script_dirty_=true`.
- **Fixed: Undo/Redo broke building ATTA positions**: After restoring the objects vector, ATTA proxy objects are now marked `modified=true` and `FlushAttaProxiesToMef()` rewrites the MEF binary. Previously the MEF kept post-edit local positions while the proxy's world pos reverted, causing the 3D view and saved level to disagree.
- **Fixed: Pause-menu AutoSave row not updating display**: The pause-mode `task_tree_view` struct was missing `auto_save_enabled_` and `auto_save_interval_seconds_` fields, so the renderer always showed defaults (`false`, `300s`) regardless of the actual state. Now both fields are passed in the pause-mode path.
- **Fixed: ResetLevel did not fully reset graphs/AI/textures**: ResetLevel now does a full folder replacement (`remove_all` + `copy recursive`) instead of in-place `overwrite_existing`, so deleted graph nodes, removed AI scripts, and any other stale files are all reverted. The backup is now always created on first level load (not gated on `enableBackup`) and captures the entire `missions\location0\levelX\` folder.
- **Fixed: AutoSave config not reloaded on startup**: `QEDAutoSaveEnabled` and `QEDAutoSaveInterval` were saved to `qedconfig.qsc` but never read back. The parser now restores both on launch.
- **Fixed: Terrain undo/redo didn't restore heightmap edits**: Added `SnapshotHMP()` / `RestoreHMP()` on `Terrain` (and a public pass-through on `Level`) that byte-copies the loaded HMP file body. Undo/redo now captures and restores the full heightmap buffer so brush edits can be rolled back.
- **Fixed: Editor created a `content` folder in the install path**: All asset cache stamps moved from `output_dir\content\cache` to `output_dir\editor\cache`; the terrain extract dir moved from `content\terrains` to `editor\terrains`. The editor no longer writes anywhere outside `editor\`.

### ⌨️ New Keybindings
| Event | Binding | Action |
|-------|---------|--------|
| `ToggleAutoSave` | `<Ctrl><Shift><A>` | Toggle auto-save on/off |
| `AutoSaveIntervalUp` | `<Ctrl><Shift><]>` | Increase auto-save interval by 10s |
| `AutoSaveIntervalDown` | `<Ctrl><Shift><[>` | Decrease auto-save interval by 10s |

### 🔧 Technical Changes
- **Unified `UndoState`**: Snapshots all editable state in one struct — objects (including ATTA proxy fields, spline data, lighting, scale), AI script (path/text/dirty), terrain HMP buffer, terrain mod options, and graph overlay (nodes/edges/visibility). One undo covers every edit type.
- **`Undo` / `Redo` re-flush ATTA proxies** after restoring the object list, so MEF binaries stay in sync with the proxy world positions.
- **`ResetLevel` always restores from full-folder backup** when one exists; the backup is created on first load of each level.
- **Cache and terrain extract dirs moved under `editor\`** so the install path stays clean.

### 🐛 Bug Fixes (post-release patches)
- **Fixed: HumanAI not found when nested under a non-AI child (e.g. `HumanSoldier → GunM16A2 → HumanAI`)**: `LoadAIScriptForSelected()` now walks the children tree recursively (BFS, up to 15 levels deep) instead of only checking direct children. Previously, when the `HumanAI` was nested inside another child task, the direct-child check failed and the AI script section showed nothing. If not found within 15 levels a warning is logged.
- **Fixed: AI script editor section never appeared for HumanSoldiers whose modelId isn't tagged `AITYPE_` in IGIModels.json**: `LoadAIScriptForSelected()` and `selected_obj_is_ai` now also check the object type (`HumanSoldier`, `HumanSoldierFemale`, `HumanPlayer`, `HumanSoldierRPG`, `HumanAI`) in addition to the `ai_model_ids_` modelId set, so the AI Script section renders for all AI containers regardless of their modelId.
- **Fixed: AI script load diagnostic logging**: when the AI script is successfully loaded, the path and resolved `HumanAI` taskId are logged; when `HumanAI` is found but has an empty `taskId`, a warning is logged.

### ⌨️ Pause Menu Reorder & UI Polish
- Pause menu now lists rows in this order: **Resume → Font → Select Level → Auto Save → Model Search → Terrain Options → Reset Level → Save Level → Quit**
- Row spacing increased from 35px to 38px for a cleaner, less cramped layout; first row starts at a slightly higher position for better top padding
- Title "IGI EDITOR" and subtitle "PAUSED" are now centered by measured text width (was hardcoded offset)
- Terrain header and checkbox rows use the same `strlen * 4` centering as plain buttons, so all text is consistently aligned
- Auto Save label shortened from "Auto Save Enable" / "Auto Save Disable" to **"Save Enable"** / **"Save Disable"** for a cleaner row
- Click hit-zones updated to ±16px to match the new 38px row spacing

### 🎯 Pause Menu Alignment Fixes
- **Spinner rows now share identical layout**: Font / Select Level / Save Enable all use the same `btn_w=22`, `gap=6`, `val_w=44`, `label_gap=14` and compute label width dynamically from `strlen(lbl) * 6`. The whole label+spinner group is centered in the 460px menu, so the `- [val] +` group sits at the same horizontal position in every spinner row.
- **Model Search text input box no longer overflows** the menu border — box width is now 200px (was 200px with a wider label) and the whole label+box group is centered.
- **Centering math corrected**: button-label centering was using `strlen * 4` (only correct for 6-7 char strings like "PAUSED") which shifted longer strings like "Reset Level" and "Save Level" off-center. Changed to `strlen * 3` (half of the 6px font width) so all plain buttons center correctly relative to the menu's vertical axis. Title "IGI EDITOR" and subtitle "PAUSED" use the same correct formula.

---

### ✨ New Features
- **Auto-Save System**: New pause-menu "Auto Save" row that displays current state ("Auto Save Enabled" / "Auto Save Disabled") and an interval spinner with `-`/`+` buttons (10s steps, 10s–3600s range). Toggle with `Ctrl+Shift+A`, increase/decrease interval with `Ctrl+Shift+]` / `Ctrl+Shift+[`. When enabled, the editor auto-saves the current level at the configured interval.
- **SaveState (Ctrl+W) & SaveObjectFile (Ctrl+S) read from qedkeybindings.qsc**: All save hotkeys are now dispatched via `DispatchEventBindings` using bindings loaded from `editor\qed\qedkeybindings.qsc` — no hardcoded keys.
- **AI Script Editor hotkey support**: `Ctrl+W` and `Ctrl+S` now commit the in-flight AI script edit and save the `.qvm` even when the AI script textbox is focused. The text editor pre-checks save bindings and commits the edit before letting the key reach the dispatcher.
- **Pause-menu Save button works while AI textbox is focused**: ESC closing the property panel now commits the AI script edit instead of discarding it.

### 🐛 Bug Fixes
- **Fixed: AI Script not saved via Ctrl+W / Ctrl+S**: The text editor block was intercepting all keys while the AI textbox was focused, so `Ctrl+W` was being inserted as a printable character instead of triggering `SaveState`. Now the editor checks for save hotkeys first, commits the AI edit, and lets the binding fire.
- **Fixed: ESC discarded AI script edits**: Pressing ESC while editing AI script text no longer discards the in-flight edit — it commits `prop_text_buf_` to `ai_script_text_` and sets `ai_script_dirty_=true`.
- **Fixed: Undo/Redo broke building ATTA positions**: After restoring the objects vector, ATTA proxy objects are now marked `modified=true` and `FlushAttaProxiesToMef()` rewrites the MEF binary. Previously the MEF kept post-edit local positions while the proxy's world pos reverted, causing the 3D view and saved level to disagree.
- **Fixed: Pause-menu AutoSave row not updating display**: The pause-mode `task_tree_view` struct was missing `auto_save_enabled_` and `auto_save_interval_seconds_` fields, so the renderer always showed defaults (`false`, `300s`) regardless of the actual state. Now both fields are passed in the pause-mode path.
- **Fixed: ResetLevel did not fully reset graphs/AI/textures**: ResetLevel now does a full folder replacement (`remove_all` + `copy recursive`) instead of in-place `overwrite_existing`, so deleted graph nodes, removed AI scripts, and any other stale files are all reverted. The backup is now always created on first level load (not gated on `enableBackup`) and captures the entire `missions\location0\levelX\` folder.
- **Fixed: AutoSave config not reloaded on startup**: `QEDAutoSaveEnabled` and `QEDAutoSaveInterval` were saved to `qedconfig.qsc` but never read back. The parser now restores both on launch.
- **Fixed: Terrain undo/redo didn't restore heightmap edits**: Added `SnapshotHMP()` / `RestoreHMP()` on `Terrain` (and a public pass-through on `Level`) that byte-copies the loaded HMP file body. Undo/redo now captures and restores the full heightmap buffer so brush edits can be rolled back.
- **Fixed: Editor created a `content` folder in the install path**: All asset cache stamps moved from `output_dir\content\cache` to `output_dir\editor\cache`; the terrain extract dir moved from `content\terrains` to `editor\terrains`. The editor no longer writes anywhere outside `editor\`.

### ⌨️ New Keybindings
| Event | Binding | Action |
|-------|---------|--------|
| `ToggleAutoSave` | `<Ctrl><Shift><A>` | Toggle auto-save on/off |
| `AutoSaveIntervalUp` | `<Ctrl><Shift><]>` | Increase auto-save interval by 10s |
| `AutoSaveIntervalDown` | `<Ctrl><Shift><[>` | Decrease auto-save interval by 10s |

### 🔧 Technical Changes
- **Unified `UndoState`**: Snapshots all editable state in one struct — objects (including ATTA proxy fields, spline data, lighting, scale), AI script (path/text/dirty), terrain HMP buffer, terrain mod options, and graph overlay (nodes/edges/visibility). One undo covers every edit type.
- **`Undo` / `Redo` re-flush ATTA proxies** after restoring the object list, so MEF binaries stay in sync with the proxy world positions.
- **`ResetLevel` always restores from full-folder backup** when one exists; the backup is created on first load of each level.
- **Cache and terrain extract dirs moved under `editor\`** so the install path stays clean.

---

## 3.2.0-pre — Graph Link Editing, Legacy Format & Edge Visibility

### ✨ New Features
- **Add/Remove Links (Two-Step Workflow)**: Navigation edges between graph nodes can now be added and removed interactively. Select node A, press `Alt++` to mark it as the link source (green double-ring indicator), select node B, press `Alt++` again to create the link. Use `Alt+-` to remove a link the same way.
- **Node Label Toggle**: On-screen node ID labels can now be toggled on/off with `Alt+L`. The title banner shows `[labels off]` when disabled.
- **Legacy Graph Format Support**: Added a parser, full writer, and position-patch saver for the undocumented alternate tagged graph format (1-byte type tags: `0x05`=int32, `0x06`=float, `0x08`=Vec3d, `0x09`=string, no magic number). This fixes the "bad magic" error that prevented level 8's `graph1.dat` and `graph7.dat` (and other legacy graphs) from loading and displaying.

### 🐛 Bug Fixes
- **Fixed: "Bad magic" error on level 8 graph1**: Graph files using the legacy tagged format (no `0xFFEEDDCC` magic) are now auto-detected and parsed transparently — both standard and legacy formats produce the same editable `GraphFile` and support full add/remove/edit/save.
- **Fixed: Links not visible / not above ground**: Edge lines were drawn at node Z (ground level), getting buried under terrain. Edges now render at the vertical centre of the node boxes (`z + H`) so they sit above ground and are clearly visible.

### ⌨️ New Keybindings
| Event | Binding | Action |
|-------|---------|--------|
| `AddGraphLink` | `<Alt><Plus>` | Two-step: mark source node, then link to target node |
| `RemoveGraphLink` | `<Alt><Minus>` | Two-step: mark source node, then unlink target node |
| `ToggleGraphNodeLabels` | `<Alt><L>` | Toggle on-screen node ID labels |

### 🧪 Tests
- Added 4 new tests for the legacy graph format: parse validity, node data sanity, write round-trip, and position-patch save.

---

## 3.1.0-pre — Visual 3D Graph Editor

- Added: Visual 3D Graph Editor (interactive 3D nodes, material color coding, and path/edge rendering via F3 overlay).
- Added: Binary parser, patch saver, and full writer serialization for AI navigation graph `.dat` files.
- Added: Config option `QGraphNodeSize` to control node box sizing.
- Fixed: Terrain sculpting vs object selection click conflicts, right-click triggers.
- Fixed: Syncing of level graph subdirectories to backup folders.

---

## 3.0.0 — igi1conv Integration, Qt Bundling, Module Refactor & Bug Fixes

This release is a major milestone: the standalone asset converter is now a first-class Qt application (`igi1conv`) living in its own repository, the editor codebase has been split into clearly separated modules, and a batch of code-review bugs has been resolved.

### 🔧 igi1conv — Standalone Asset Converter (Qt)
- **Dedicated repo**: `igi1conv` is developed at [project-igi-conv](https://github.com/heaven-hm/project-igi-conv) and bundled prebuilt with each editor release.
- **Qt application**: Ships with a full graphical GUI mode and a headless CLI mode. The editor invokes only the CLI internally (`dat to-mtp`, etc.).
- **Full Qt package**: The entire runtime (exe + Qt5Core/Qt5Gui/Qt5Widgets/Qt5Svg DLLs, platform plugins, image formats) is bundled at `editor/tools/igi1conv/` — no separate Qt installation required.
- **v1.6.0**: Updated to `igi1conv` v1.6.0 with improved asset conversion accuracy.
- **Native MTP generation**: The editor delegates all `.mtp` generation to `igi1conv dat to-mtp`, which reproduces the original game `.mtp` byte-for-byte and fixes transparent/wrong-texture in-game issues.

### 🗂️ Module Refactor
- **renderer_objects** split into picking, mesh, visual, metadata, texture, and ATTA attachment subsystems.
- **app** split into `app_input` (mouse + keyboard + dispatch), `app_editor`, `app_view`, `app_lookup`, `app_level`, and `app_ui` modules.
- **terrain** implementation split into terrain_io, terrain_lod, terrain_mesh, and terrain_query.
- **LevelObjects** QSC serialization extracted into its own module.

### 🖥️ UI Improvements
- **Level Spinner**: Level number selector added to the pause menu for quick level switching.
- **Texture Panel Removed**: Texture list removed from the pause menu for a cleaner layout.
- **Editor File Verification on Launch**: Missing `editor/` directory files are detected and reported with a clear error dialog before the editor reaches the render loop.

### 🐛 Bug Fixes
- **Arrow Keys**: Arrow keys no longer move the camera unless `SHIFT+ALT` is held — fixes accidental camera drift during text input.
- **Weapon Orientation**: Corrected weapon/ammo model orientation so pickups display at the right angle in-game.
- **ESC Menu Re-open**: Fixed regression where pressing ESC a second time failed to reopen the pause menu.
- **Terrain Rings on Right-Click**: 3D brush rings now appear immediately when terrain is selected via right-click.
- **DAT Round-Trip Loss**: Fixed data loss when a DAT file was written back after read.
- **matCount Guard**: Added bounds check to prevent out-of-range material slot access.
- **`std::exit` replaced**: Replaced bare `std::exit` calls with proper cleanup paths.
- **VNAM Shadow**: Fixed variable shadowing bug in VNAM chunk parsing.
- **vert_info Overflow**: Corrected vertex info buffer overflow on large meshes.
- **JSON Parser**: Fixed edge-case crash in the embedded JSON parser on malformed input.
- **Boundary-Aware Texture Matching**: `009_01_1` can no longer accidentally grab `1009_01_1.tex`.

---

## 2.9.0 — New Terrain Editor, Foreign Models Support & UI Fixes
This release introduces a new Terrain editor, support for loading and adding foreign models from other levels, and critical bug fixes to the pause menu layout and viewport interaction.

### 🗺️ New Terrain Editor
- **Interactive Terrain Tools** — Enhanced level design capabilities with the new terrain editor interface and brush controls.

### 📦 Foreign Model Support
- **Cross-Level Assets** — Added the ability to import and add foreign models from different game levels directly into the current level.

### 🔧 Pause Menu & UI Bug Fixes
- **Terrain Options Expand/Collapse** — Fixed the list in Terrain Options in the Pause menu to properly expand and collapse when clicked.
- **Immediate 3D Rings Refresh** — When terrain is selected via right-click, the 3D brush rings now display immediately in the viewport without delay by forcing immediate frame redisplays.

---

## 2.8.0 — Inline AI Script Editor, Mini-Notepad, Autocomplete Overhaul & Find Fixes
This release delivers a full inline AI script editor embedded inside the property panel, a proper mini-notepad text editor with scrolling and arrow key navigation, a global autocomplete fix, and three keyboard shortcut corrections in the find system.

### 🤖 Inline AI Script Editor
- **Correct Task ID Resolution** — AI script path now uses the `HumanAI` child task's ID (e.g. `2203`) instead of the parent `HumanSoldier` ID, correctly pointing to `ai/2203.qvm`.
- **Path Textbox** — Editable single-line textbox showing the full `.qvm` path. Type a new path and press Enter to load and decompile that file. Horizontal scrolling keeps long paths visible.
- **Script Textbox** — Tall multiline textbox showing the decompiled QSC source. The orange "modified" label appears whenever unsaved edits exist.
- **Compile-on-Save** — AI script edits are compiled back to `.qvm` only when the user saves the level (F3 / Save), with round-trip validation. Errors are shown in the status bar; dirty flag stays set on failure.

### 📝 Mini-Notepad Text Editor
- **Vertical Scrolling** — Mouse wheel scrolls the script content. PageUp / PageDown jump a screen at a time.
- **Arrow Key Navigation** — Left/Right move the caret one character. Up/Down move by visual line, preserving the column position. Works correctly across `\n`-separated lines and wrapped lines.
- **Mouse Click Positioning** — Clicking inside the script box positions the caret at the character closest to the click, accounting for the current scroll offset.
- **Path Horizontal Scroll** — The path box scrolls horizontally as the caret moves past the visible area.
- **Proper `\n`-Aware Wrapping** — The `draw_edit_box` renderer now splits on `\n` characters AND wraps at the box width, showing each logical line correctly. Cursor is always on the right visual line.
- **Editable Appearance** — Both boxes now draw a dark background and a white/yellow border (yellow = active editing), matching the visual style of all other property fields.

### 🔡 Autocomplete Everywhere Fixed
- **Root Cause** — Field-ID sentinels for AI boxes are `-10`/`-11` (both `< 0`), which triggered a stale `prop_text_edit_field_ < 0` guard that blocked Ctrl+N, Ctrl+Space, and Ctrl+O in every field where they were hit. All guards now check `== -1`.
- **Ctrl+N / Ctrl+Space / Ctrl+O** — All autocomplete and picker paths now work in the AI script box, the AI path box, and all standard string fields.
- **Caret-Preserving Insert** — When Ctrl+N opens the keyword picker, the cursor position is captured (`picker_target_caret_`). Confirming a pick INSERTs the keyword at that exact position in the AI script box. Standard fields keep the existing full-replace behaviour.
- **DispatchEventBindings Paths Fixed** — `AutoCompleteTaskName` and `AutoCompleteModelName` event bindings also had the stale guard; both now fixed and save the caret on open.

### ⌨️ Find Shortcut Fixes
- **TaskFindTextInTask** — Rebound from `Ctrl+H` (ASCII 8 = Backspace, permanently blocked) to `Ctrl+Shift+X`.
- **TaskFindByTaskNote** — `Ctrl+Shift+N` was being silently swallowed by the Ctrl+N autocomplete intercept (both produce ASCII 14 in GLUT). Guard now checks `!shiftDown` so Ctrl+Shift+N correctly reaches `DispatchEventBindings`.
- **TaskFindAgain** — After finding the next match, `Ctrl+Shift+F` now scrolls the task tree so the found item is visible and highlighted.

---

## 2.7.0 - 3D Model Viewer, Autocomplete Task, Exact Keybindings & Task Tree Fixes
This release introduces an interactive rotating 3D Model Viewer for selected level assets, auto-complete for task inputs, and critical fixes for keybinding collisions and task subtree importing/exporting.

### 📐 3D Model Viewer
- **Real-time 3D Preview**: Renders a spinning 3D preview of the selected model inside the Model ID picker (`Ctrl+O`), loaded natively from MEF files.
- **Automatic Scaling and Orientation**: Preview models rotate automatically across dual axes and are scaled to fit the preview frame dynamically.

### 🔤 Task Autocomplete
- **Task Type Picker**: Added a dedicated sidebar autocomplete task type picker (`Ctrl+N` panel) with instant incremental search.
- **Inline Keyword Auto-complete**: Trigger keyword suggestions directly using `Ctrl+Space` for rapid task configuration.

### ⌨️ Exact Keybinding Validation
- **Modifier Exact Matching**: Dispatches event bindings only when key modifiers match exactly, preventing shortcuts like `Ctrl+C` from firing during `Ctrl+Shift+C` commands.
- **Autorotated Keybinding Config**: Preserves named configurations (`SaveSubTask`, `TaskMagicObjToggle`, etc.) directly from `qedkeybindings.qsc` instead of resetting them.

### 🔧 Task Tree & File Dialogs
- **Subtree Export & Import**: Save task subtrees to custom files and load them back onto target nodes dynamically.
- **Task Creation options**: Insert new tasks directly at the camera's coordinates or insert them at specific task tree locations.

---

## 2.6.0 - Properties Editor UI, Attachments, Splines & CLI Tools
This release introduces a new Properties Editor UI, support for editable Attachments, spline track fixes, and enhanced CLI tools for image and font atlas processing.

### 📋 Properties Editor & Attachments
- **Properties Editor UI**: Added a brand new User Interface for the Properties Editor.
- **Editable Attachments**: Added support for editing and configuring Attachments objects.

### 🛣️ Road Tracks & Assets
- **Road Tracks**: Fixed spline calculation and rendering issues for Road Tracks (`SplineObjs`).
- **Editor Fonts & Sprites**: Added custom Editor Fonts and Sprites assets.

### 🖼️ Image Conversion Utilities
- **Direct Format Conversion**: Added `--ToPng` and `--ToTga` arguments to convert individual `TGA` or `PNG` files instantly via the CLI.
- **PNG Encoding**: Integrated `stb_image_write.h` to natively write high-quality PNGs directly from the engine.

### 🔤 Font Output Improvements
- **FNT Atlas Export**: New `--export-png` flag for the `--fnt` parser extracts the internal bitmap texture of font files directly to PNG.
- **Glyph String Concatenation**: The `--fnt` info log now aggregates all active glyphs and prints them as a single string for quick character set verification.

### 🔧 Stability
- **Bug Fixes**: Fixed general bugs and improved overall editor stability.

---
## 2.5.0 - Property Panel Scrolling & Child Task Display
This release adds a scrollable property editor and displays weapon/ammo fields for soldier units.

### 📋 Enhanced Property Editor
- **Vertical Panel Scrolling**: Property panel now supports mouse-wheel scrolling for large task schemas, with a visual scrollbar indicator.
- **Child Task Fields Display**: Weapon, ammo, and AI sub-task fields now appear inline below parent task properties as read-only previews for quick reference.
- **Panel Scroll Reset**: Scroll position automatically resets when selecting a new object, preventing disorientation.

### 🎯 UI Improvements
- **Compact Scrollbar**: Minimal scrollbar thumb appears only when content overflows the visible panel area.
- **Keyboard-Free Navigation**: Intuitive mouse-wheel scrolling makes exploring large task hierarchies seamless without switching to the tree view.

---

## 2.4.0 - Position Editor Drag & All-Object Picking
This update fixes position editing when the cursor hits window edges and enables clicking collision-only meshes.

### 🖱️ Position Editor Edge Continuity
- **Stuck Cursor Handling**: When your mouse hits the window edge, the position editor now remembers your last drag direction and continues moving the object smoothly.
- **Per-Frame Delta Logic**: Switching from cumulative to per-frame drag delta prevents stalling when the cursor can't move further.
- **Seamless Manipulation**: No need to re-position your mouse—object keeps moving in the direction you were pushing.

### 🎯 Universal Object Picking
- **Collision-Mesh Fallback**: Vehicles, cargo containers, and other collision-only models are now clickable in the viewport, even without render geometry.
- **All Attachments Pickable**: MEF sub-models now serve as valid pick targets, allowing selection of child objects through any mesh surface.

---

## 2.3.0 - Font Toggle & Text Cursor Alignment
This release adds live font switching and precise text cursor positioning in property textboxes.

### 🔤 Font System Enhancements
- **Live Font Toggle**: New "Font: Editor / Font: System" button in pause menu switches between bitmap fonts at runtime without reloading.
- **Dynamic Font Switching**: All HUD text instantly updates when toggling fonts—no need to restart the editor.
- **Clean UI**: Removed unused Debug button and replaced with functional font control.

### ✏️ Text Editing Precision
- **Cursor-to-Glyph Alignment**: Text cursor now sits exactly where typed characters will appear, not offset to the right.
- **Per-Character Advances**: Cursor position calculated from actual glyph advance widths, supporting variable-width fonts.
- **Arrow Key Navigation**: Caret movement with arrow keys now aligns perfectly with the rendered text.

---

## 2.2.0 - Building Interior Occlusion & Door Selection
This release improves building visibility logic and allows selecting doors, lights, and cameras from outside.

### 🏢 Smart Building Occlusion
- **Selective Child Hiding**: Only interior sub-structures are hidden when viewing from outside; doors, lights, and security cameras remain clickable on exterior surfaces.
- **Exterior Attachment Support**: Building-attached objects (doors, alarms, cameras) are now properly selectable from outside without hiding them.
- **Ancestor Chain Traversal**: Complex nested building structures now correctly hide only true interior children, not surface attachments.

### 🔍 Improved Selection
- **Full Building Hierarchy Support**: Sub-children of buildings (nested MEF attachments) can now be selected from inside the building.
- **Occlusion Depth Testing**: Building hulls render first in the picking pass, using GPU depth testing to properly occlude interior objects from outside views.

---

## 2.1.0 - Windows DPI Scaling & Windowed-Mode Precision
This release fixes cursor precision in windowed mode on high-DPI displays.

### 💻 High-DPI Support
- **Per-Monitor DPI Awareness**: Editor now declares DPI awareness to Windows, ensuring GLUT delivers physical pixel coordinates.
- **Windowed Mode Precision**: Slider, button, and textbox hit-tests now respond exactly under your cursor at 100%, 125%, and 150% display scaling.
- **Runtime DPI Detection**: Fallback logic automatically detects display scaling if the OS manifest isn't detected.

### 🎨 Widget Accuracy
- **Pixel-Perfect Hit-Testing**: All interactive elements (sliders, text fields, buttons) respond with zero offset, even on scaled displays.
- **Cross-Display Compatibility**: Works seamlessly with external monitors at different scaling factors.

---

## 2.0.0 - Workspace Navigation & Map Selection
This release introduces direct 3D interaction and enhanced state restoration.

### 🖱️ Workspace & State Polish
- **Map View Selection**: Implemented click-to-select support on the main map rendering, enabling direct object interaction from the 3D viewport canvas.
- **Undo/Redo Stability**: Fixed state restoration where Train locations are correctly reserved and restored when performing Undo operations.

---

## 1.9.0 - Train Engine & Precision Logic
This update adds support for railway networks and addresses mission serialization limitations.

### 🚂 Train & Locomotive Support
- **Train Object Support**: Initial integration and rendering of Train models and locomotives.
- **Locomotive Orientation**: Resolved critical direction and alignment bugs when placing locomotive engines.
- **Track Spline Parsing**: Implemented robust rendering and Catmull-Rom smoothing for train track splines.

### 📐 Parser & Level Synchronization
- **Float/Double Precision Guard**: Solved precision truncation issues within QSC file serialization, preserving exact floating-point decimals.
- **Level 3 Integrity Fix**: Patched bracket and node compiler errors when saving mission data in Level 3.

---

## 1.8.0 - Level Cache, Textures, and CLI Standalone
This major update accelerates startup times and establishes a standalone utility toolchain.

### ⚡ Performance & Caching
- **Level Load Cache System**: Introduced an advanced caching layer for models and textures, decreasing startup load times across repeated sessions.
- **AI Texture Map Fixes**: Corrected texturing and mesh configurations for AI MEF models.
- **Render Distance Optimization**: Stabilized dynamic clip distance and global coordinate mappings.

### 🛠️ Headless Toolchain
- **CLI Independence**: Unlocked headless functionality from local executable path structures, allowing command-line tools to resolve game directories standalone.

---

## 1.7.0 - Robust Hierarchical Manipulation & Undo/Redo
This release focus on stabilizing complex object manipulation and providing essential safety features for level editing.

### 🔄 State Management & Safety
- **Multi-Step Undo/Redo**: Implemented a comprehensive Undo/Redo system for object manipulation. The editor now tracks level snapshots, allowing users to revert and redo changes with `CTRL+Z` and `CTRL+Y` (fully configurable via `qedkeybindings.qsc`).
- **Persistent State Fix**: Resolved a persistent bug where modifications to building objects were reset when selecting a different object.
- **Improved Change Tracking**: Updated `LevelObjects::SaveToQSC` to automatically clear stale cached lines for all modified objects, ensuring changes are always reflected in the underlying script.

### 📐 Precision Hierarchical Manipulation
- **Global Root Pivot Enforcement**: Fixed a critical bug in hierarchical rotation where child objects would drift or disappear. Nested descendants (furniture, items, AI) now rotate relative to the root building origin, ensuring perfect coordinate stability.
- **AI Graph Synchronization**: AI soldiers inside buildings now have their internal `graphPos` metadata automatically updated during building translation and rotation, maintaining pathing integrity.
- **Building Snap Preservation**: Refined hierarchical "Snap to Ground" and "Snap to Object" to ensure all child objects maintain their relative offsets after the parent snaps.
- **Snap Search Radius**: Added a 5000-unit search radius to "Snap to Object" for more predictable nearest-neighbor detection.

### 📜 Hierarchical Synchronization
- **Hierarchical Task Editor Sync**: The Task Editor now live-updates with the **full hierarchical script tree** when a container object is manipulated, preventing data loss in the script view.
- **Balanced Parenthesis Engine**: Completely refactored the QSC serialization engine to guarantee perfectly balanced parentheses and correct comma separation in saved mission files.

---

## 1.6.0 - Advanced QVM Configuration & In-Memory Logic
This update transitions the editor to a proprietary configuration system powered by the IGI bytecode engine.

### ⚙️ QSC/QVM Configuration System
- **Unified Config Standard**: Migrated all editor settings and keybindings from legacy `.txt` files to a robust `.qsc` (source) and `.qvm` (compiled) system.
- **In-Memory Decompilation**: Implemented `QVM_DecompileToString` in `qvm_decompiler.cpp`. The editor now parses compiled `.qvm` files and decompiles them directly into a RAM string for processing, eliminating temporary file overhead.
- **Automated Startup Compilation**: The editor now automatically scans the `content/qed/` directory and compiles any new or updated `.qsc` configuration scripts to `.qvm` bytecode at launch.
- **QED Prefix Enforcement**: Enforced a new project-wide convention where all configuration keys must be prefixed with `QED` (e.g., `QEDFontSize`, `QEDLevel`) and lines terminate with a semicolon (`;`).

### 🛠️ Advanced QED Parameters
- **New Engine Parameters**: Added support for 20+ advanced configuration settings including `QEDConsoleAutoActivate`, `QEDSearchType`, `QEDInvertMouse`, `QEDDisplayTaskNote`, and `QEDCameraLock`.
- **Dynamic File Overrides**: Implemented `QEDSetObjectFile("path")` with support for `LOCAL:` path resolution, allowing modders to load specific object scripts for rapid testing.
- **Camera State Persistence**: Added configuration support for initial camera orientation, radius, and matrices, allowing the viewport to restore its exact state between sessions.

---

## 1.5.0 - Asset Extraction & Advanced Native Fixes
This release introduces automated asset extraction and addresses critical rendering and caching bugs for complex native models.

### 📦 Automated Asset Management
- **Level Asset Extractor**: Implemented a robust `AssetExtractor` system that automatically unzips textures and models from `.res` archives into the editor's working directory on-demand (`--extract-level` CLI).
- **Timestamp Caching**: Added file-timestamp validation to ensure `.res` extraction only occurs when necessary, drastically improving level loading speeds across sessions.
- **Level Cache Isolation**: Fixed critical caching bugs where meshes and textures from a previously loaded level (e.g., Level 4) would bleed into the active level (e.g., Level 1). The engine now completely purges the GL mesh, texture, and DAT map caches when switching levels.

### 📐 Advanced Native Rendering Fixes
- **Bone Model Geometry (Type-1)**: Fixed an issue where character models (like `013_01_1` and `003_02_1`) failed to load faces. The `ParseSplitBoneTriangles` parser now dynamically scans all ECAF offsets across out-of-order blocks to accurately calculate missing triangle counts.
- **Building Floors & Collision Rendering (Type-3)**: Fixed a major rendering bug where multi-room buildings were missing their floors. The editor now correctly extracts `XTVC`/`ECFC` collision chunks (where the original game engine stores untextured structural elements like floors and ramps) and appends them as standalone untextured render blocks.
- **Precision Z-Offset Snapping**: Refined the terrain snapping system (`mainZOffset`) for buildings. The calculation now strictly utilizes the lowest Z-coordinate of *textured* submeshes, preventing invisible underground foundations from interfering with accurate floor-to-terrain alignment.

---

## 1.4.0 - Native Game Engine Parity & Toolchain Expansion
This major release marks the complete transition to the Project IGI native file ecosystem. The editor now directly parses and manages proprietary game assets, providing pixel-perfect parity with the original game engine and a robust headless toolset for modders.

### 🛠️ Native File Format Ecosystem
- **Native MEF (Model Engine Format)**: Complete replacement of the legacy OBJ/GLB pipeline with a high-performance proprietary MEF parser.
- **Bone & Rigging Support**: Full support for character bone hierarchies, rig mapping, and animation playback structures.
- **Native TEX Loader**: Direct support for IGI Texture formats (Version 7, Mode 67 BGRA8888) with mipmap and transparency handling.
- **RES Archive Parser**: Built-in ILFF/IRES archive management for dynamic asset extraction from original game resource files.
- **MTP Package Support**: Implementation of the FORM/IFF structured texture mapping parser for complex model-texture packaging.

### 📜 Integrated Scripting & VM Toolchain
- **QVM Decompiler**: Robust reverse-engineering of IGI bytecode (Version 0.5) back to human-readable QSC scripts.
- **QCompiler Pipeline**: Seamless integration of mission logic compilation, enabling direct saving of QSC changes into game-ready QVM binaries.
- **Bytecode Verification**: Automated integrity checks for compiled scripts to ensure stability in the game engine.

### 💻 Headless CLI & Automation
- **Unified Headless Toolset**: Added advanced command-line switches to `igi1ed.exe` for automated workflows:
  - `--extract-level <num>`: Batch extract all level resources (Models, Textures, Terrain).
  - `--mef <file>`: Detailed model geometry and bone structure analysis.
  - `--qsc/--qvm`: CLI-based compilation and decompilation routines.
  - `--res/--mtp`: Archive listing and specific block extraction.
  - `--terrain`: Raw heightmap and lightmap structure analysis.

### 🚀 Performance & Systems Optimization
- **AssetExtractor**: Intelligent management system for level data with file-timestamp caching to prevent redundant extractions.
- **Unified Logger**: Level-specific instrumentation with debug dump support and improved UI telemetry.
- **AppData Synchronization**: Automated migration and locking between local editor folders and `%APPDATA%\QEditor`.
- **Skeletal Rendering**: Improved character rendering with accurate bone-weight visualization and coordinate alignment.

---

## 1.0.0 - Official First Public Release
Welcome to the first public release of the **Project IGI 3D Editor**! This release marks a massive milestone, providing a state-of-the-art 3D modding suite with intuitive controls, visual script compilation pipelines, and precision world manipulation tools.

### 🌟 Core Public Release Features
* **Interactive 3D Terrain & Heightmap Editor**: Fully rendered real-time 3D terrain viewport with heightmap sculpting brushes.
* **Procedural Spline & Waypoint System**: Dynamic editing of track splines, waypoint structures, and repeated mesh fence/wire layouts.
* **Flight Camera & 3D Navigation**: Full 6-DOF fly-through camera with fine-grained speed controls, teleportation hotkeys, and real-time navigation grid.
* **Complete Task & Object Editor**: Just like the classic IGI 2 Editor, you can add new tasks (`Task_New`), duplicate node hierarchies, copy/paste selections, delete objects, and perform full multi-step **Undo/Redo** operations.
* **AI Behavior & Mission Intel Editor**: Full editing suite for soldier paths, movement node scripts, tactical AI weapon/ammo layouts, and mission logic constraints.
* **All 14 Levels Supported**: Robust support for decompiling, editing, and compiling all 14 missions of the original game (with the first 3 levels fully tested and verified).
* **Automatic Backup & Recovery**: Integrated restore systems that safeguard your pristine game files and allow instant reset to factory level state.

---

## BETA 0.0.9 - Global Database Search & UI Polish
### Global Model Search Database
- **Global Model Search Mappings**: Replaced local viewport-only model searches in `SearchModelById()` with a global database search inside `%APPDATA%\QEditor\IGIModels.json`.
- **Query Master JSON**: Replaced `SearchModelByName()` logic to query the global master JSON database and find matching model mappings.
- **Robust JSON Parsing**: Implemented `LoadAllModelsFromJson()` standalone C++ helper to dynamically parse JSON structures containing `ModelName` and `ModelId`.
- **Native Prompts**: Used standard, native **Windows InputBox** dialogue (`Utils::PromptForText`) to request user search queries.
- **MessageBox Integration**: Used **Windows MessageBox** (`MessageBoxA`) to display structured, multi-line database search matches.
- **Encoding Fix**: Prevented encoding glitches (such as the `â€¢` bullet character rendering bug) by implementing CP1252-safe ASCII hyphens (`- `) for item bulleting.
- **Output Truncation**: Capped maximum displayed search results to the first 25 matches with trailing count truncation (`... and X more matches`) to prevent message boxes from overflowing the screen.

### UI Telemetry & Rendering Stabilization
- **Right-Click Context Fix**: Fixed right-click context menu crash by ensuring safe pointer validation on selected tree index.
- **Tooltip Realignment**: Realigned context menu tooltip coordinates to track mouse cursor coordinates exactly.
- **HUD Active State Indicator**: Added conditional telemetry text displaying "Moving" or "Rotating" in the HUD only during active manipulation.
- **Natural Light Restore**: Removed bright red directional/ambient lighting overrides from `renderer_objects.cpp` and restored standard white natural lighting.
- **GLB Translation Alignment**: Corrected coordinate transformation matrices for GLB model rendering (reconciling Y-up to Z-up model coordinates).
- **Scale Glitch Resolution**: Resolved scaling mismatch glitches by removing redundant scaling multiplications in `renderer_objects.cpp`.

### QSC/QVM Pipeline Enhancements
- **Compiler Repairs**: Repaired template-arguments compiler errors and duplicate declarations at the end of `app.cpp`.
- **Scale Getter Signature**: Restructured `App::GetSelectedObjectScale()` function signature to return correct scale parameters.
- **Matrix Syntax Correction**: Restored proper rotation matrix index syntax in `app.cpp` line 2270 to ensure error-free builds.
- **Automated Backup Reset**: Added automatic level reset logic restoring `objects.qsc` and `objects.qvm` from read-only backups upon command.
- **Implicit Node Filtering**: Prevented duplicate-node serialization errors in QSC by refining implicit static node hierarchy filtering.

---

## BETA 0.0.8 - Cutscene Graph Area Protection & AppData Migration
### AppData & Migration Integration
- **Automatic AppData Sync**: Implemented automatic local `QEditor` to `%APPDATA%\QEditor` migration and synchronization logic in `utils.cpp` (`ValidateAndSetupQEditor`).
- **Pristine Backup Guard**: Created exclusions for read-only `QFiles` folder during `AppData` folder copying to protect pristine level configuration backups.
- **Config Path Resolution**: Replaced legacy config directory resolution to consistently target local executable directory using `GetExeDirectory()`.
- **Level Texture Resolution**: Resolved level texture resolution failures by mapping local GLB path references directly to `AppData` textures folders.
- **Altitude Override Guard**: Implemented dynamic coordinate Z-snapping bypass pipeline to keep specific critical actors at their defined altitudes.

### Cutscene Graph Area Protection
- **Graph Area JSON Mapping**: Implemented a custom JSON parser for mapping Graph Area details from `graph_area_levelX.json`.
- **Graph ID Conversions**: Added automatic parser logic that converts visual graph IDs (e.g., `Graph #1` -> integer `1`) for precise matching.
- **Parser Pipeline**: Created the `LoadCutsceneGraphIds` pipeline inside `app.cpp` to parse Graph Area mappings on load.
- **Static Actor Protection**: Flagged all AI models located in "Cutscene" graph zones to preserve author-defined orientations.
- **AI Rotation Bypasses**: Added a static rotation override guard to skip standard yaw adjustments (`0.0` -> `6.28318`) for cutscene actors.
- **Ground Snapping Bypasses**: Prevented terrain-snapping modifications for designated cutscene units in `SnapObjectsToTerrain()`.
- **AI Sync Bypasses**: Bypassed position synchronization and metadata overrides inside `LoadAIModelsFromFolder()` for cutscene actor IDs.

---

## BETA 0.0.6 - Level 14 & GLB Pipeline
### Level Support
- **Full Level 14 Support**: Completed terrain and object support for Level 14 (The Maivon Compound).
- **Pre-defined Object Library**: Integrated the 3DEditor's pre-defined objects collection for rapid level assembly.

### Asset Management
- **GLB Model Format**: Migrated to OpenGL-friendly GLB format, combining models and textures into single binary files for faster loading and better compatibility.

---

## BETA 0.0.5 - IGI 2 Controls & Splines
### User Interface
- **IGI 2 Controls System**: Fully implemented the IGI 2 style control scheme for object manipulation (drags, keyboard modifiers).
- **Enhanced Manipulation**: Refined 6-DOF controls for smoother object placement.

### Foundation
- **Spline System Foundation**: Introduced the foundational logic for spline paths used by tracks and fences.
- **Level Loading Fixes**: Resolved critical crashes when switching between different game levels.

---

## BETA 0.0.4 - Clean Persistence & QSC Integrity
### Object Serialization Stabilization
- **Intentional Change Tracking**: Introduced a `modified` flag for all level objects. The editor now only saves changes for objects you've actually interacted with (moved, rotated) or that were updated during an AI synchronization.
- **QSC Data Preservation**: Fixed "Sticky Snapping" by decoupling the visual terrain snap from the saved QSC coordinates. The original file position is now preserved unless explicitly moved by the user.
- **Minimal Diffs on Save**: By using the `modified` flag, the editor now performs minimal line rewrites in `objects.qsc`, keeping the rest of the file's formatting, indentation, and floating-point precision intact.

### Manual Manipulation & Snapping
- **Manual Snap Guard**: Integrated the underground check into the manual snap key ('S') and reset key ('Space') to prevent accidental displacement of structural components during editing.

---

## BETA 0.0.3 - AI Logic & Stabilization
### AI Logic Fixes
- **Fixed AI Rotation Over-match**: Resolved a critical issue where objects containing "ai" in their name (e.g., "Chair", "Container") were incorrectly identified as AI units and had their rotation forced to 360 degrees.
- **Improved Type Safety**: The editor now explicitly identifies object types (`Building`, `EditRigidObj`, `HumanSoldier`) during the load phase to ensure level-specific rules apply only to the correct units.

### Structural Persistence
- **Robust "Joint Fixer" Exclusion**: Enhanced `IsUndergroundModel` logic with broad keyword support and string normalization to prevent structural models (tunnels, junctions, joint fixers) from snapping to the terrain.
- **Refined Matching**: Implemented case-insensitive trimming and normalization for model names and IDs to ensure matches are found even with inconsistent QSC string formatting.

---

## BETA 0.0.2 - MVP Demo (3 Days Sprint)

### IGIPath Resolution Fix
- **Fixed `IGI_GAME_PATH` placeholder issue**: `GetLevelQVMPath()` now correctly reads `IGIPath` from the executable directory's `config.ini` instead of using a relative path that was picking up placeholder values from the project root
- **Added detailed path logging**: New log entry shows the resolved IGIPath and the config file path being used: `[App] GetLevelQVMPath using IGIPath: D:\IGI1 (from config: D:\...\config.ini)`
- **Root cause**: Was using `.\config.ini` (relative to current working directory) instead of exe directory path

### Compiler/Decompiler Folder Cleaning
- **Automatic input/output folder cleanup**: Both `Compiler::Compile()` and `Decompiler::Decompile()` now clean their respective input and output directories before operations
- **Prevents stale file errors**: Removes old QSC/QVM files that could interfere with new compilation/decompilation operations
- **Detailed cleanup logging**: Reports number of items removed from each directory

### Config Loading Improvements
- **Exe-directory config resolution**: All config operations now consistently use the executable directory path via `GetExeDirectory()`
- **Eliminated config path ambiguity**: No more confusion about which config.ini is being read (project root vs exe directory)

### Technical Changes
- Modified `GetLevelQVMPath()` in `app.cpp` to construct full config path using `exeDir + "\\config.ini"`
- Added comprehensive logging in `GetLevelQVMPath()` to track IGIPath resolution
- Added folder cleaning logic in `compiler.cpp` (lines 54-69) and `decompiler.cpp` (lines 43-58)
- All config paths now resolve to: `<exe_directory>\config.ini`

### Build System
- Both Debug and Release configurations build successfully with MSVC 2022
- No breaking changes to existing functionality

---

## BETA 0.0.1

### QSC Save Logic Improvements
- **Added `original_rot` tracking**: Store original rotation values from QSC during load to enable proper change detection
- **Change detection on save**: Only update objects whose position or rotation has genuinely changed compared to their originally loaded state
- **Terrain snap offset handling**: Added `snap_z_offset` field to track Z offset added by terrain snapping, subtracted during save to match game coordinates
- **Multi-line Building support**: Fixed `Task_New` line replacement for Buildings with child tasks (no closing paren on first line)
- **Snap-to-terrain sync**: After snapping objects to terrain, sync `original_pos.z` so snap-only changes don't count as user edits
- **Preserved formatting**: Maintain original line indentation and terminators (`;` or `,`) for multi-line blocks

### Key Fixes
- Fixed issue where all `EditRigidObj` entries were being rewritten after reset and save
- Fixed Z-axis position discrepancy between editor view and in-game (editor was saving snapped Z, game expects raw terrain Z)
- Fixed compiler errors caused by incorrect multi-line `Task_New` termination
- Improved float formatting with `%.10g` to preserve precision and avoid scientific notation

### Technical Details
- Modified `LevelObject` struct to include `original_rot` and `snap_z_offset` fields
- Updated `LevelObjects::Load()` to capture original rotation values from QSC arguments
- Updated `App::SnapObjectsToTerrain()` to store snap offset and sync original Z
- Updated `LevelObjects::SaveToQSC()` with comprehensive change detection logic
- Added detailed logging for skip/save operations during QSC save

### Compilation
- Successfully builds with MSVC 2022 (Visual Studio)
- No breaking changes to existing functionality
## 2026-09-05 — Single-session smart live verification

- Added `Invoke-SmartSingleSession.ps1`, which builds one level scenario that
  launches `igi1ed.exe` once, selects every eligible named model object, takes
  ten deterministic orbit screenshots per object, verifies authored position /
  orientation and live texture assignment evidence, and closes once.
- Updated `Run-SmartTest.ps1` to use the single-session runner by default for
  one level or levels 1–14. `-LegacySerial` retains the prior per-object matrix
  for comparison only.
- Added `tests_run.cmd` with compact modular arguments such as
  `tests_run --level 5 --objects rigid --maximum 3`, plus `--all-levels`,
  `--object-type`, `--views`, `--all-objects`, and `--prepare-only`.
- Anonymous `-1#...` records are reported as skipped because the editor's
  find-by-ID control cannot select them; they are never counted as visual passes.
