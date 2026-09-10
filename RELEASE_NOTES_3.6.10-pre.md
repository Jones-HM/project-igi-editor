# Project IGI Editor 3.6.10-pre

This pre-release bundles the editor workflow, game-font/menu fixes,
sniper/AI rendering improvements, foreign-model source provenance repair, and
the associated regression/E2E tooling.

Highlights:

- Game font fallback and retail-style menu rendering, hit testing, save, and
  autosave behavior.
- Sniper/AI skinned-model rendering, authored rotations, animation fallback,
  nested HumanAI discovery, and graph-target handling.
- Cross-level model, texture, and ATTA dependency resolution with exact source
  provenance and ambiguous-source rejection.
- Staged, validated, rollback-safe RES/DAT/MTP import publication.
- Visual-integrity, F11 framing, weather/loading, OpenGL-state, and large-level
  lightmap stability improvements.

Validation note: focused model-source tests and the explicit-source disposable
WMI Session 1 import passed. The full suite still reports corpus/path failures,
and the importer’s observed process memory was below the required 30 MB gate.
This is a pre-release candidate, not a stable-release claim.

See `docs/ISSUE_FIXES.md` for the durable fix record and safe future-release
procedure.
