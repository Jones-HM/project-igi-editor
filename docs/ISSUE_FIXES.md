# Issue Fixes and Safe Release Notes

This file is the single issue-history file promoted to `develop`. Keep future
bug fixes here with the symptom, root cause, implementation, verification, and
remaining release gates. Do not record a bug as fully fixed from a focused unit
test alone.

## IGIED-FOREIGN-MODEL-TEXTURES-001

Status: fixed and verified (import path). Release binary deployed to `D:\IGI1`.

### Symptom

Importing a model from another level could bind same-named textures from the
destination or an unrelated level. Character parts could therefore receive the
wrong appearance (notably Sniper `001_02_1`), and an import could report
success without a complete model/DAT/MTP publication.

### Root cause

Model IDs and texture IDs are not globally unique asset identities. The old
resolver used loose fallbacks and destination state, so it could select the
wrong level-local mesh or texture bundle. Two concrete defects:

1. The `001_02_1` forensics: level `.res` archives are polluted with
   same-name entries from other levels, while the authoritative bytes live in
   the shared `common` archive (`missions/location0/common/.../location0.res`).
   `FindTextureDataFromLevel` / `FindMeshDataFromLevel` searched only the level
   archive, so a lookup for a texture that exists only in `common` missed its
   own source level and fell through to same-name bytes from an unrelated
   level's archive.
2. Session staleness: after publishing an import, `texture_sources_` still held
   the stale pre-import mapping, so a later import in the same session resolved
   the model to the wrong level's archive. Existing RES entries also were not
   replaced when their bytes differed.

3. Import skip on natively-present destinations: `CommitPropTextEdit` called
   `ModelSourceRequiresImport` first, so committing a model field whose model
   already exists in the destination inventory (e.g. `001_02_1` is native to
   level 12) silently skipped the import — which is why levels 1/2/9 worked
   while level 12 appeared to do nothing. An explicit Enter on the model field
   is now always an import request; the staged replace path skips
   byte-identical entries, so refreshing a natively-present model is a safe
   no-op when bytes already match. If the import fails but the model exists
   natively, the commit falls back to the reference change instead of flagging
   the model as missing.

### Resolution

- Track exact `(model ID, source level)` provenance through the picker and CLI.
- Resolve ordered DAT texture mappings from the selected source level.
- Load mesh and texture bytes from that same source level only.
- Reject an untagged import when candidate source bundles differ in mapping,
  mesh bytes, or texture bytes; require explicit source provenance.
- Validate MEF material slots against the ordered mapping before publication.
- Stage and validate model RES, texture RES, DAT, and regenerated MTP output,
  then publish them together with rollback backups.
- Replace differing same-name RES entries instead of silently retaining stale
  destination bytes.
- Resolve cross-family ATTA dependencies recursively before publication.
- Make the E2E harness fail on an unobserved or nonzero importer exit and enforce
  the required 30 MB process-memory gate.
- Include the shared `common` archive in the source bundle: `FindTextureDataFromLevel`
  / `FindMeshDataFromLevel` now resolve `levelN.res` first, then
  `common/.../location0.res`, via the shared `FindModelSourceEntry` helper
  (level wins on name collisions; format-suffix fallback stays per-archive).
- Upsert `texture_sources_` (plus `texture_level_map_` and texture-cache
  invalidation) immediately after publishing, so later imports in the same
  session resolve the just-published destination bundle.
- Dedupe the model picker to one row per model ID (current level wins,
  otherwise smallest owning level), with direct Up/Down navigation and Enter
  committing the highlighted row's deterministic source level.

### Verification

- `igi_tests`: 13/13 `ModelTextureResolution.*` tests passed, including the two
  synthetic-`.res` integration tests (`LevelMissingFallsBackToCommonArchive`,
  `LevelArchiveWinsOverCommonArchive`) and the picker tests (dedupe,
  nav-preview, enter-commit, click).
- Release x86 build of `igi_tests` and `igi1ed.exe` passed.
- Disposable-copy explicit-source import of Sniper `001_02_1` (source level 2)
  passed into every destination level 1–5: matching model bytes, 13/13 matching
  texture bytes, and ordered DAT mapping equality per level
  (`D:\e2e-ai-swap-L1..L5-artifacts\report.json`, all `status=PASS`, exit 0).
- Disposable fresh-import control (`006_01_1`, level2-only → level1) passed with
  `destinationFilesChanged: [level1.dat, level1.mtp, models-level1.res,
  textures-level1.res]`.
- Disposable automatic-source negative control correctly rejected the divergent
  `001_02_1` bundle as ambiguous with no destination mutation; the installed
  `D:\IGI1` corpus was not modified during E2E.
- Level 12 regression (import looked like a no-op): disposable-copy explicit
  import of `001_02_1` (source level 2 → destination level 12) passed after the
  always-import fix — `status=PASS`, exit 0, 0 texture mismatches, and
  `destinationFilesChanged: []` (level 12's archives already carry
  byte-identical content, so the safe refresh was a no-op)
  (`D:\e2e-l12-alwaysimport2-artifacts\report.json`).

### Release gate still required

The full test executable reports pre-existing corpus-gated failures
(`DatParser/GraphParser/ResParser/TexParser FileExists*` — the checkout lacks
the installed mission corpus under `bin\Release\missions`), unrelated to this
change. The live picker scenario harness separately needs a UI-targeting fix
(it applied `463_03_1` instead of the filtered `003_02_1`).

## Safe future fix and release procedure

1. Reproduce the issue with a disposable copy; hash all protected inputs.
2. Add a general regression test without hardcoding a user query or special
   model ID in production logic.
3. Make the smallest source change, then run the focused test and a clean x86
   build.
4. Run the relevant live WMI Session 1 test with fresh logs, screenshots, file
   hashes, and mutation/rollback checks.
5. Run the full suite and record skips and failures separately from passes.
6. Update `CHANGELOGS.md`, review the complete diff, and only then create a
   versioned ZIP with binaries and a SHA-256 manifest.
7. Push the implementation branch first. Promote only this Markdown file to
   `develop` unless the maintainer explicitly requests more files.
8. Create or update the GitHub release only after authentication, remote
   checks, tests, and external review gates pass.
