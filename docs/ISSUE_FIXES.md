# Issue Fixes and Safe Release Notes

This file is the single issue-history file promoted to `develop`. Keep future
bug fixes here with the symptom, root cause, implementation, verification, and
remaining release gates. Do not record a bug as fully fixed from a focused unit
test alone.

## IGIED-FOREIGN-MODEL-TEXTURES-001

Status: implementation fixed; final release gate pending.

### Symptom

Importing a model from another level could bind same-named textures from the
destination or an unrelated level. Character parts could therefore receive the
wrong appearance, and an import could report success without a complete
model/DAT/MTP publication.

### Root cause

Model IDs and texture IDs are not globally unique asset identities. The old
resolver used loose fallbacks and destination state, so it could select the
wrong level-local mesh or texture bundle. Existing RES entries also were not
replaced when their bytes differed.

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

### Verification

- `igi_tests`: 5/5 `ModelTextureResolution.*` tests passed.
- Release x86 build of `igi_tests` and `igi1ed.exe` passed.
- Disposable WMI Session 1 explicit-source import passed with matching model
  bytes, texture bytes, and ordered DAT mapping; the installed `D:\IGI1`
  corpus was not modified.
- Disposable automatic-source negative control rejected divergent bundles and
  made no destination mutation.

### Release gate still required

The observed CLI importer process was only 3.6 MB, below the required 30 MB
runtime gate. The full test executable also reported 39 existing corpus/path
and verify-level integration failures. Do not publish a release until those
two gates are re-run and resolved or explicitly waived by the maintainer.

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
