# Known Issues and Engine Limitations

This document tracks all known rendering bugs, engine limitations, and editor work-in-progress features for the Project IGI Editor.

---

## 🏔️ 1. Rendering and Model Issues

### 🎥 Camera Orientation
* **Symptom**: Camera orientations in certain views or cutscenes do not align correctly with the expected angles.
* **Root Cause**: Discrepancies in yaw/pitch/roll representations and matrix multiplication orders between the editor's camera viewport and the game's internal camera structures.

### 📏 Wire Width
* **Symptom**: Fence wires and secondary wire meshes render with incorrect thicknesses or fail to scale dynamically based on viewport distance.
* **Root Cause**: Platform line-width rendering limits and lack of shader-based variable wire width scaling.

### 🧍 Dual-vertex-set character models — wrong textures on parts (editor viewport)
* **Symptom**: A few AI/character models (notably `001_02_1`) render with textures on the wrong body parts in the **editor viewport** — e.g. a clothing/sign texture appears on the face/neck/hands. In-game these render correctly (editor-preview only). Most character models (`004_02_1`, `008_01_1`, `009_01_1`, `013_01_1`) are unaffected.
* **Root Cause**: The model→texture mapping is correct — the MEF's `TAMC` material chunk defines block *i* → texture *i*, and the editor honors that. The problem is **geometry parsing**: these models carry two render/vertex sets (`XTVC0/ECFC0/TAMC0` + `XTVC1/ECFC1/TAMC1`), and the binary-MEF parser splits triangles/vertices into render blocks differently than the model intends, so correct textures land on the wrong geometry. A safe fix needs a reference for the correct per-block geometry (e.g. the original IGI `dconv` OBJ export) to validate against the models that already render right.
* **Note**: Unrelated to the `001_02_1` *import* texture bug
  (`docs/ISSUE_FIXES.md` → `IGIED-FOREIGN-MODEL-TEXTURES-001`, fixed and
  verified: common-archive source bundle + session-staleness + RES replace;
  E2E `001_02_1` into levels 1–5 all PASS with 13/13 byte-identical textures).
  The remaining item here is editor-viewport geometry only.

---

## 1b. 32-bit address-space ceiling and x64 feasibility

* **Symptom**: Lightmap auto-load and other large contiguous allocations (200+ MB `.res` archives, many `.olm` lightmap textures) can exhaust the 32-bit process address space, especially after in-session level switches fragment it. Level 3+ with lightmaps on historically crashed here, which is why `QEDLightmaps` defaulted to OFF.
* **Root Cause**: The editor builds only as a Win32 (`-A Win32`) process, capping usable contiguous memory near ~2 GB. The `.res` streaming reader (`RES_ForEachEntry`) and the `.olm` lightmap loader (`olm_texture.cpp`) already read chunk-by-chunk to keep peak memory to one entry/texture, which removes the crash without changing the build target.
* **x64 feasibility spike**: An x64 (`-A x64`) build is viable at the build-config level: `CMakeLists.txt` already selects `third_party/GL/lib/x64` (freeglut/glew32 `.lib`) and `assets/dlls/x64` runtime DLLs when `CMAKE_SIZEOF_VOID_P == 8`, and the freeglut+GLEW+OpenGL32 toolchain is architecture-agnostic. The remaining porting work is auditing pointer-width assumptions (file offsets cast to `int`, `DWORD`-sized `tellg` results, hand-packed struct reads) before flipping the default. Not implemented in this issue; tracked as a future structural fix.
* **Note**: With the streaming `.olm` loader, `QEDLightmaps` no longer needs to stay OFF purely to avoid crashing level 3; it remains OFF by default as a conservative opt-in, and the Escape-menu checkbox turns it on without crashing.

## 🛤️ 2. Splines and Placements

### 📍 Missing Position
* **Symptom**: Certain objects or entities are loaded without valid coordinates, defaulting to the map origin or failing to render.
* **Root Cause**: Unmapped coordinate offsets in the parsed binary QSC/QVM level files for specific object classes.

### 🎬 Train Placements in Cutscenes
* **Symptom**: Trains or railway carriages appear misplaced, misaligned, or floating during cutscene playback.
* **Root Cause**: The engine uses hardcoded spline overrides and coordinate offsets specifically during cutscenes, which are not currently synchronized with the editor's standard object placement.

---

## 📢 3. Reporting Other Bugs and Issues

If you encounter any other bugs, crashes, or rendering issues that are not documented here, please feel free to report them to the dev team!

* **🎮 Discord**: Message us directly at `Jones_IGI#3954` or join the modding community on the [Project IGI Discord Server](https://discord.com/invite/QpbQrRFAER).
* **📧 Email**: [DEFAULT_RECIPIENT_EMAIL](mailto:DEFAULT_RECIPIENT_EMAIL)
* **🌟 GitHub**: Create a detailed issue on the [project-igi-editor GitHub Repository](https://github.com/Jones-HM/project-igi-editor/issues) with reproduction steps and logs.
