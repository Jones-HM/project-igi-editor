# Foreign-level model texture repair implementation plan

> Planning only. Do not implement or deploy as part of this document-writing request. During implementation, use executing-plans or subagent-driven-development task by task, with regression and live evidence at each gate.

**Goal:** Export/import `001_02_1` into a foreign level and preserve its correct material assignment, texture bytes, UV appearance, and persistence after reopening.

**Architecture:** Resolve an explicit source asset bundle before changing destination archives. Validate ordered material mappings and rendered appearance independently of loader success or the existing color heuristic.

**Tech stack:** C++17, OpenGL, GoogleTest, PowerShell, bundled igi1conv, x86 Windows editor.

**Spec:** User request: detailed plan for the remaining `001_02_1` foreign-level texture defect, with no implementation. Companion plan: `2026-09-09-level12-winchhouse-cable.md`.

## Constraints and evidence

- Baseline inspected: `e2e-live-testing`, commit `02251bd`; working tree initially clean.
- Read `docs/game_data_types.md`, `game_file_formats.md` (MEF/TEX/DAT/MTP/RES and FNT sections), `game_model_naming.md`, `game_structure.md`, and `LIVE_E2E_TESTING.md`. The older names parameter_data_types.md, format_fnt.md and file-formats.md are not present; use current equivalents. Verify actual IGI1 paths rather than assuming the IGI2 layout in parts of the documentation.
- Production code must not special-case model `001_02_1`, level 1, or a test query. Corpus IDs belong in fixtures and E2E inputs only.
- Preserve the installed corpus; perform export mutation tests in an isolated copy with validated paths. Use WMI for interactive editor/game launches and `D:\IGI1` as the working directory for the installed game. Do not move startup cameras or modify authored placement to improve screenshots.
- Prior live captures at Level 1/task 0 have 13 materials and bind part 8 to material slot 7, texture `004_13_1`. All texture loads succeeded, but appearance differed from expectations.
- Commit `02251bd` changed the color-ratio threshold from 0.10 to 0.20 and added a test using 0.161255. This changes acceptance, not rendered pixels. The earlier assertion that quantization caused that ratio was not proved by a CPU/GPU pixel comparison. Do not treat this commit or its green E2E as proof of a texture repair.
- `001_01_1` and `001_02_1` are different variants. Comparing those two models is not a correct texture reference. Compare `001_02_1` against itself in its verified source level.

## File map: where to change

| File / symbol | Current concern | Proposed responsibility |
|---|---|---|
| `source/renderer/renderer_objects_texture.cpp`, `GetLevelTextureDatPath`, `EnsureGlobalTextureMapLoaded`, `GetTextureIdsForModel` | Extracted DAT overrides installed DAT; global map overwrites earlier levels with later ones | Deterministic mapping from selected source, exact variant before fallback |
| `source/renderer/renderer_objects.cpp`, `FindTextureData`, `RefreshTextureResCache` | First indexed same-name entry wins; suffix fallback accepts numeric suffixes | Explicit archive provenance and only recognized format suffixes |
| `source/renderer/renderer_objects_atta.cpp`, `AddModelToLevelRes` | Models written before texture/DAT/MTP success; existing DAT entries can remain wrong; later failures log warnings | Preflight full bundle, stage, verify, publish together or restore backups |
| `source/renderer/renderer_objects_mesh.cpp`, `GetOrLoadMesh`, `GetOrLoadSkinGeometry` | Mesh resolution and mapping resolution can use different sources | Preserve source provenance with resolved mesh |
| `source/renderer/renderer.cpp`, `DrawSkinnedMesh` | Per-slot bindings and fixed-function state need comparison with static path | Fix only if binding/UV/state capture demonstrates divergence |
| `source/renderer/renderer_objects_picking.cpp`, `CaptureObjectVisualEvidence` | Whole-texture chroma is compared against visible UV subset | Add texture/readback provenance; replace false appearance inference |
| `source/visual_integrity.h/.cpp`, `tests/test_visual_integrity.cpp` | Threshold PASS can conceal mismatch | Source-sampled evidence or explicit INCONCLUSIVE when comparison unavailable |
| New `source/renderer/model_texture_resolution.h/.cpp`, `tests/test_model_texture_resolution.cpp` | No isolated source-selection contract | Pure resolution policy used by importer and renderer |
| `CMakeLists.txt` | Explicit source lists | Add new production/test files to editor and test targets |
| New `tools/e2e/Test-ForeignModelTextures.ps1` | Existing run does not prove a fresh export/reopen | End-to-end source/destination comparison with artifact hashes |

Line numbers drift: locate these exact symbols with `rg -n` before editing. Do not refactor unrelated rendering or import UI.

## Task 1: establish the real failing export and reference

- [ ] Record HEAD, diff, architecture, editor SHA-256, converter hash and input file hashes. Inspect active compiler processes before starting a build; one build owner only.
- [ ] Locate exact `001_02_1` entries across source DAT/MTP and model archives. Identify which source supplied the imported MEF. Compare ordered texture lists, not sets. If multiple same-name assets have different hashes, record the conflict and source selection explicitly.
- [ ] Decode source and destination TEX with the bundled converter into disposable output. Compare decoded RGBA as well as container hashes; inspect all 13 materials, not just part 8.

```powershell
& .\assets\editor\tools\igi1conv\igi1conv.exe tex to-png `
  D:\IGI1\content\textures\level1\004_13_1.tex `
  -o .\artifacts\texture-reference-004_13_1.png
git rev-parse HEAD
Get-FileHash .\bin\Release\igi1ed.exe -Algorithm SHA256
```

- [ ] Capture the exact same model and pose before export in the source level, immediately after import, and after a cold destination reopen. Freeze animation and normalize camera relative to bounds. Also capture the ordinary animated draw if used by that task type.
- [ ] Make a material ledger: model hash, source archive, materialSlot, ordered DAT ID, MTP texture ID, TEX hash, decoded dimensions, GL texture ID, GPU readback hash, UV range, normal-pass draw count.
- [ ] Distinguish three outcomes: wrong source bytes/mapping; correct bytes but bad draw/UV; correct draw but false verifier. Only a failing comparison on the user's export path advances to a repair claim.

**Deliverable:** `artifacts/e2e/foreign-model-textures-<timestamp>/baseline.json` plus source/destination screenshots and per-material ledger. Do not manufacture a failing texture case by weakening or strengthening a threshold.

## Task 2: make source selection deterministic

Create this small pure seam, adapting calls to existing DAT/RES readers without moving all resource management:

```cpp
struct ModelTextureSource {
    int level;
    std::string modelId;
    std::vector<std::string> textures; // ordered; slot index is meaningful
};

// Null means no exact source match. Caller must report missing mapping.
const ModelTextureSource* FindExactTextureSource(
    const std::vector<ModelTextureSource>& sources,
    int sourceLevel, const std::string& modelId) {
    for (const auto& s : sources)
        if (s.level == sourceLevel && s.modelId == modelId) return &s;
    return nullptr;
}
```

- [ ] First write the following red test and connect the eventual function to the real importer. A standalone helper unused by production does not close the regression.

```cpp
TEST(ModelTextureResolution, KeepsSelectedVariantAndMaterialOrder) {
    const std::vector<ModelTextureSource> sources{
        {2, "001_02_1", {"face_b", "vest_b", "trousers_b"}},
        {1, "001_01_1", {"face_a", "vest_a"}},
        {9, "001_02_1", {"different_face", "different_vest"}}
    };
    const auto* chosen = FindExactTextureSource(sources, 2, "001_02_1");
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->textures,
              (std::vector<std::string>{"face_b", "vest_b", "trousers_b"}));
    EXPECT_EQ(FindExactTextureSource(sources, 3, "001_02_1"), nullptr);
}
```

- [ ] Import with explicit source level/provenance; never let destination's stale exact entry become the source bundle. For display of an already resident model, destination's verified entry remains authoritative.
- [ ] Use exact ID first. For missing LOD metadata, consult the same variant primary (`NNN_VV_1`) only after verifying the LOD shares the mapping. Do not collapse variants by numeric prefix as a repair.
- [ ] Replace `FindTextureData`'s generic trailing lowercase/digits test with the recognized suffix list already present in `StripTextureFormatSuffix`. Numeric `_1` is part of identity, not a format suffix. Add cases for `004_13_1`, recognized alpha suffixes, missing exact ID, and conflicting archives.
- [ ] Return source path/hash with bytes so imports and GPU diagnostics can prove the selected entry. Resolve explicit source archive first; fallback must be recorded, not silent.

## Task 3: persist a complete bundle and repair stale destination metadata

- [ ] In `AddModelToLevelRes`, collect all family/attachment MEFs and their ordered texture mappings before the first destination write. Restrict family selection by actual dependencies and verified LOD family; do not assume every same-prefix variant is required.
- [ ] Reject unresolved required textures before writing. Validate every MEF material slot against its mapping; distinguish documented shared/attachment mapping from an accidental modulo wrap.
- [ ] Replace append-only handling for the explicitly imported model with an upsert. Keep unrelated entries and their order intact. Existing correct entries are no-ops; existing conflicting entries for the chosen import are replaced with the chosen source mapping.

```cpp
// Inside the familyModels loop, after source provenance is fixed:
const auto* source = FindExactTextureSource(sources, sourceLevel, fm.first);
if (!source) return false;
auto existing = std::find_if(dat.models.begin(), dat.models.end(),
    [&](const auto& entry) { return entry.modelName == fm.first; });
if (existing != dat.models.end()) {
    if (existing->textures != source->textures) {
        existing->textures = source->textures;
        anyAdded = true; // rename to mappingsChanged in this touched block
    }
} else {
    bool present = false;
    DAT_AddModel(dat, fm.first, source->textures, present);
    anyAdded = true;
}
```

- [ ] Stage models RES, textures RES, DAT and regenerated MTP under a unique sibling staging directory. Use the existing bundled `dat to-mtp` conversion on staged paths. Reparse each staged file and verify slot order/byte hashes before publishing.
- [ ] Keep per-operation backups and a journal listing exact destination paths. If any publish fails, restore already replaced files and report failure; never return true after a required DAT/MTP conversion error. Do not claim a multi-file filesystem rename is atomic. Test forced conversion and second-file replacement failures.
- [ ] Invalidate destination mesh, skin geometry, texture GL cache, DAT cache and archive indexes only after successful publication. Verify ownership before deleting GL IDs. A retry after failure must reload restored data.
- [ ] Test idempotent import, wrong existing mapping, duplicate texture filenames across levels, alpha suffix preservation and cold reopen with the source-only fallback unavailable to the resolver. That last test proves export completeness.

## Task 4: repair draw or verifier only where measured

- [ ] If decoded source differs from GPU readback, inspect `GL_RegisterTexture`, pixel unpack state, row pitch and decoder channel ordering before changing material mappings.
- [ ] If upload is correct but draw selects the wrong object, capture `GL_ACTIVE_TEXTURE`, `GL_TEXTURE_BINDING_2D`, program and sampler per failing draw. Explicitly bind unit 0 and reset fixed-function secondary texture-unit state when entering the skinned path; restore captured caller state afterward. Keep static/animated UV conventions consistent using an asymmetric colored-corner fixture.
- [ ] If pixels match the source, remove the unsupported whole-texture-to-whole-output chroma assertion. A gray UV patch of a colorful atlas is legitimate, as are dark lighting, occlusion and mipmap blending. Raising 0.10 to 0.20 is not a calibrated repair.
- [ ] Introduce a neutral-light textured reference pass using the same geometry, UVs, pose, texture bindings and visibility as the normal pass. Serialize the comparison inputs, shader policy and tolerances. When that reference is absent, record appearance as INCONCLUSIVE rather than silently PASS.

Proposed comparison contract (new fields/types, not existing APIs):

```cpp
enum class AppearanceResult { Pass, Fail, Inconclusive };
AppearanceResult CompareReferenceRgb(
    const std::vector<unsigned char>& expected,
    const std::vector<unsigned char>& actual,
    const std::vector<unsigned char>& visibleMask,
    unsigned channelTolerance) {
    if (visibleMask.empty() || expected.size() != visibleMask.size()*3 ||
        actual.size() != expected.size()) return AppearanceResult::Inconclusive;
    size_t visible = 0, mismatched = 0;
    for (size_t p = 0; p < visibleMask.size(); ++p) {
        if (!visibleMask[p]) continue;
        ++visible;
        bool bad = false;
        for (size_t c = 0; c < 3; ++c)
            bad |= std::abs(int(expected[3*p+c])-int(actual[3*p+c])) >
                   int(channelTolerance);
        mismatched += bad;
    }
    if (!visible) return AppearanceResult::Inconclusive;
    return mismatched ? AppearanceResult::Fail : AppearanceResult::Pass;
}
```

Use exact comparison for CPU fixtures. Calibrate live masks to exclude raster edges and derive GPU tolerance from repeated same-input captures, then pin it before evaluating the fix. Never compare differently lit source/destination screenshots with this function directly. Add neutral atlas patch, strong missing diffuse, UV-flip, wrong-variant and absent-reference tests.

## Task 5: build and perform the actual export E2E

- [ ] Build editor and tests once with a validated x86 configuration. Await completion and check executable timestamp/hash and test registration. A zero-test filtered run is failure of validation.

```powershell
cmake --build build --config Release --target igi_tests igi-editor --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& .\bin\Release\igi_tests.exe --gtest_filter='ModelTextureResolution.*:VisualIntegrityTest.*' --gtest_color=no
if ($LASTEXITCODE -ne 0) { throw 'Regression tests failed' }
```

- [ ] Implement `Test-ForeignModelTextures.ps1` with required `SourceLevel`, `DestinationLevel`, `ModelId`, `GameRoot`, `EditorExePath`, `ArtifactsRoot`. Resolve every path, reject input/output overlap, capture manifest from current destination QVM, require exactly one selected test task. Never patch a stale inventory manually.
- [ ] Exercise the actual import entry point (`AddModelToLevelRes` via current CLI/UI), insert a disposable test placement through supported editor workflow, save explicitly, capture, close, reopen, capture again. Record all mutated files and restore/verify their hashes if the installed corpus was used.
- [ ] Run source level and foreign destination cases; use Level 1 for the existing placement only after verifying it is foreign relative to the selected model source. Also use a destination that initially lacks the model and a destination with an intentionally wrong mapping in a disposable fixture.
- [ ] Use native WMI launch, SessionId 1, Responding true and >30 MB. Capture at least the existing 12 exterior + 4 interior diagnostic views; report ordinary visible RGB separately from diagnostic false colors.
- [ ] Extend `Analyze-VisualIntegrityBundle.ps1` and `Test-SmartCaptureArtifact.ps1` to check the new appearance evidence independently. The existing structural analyzer can PASS while sourceRecordedStatus is FAIL; its status alone cannot establish correct textures. Check ignored tools into version control intentionally during implementation (`git check-ignore -v` first).

## Acceptance and handoff

All requirements must pass: demonstrated export failure before repair; correct source bytes and ordered destination mappings; correct neutral-reference appearance; normal editor screenshot inspected; cold reopen retains textures without foreign fallback; negative wrong-variant/missing-texture controls rejected; no level corruption; exit/restoration verified. Link before/after evidence, tests, binary/source hashes, source and destination levels, and the actual causal code change. Record bug `IGIED-FOREIGN-MODEL-TEXTURES-001` in mem0 only after verified resolution, per user instruction. Do not record this plan as a fixed bug. Commit/push implementation only under the execution request's publication scope.
