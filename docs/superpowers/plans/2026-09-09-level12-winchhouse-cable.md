# Level 12 Winch House cable implementation plan

> Planning only. During implementation, use executing-plans or subagent-driven-development task by task. No code, game assets, camera defaults or installed binaries are changed by this planning request.

**Goal:** Correct the cable-car cable's initial shape and keep it stable and correctly occluded as the camera enters and passes the Level 12 Winch House.

**Architecture:** Verify the authored cable representation, derive camera-independent segment geometry from it, and establish all material/depth state at spline draw boundaries. Share the resulting geometry with picking and evidence so the test measures the actual cable.

**Tech stack:** C++17, GLM double precision geometry, OpenGL, GoogleTest, PowerShell native WMI E2E.

**Spec:** User's startup cable shape and camera-transition defect. Read `docs/level12-winchhouse-fix-plan.md` for the prior building-surface investigation; this cable plan is narrower and does not assume its proposed causes are confirmed.

## Interpretation, constraints and observed code

Interpret the incomplete description as: cable starts malformed and changes, disappears or snaps into a different appearance when entering/passing the house. Capture the transition to determine which behavior occurs. Do not assume camera position should alter cable geometry.

- Baseline `02251bd`, branch `e2e-live-testing`.
- Existing inventory `artifacts/level12-workflow-inventory.json` contains two four-waypoint splines around the cable route, with diagnostic identities `-1#144` and `-1#149`. Their waypoint model IDs include `320_01_1` and `320_02_1`. These ordinal identities are discovery hints only; regenerate from current QVM and inspect **both** waypoint model and segmentModelId because inventory may conflate them.
- First route starts near `(112255136, -42044152, 179558752)` and ends near `(112266360, -46481944, 179015280)`. Coordinates are large enough that early float conversion deserves testing.
- `renderer_splines.cpp::Draw` walks all `childrenIndices`, using the first nonempty `segmentModelId` as fallback. It does not first filter child types.
- `DrawSplineSegment` ignores `parent.linearSegments` and `parent.splineSegmentCount`, casts positions to float before subtraction, assumes local X is longitudinal, clamps local extent to at least 1, caps tiles at 64 and derives all tangents from neighboring positions.
- `DrawSplineSegment` sets only a subset of the shared object shader state. `DrawAttachmentsForSpline` executes opaque and transparent attachment passes between tiles; the next tile does not reestablish every uniform.
- The ordinary object shader has `u_alpha`, `u_baseColor`, `u_useLightmap`, tint and glass-related state. Inheriting a prior house/glass draw's state is a credible camera-dependent visibility cause, but remains a hypothesis.
- A separate `Wire` parser drops endpoint arguments 6–8 from typed fields, but the present cable inventory points to splines. Do not implement a Wire rewrite unless fresh task lineage proves the affected cable uses that branch.
- Do not disable depth testing, terrain occlusion, LOD globally, or move the initial camera outside the house. Do not hardcode `463_01_1`, `320_*` or Level 12 in production decisions.

## File map

| File / symbol | Required work if reproduction confirms the path |
|---|---|
| `source/level/level_objects.cpp`, spline branches | Parse authored spline mode and segment settings using current declarations; retain waypoint orientation and segment identity |
| `source/level/level_objects.h` | Store only missing authored spline fields; retain double positions |
| `source/level/task_schema.cpp`, `level_objects_serialize.cpp` | Keep field offsets and unchanged round-trip preservation aligned |
| New `source/renderer/spline_geometry.h/.cpp` | Pure segment basis, path sampling and transforms used by renderer and tests |
| `source/renderer/renderer_splines.cpp/.h` | Filter waypoint children, honor mode, use shared geometry, explicitly bind material state |
| `source/renderer/renderer_objects_atta.cpp`, `DrawAttachmentsForSpline` | Avoid state leaks; apply identical tile transform to child geometry |
| `source/renderer/renderer_draw.cpp` | Verify frame ordering, object visibility policy and camera-dependent state passed to splines |
| `source/renderer/renderer_objects_picking.cpp`, `debug_command_manager.cpp` | Capture spline segment IDs/transforms rather than isolated MEF only |
| New `tests/test_spline_geometry.cpp`; existing parser/serialization test files located via CMake | Degenerate, large-coordinate, authored-mode and round-trip cases |
| `CMakeLists.txt` | Register helper and meaningful tests in both required targets |
| New `tools/e2e/Test-Level12CableTransition.ps1` | Deterministic ordinary-scene camera replay and structural/depth assertions |

## Task 1: prove the representation and camera transition

- [ ] Read current Level 12 QVM by decompiling a copy into artifacts. Extract SplineObj and SplineObjWaypoint declarations, the two routes, WinchHouse, cable car and relevant attachments. Record argument tokens and parent-child order, not just task IDs.
- [ ] Extract the actual segment MEFs from their resolved archives; record local bounds, render blocks, axis, attachment transforms and LOD variants. Confirm whether cable longitudinal axis is X, Y or Z from geometry and authoring/reference evidence. Do not infer every model's axis from its longest AABB dimension alone.
- [ ] Capture the authored startup view, outside approach, doorway, valid interior, past-house view and reverse traversal. Record view/projection matrices, clip planes, viewport, selected task and draw flags. Freeze camera at each pose and repeat it after an unrelated glass object was drawn.
- [ ] Record per tile: source waypoint identities, endpoints, segment model/hash, model matrix, draw count, shader/sampler/alpha/lightmap state and projected depth. Use a temporary unique log prefix `[CABLE-TRACE]` and remove it before delivery.
- [ ] Classify the failure with this evidence:

| Observation | Primary code branch to test |
|---|---|
| Tile transforms change at the same authored state when only camera moves | Camera input leaking into geometry or parent/attachment LOD selection |
| Matrices stable, submissions stable, visible pixels change unexpectedly | Shared shader/GL state, depth or clipping |
| Initial transforms miss waypoint endpoints | Axis, bounds-origin, scale, interpolation or units |
| Far geometry disappears with constant shader state | Clip/frustum/LOD decision or legitimate occlusion; inspect depth before changing policy |
| Isolated model passes but cable fails | Missing spline evaluation in evidence path |

**Deliverable:** baseline route manifest and six-pose replay with raw screenshots. No production behavior fix until one branch explains the reproduced symptom.

## Task 2: isolate segment placement and preserve precision

Proposed helper interface in `spline_geometry.h`:

```cpp
struct SplineTile {
    glm::dvec3 begin, end;
    glm::dmat4 model;
};
std::optional<SplineTile> MakeXAlignedTile(
    glm::dvec3 a, glm::dvec3 b,
    double localMinX, double localLength, double crossScale);
```

Example implementation for the **verified local-X convention**; if measured local axis differs, precompose its explicit basis before the following transform rather than change the world route:

```cpp
std::optional<SplineTile> MakeXAlignedTile(
    glm::dvec3 a, glm::dvec3 b,
    double minX, double length, double crossScale) {
    const glm::dvec3 delta = b-a;
    const double span = glm::length(delta);
    if (!std::isfinite(span) || span <= 1e-9 ||
        !std::isfinite(minX) || !std::isfinite(length) || length <= 1e-9 ||
        !std::isfinite(crossScale) || crossScale <= 0) return std::nullopt;
    const glm::dvec3 forward = delta/span;
    const glm::dvec3 reference = std::abs(forward.z) < 0.99 ?
        glm::dvec3(0,0,1) : glm::dvec3(0,1,0);
    const glm::dvec3 right = glm::normalize(glm::cross(reference,forward));
    const glm::dvec3 up = glm::cross(forward,right);
    const double sx = span/length;
    glm::dmat4 m(1);
    m[0] = glm::dvec4(forward*sx,0);
    m[1] = glm::dvec4(right*crossScale,0);
    m[2] = glm::dvec4(up*crossScale,0);
    m[3] = glm::dvec4(a-forward*(sx*minX),1);
    return SplineTile{a,b,m};
}
```

- [ ] Write and run this red regression before replacing inline placement. Include `<optional>`, `<cmath>`, GLM and GoogleTest in their appropriate files.

```cpp
TEST(SplineGeometry, PlacesBothEdgesAtLargeAuthoredEndpoints) {
    const glm::dvec3 a(112255136,-42044152,179558752);
    const glm::dvec3 b = a + glm::dvec3(0.25,-1000,0.5);
    auto tile = MakeXAlignedTile(a,b,-2.0,4.0,40.96);
    ASSERT_TRUE(tile.has_value());
    auto first = glm::dvec3(tile->model*glm::dvec4(-2,0,0,1));
    auto last = glm::dvec3(tile->model*glm::dvec4(2,0,0,1));
    EXPECT_LT(glm::length(first-a),1e-6);
    EXPECT_LT(glm::length(last-b),1e-6);
}
TEST(SplineGeometry, RejectsZeroSpan) {
    EXPECT_FALSE(MakeXAlignedTile({0,0,0},{0,0,0},0,1,40.96));
}
```

- [ ] Add reversed, vertical, very short, nonzero local-origin and non-finite input cases. Confirm cross-section scale remains constant while length changes.
- [ ] Keep path subtraction/interpolation and model construction double precision. Convert at the existing GPU boundary only. If final view/model float cancellation still causes jitter, compute camera-relative model/view composition in double for both cable and reference geometry; do not move only the cable into a different coordinate space.
- [ ] Replace the artificial minimum localLength of 1 with explicit invalid-geometry handling. Honor genuine small geometry dimensions.

## Task 3: honor authored path mode and child order

- [ ] Parse spline flags against the actual QVM declaration; do not invent positional offsets. Fields `linearSegments` and `splineSegmentCount` already exist but must be checked for populated values and exact semantics.
- [ ] Build an ordered list of valid, nondeleted `isSplineWaypoint` child indices before computing neighbors. Do not connect through decorative child tasks. Keep disconnected/deleted waypoint behavior consistent with editor editing semantics and test it explicitly.
- [ ] Use authored linear mode to interpolate directly. Keep Hermite interpolation only for authored curved mode and verify whether waypoint orientation controls tangents. Do not apply both a derived tangent and an authored rotation without a defined frame convention.

```cpp
// In the geometry helper; p0/p1 and tangents are glm::dvec3.
auto sample = [&](double t) -> glm::dvec3 {
    if (parent.linearSegments) return p0 + t*(p1-p0);
    const double t2=t*t, t3=t2*t;
    return (2*t3-3*t2+1)*p0 + (t3-2*t2+t)*tan0 +
           (-2*t3+3*t2)*p1 + (t3-t2)*tan1;
};
```

- [ ] If curved cable needs uniform tiles, construct a double-precision cumulative arc-length table, invert distance by lower_bound/interpolation, and place tiles at equal distances along the curve. Preserve endpoints exactly. Derive subdivision/error limits from model radius and authored segmentation rather than a fixed 64-tile cap that silently stretches long spans.
- [ ] For adjacent curved tiles, retain a continuous transported frame if independent near-vertical reference selection creates roll jumps. Add a near-vertical bend test checking neighboring cross-section axes do not flip sign.
- [ ] Test parser -> geometry -> serialization: unchanged tokens round-trip; linear path remains linear; unrelated spline systems (rails and lift paths) retain their behavior.

## Task 4: eliminate draw-state dependence if reproduced

At each spline submesh submission, explicitly set the uniforms actually used by the shared object shader. Reapply after `DrawAttachmentsForSpline`, since transparent attachments can change state. Example opaque initialization, using real uniform names; preserve authored alpha per submesh instead of forcing glass opaque:

```cpp
glUseProgram(shader_program);
glUniform1i(glGetUniformLocation(shader_program,"u_useLightmap"),0);
glUniform1f(glGetUniformLocation(shader_program,"u_alpha"),1.0f);
glUniform4f(glGetUniformLocation(shader_program,"u_baseColor"),1,1,1,1);
glUniform1f(glGetUniformLocation(shader_program,"u_glassMin"),0.0f);
glActiveTexture(GL_TEXTURE0);
glBindTexture(GL_TEXTURE_2D,sub.textureID);
glUniform1i(glGetUniformLocation(shader_program,"u_texture"),0);
glUniform1i(glGetUniformLocation(shader_program,"u_useTexture"),sub.textureID!=0);
glEnable(GL_DEPTH_TEST);
glDepthFunc(GL_LESS);
glDepthMask(GL_TRUE);
glDisable(GL_BLEND);
glDisable(GL_POLYGON_OFFSET_FILL);
```

- [ ] Also reset the actual tint uniform found in `Renderer_Objects::Draw`; cache uniform locations after shader link, not once per tile. Carry legitimate parent lighting rather than inherit the last building draw.
- [ ] Route alpha cutout/blended cable submeshes using their authored material policy. Match parent/attachment scaling, depth-write and pass ordering. Retain normal scene occlusion.
- [ ] Capture caller GL state at the pass boundary and restore the states changed by this pass. Avoid unconditional restoration to guessed defaults.
- [ ] Add an offscreen GL regression: draw the same cable after opaque, glass, lightmapped and untextured predecessor draws. Pixel/ID/depth output must match for fixed camera and cable. Pure matrix unit tests cannot catch this defect.
- [ ] If the disappearing cable instead comes from clipping or culling, fix that measured bound/unit decision and keep the shader change limited to independently proven state leakage. Do not implement every hypothesis by default.

## Task 5: normal-view E2E and acceptance

- [ ] Implement `Test-Level12CableTransition.ps1` with `GameRoot`, `EditorExePath`, `ArtifactsRoot`, `RouteManifest` and `CameraPath` inputs. Derive target identities from current authored hierarchy and verify source hashes. Emit explicit failure if either expected route is missing.
- [ ] Extend native diagnostic capture to submit the same evaluated spline tiles as the visible pass. Assign route/interval/tile/material IDs. An isolated `capture-model model=320_02_1` cannot prove spline placement or continuity.
- [ ] Record startup and an out-and-back camera path through the house; use at least six fixed poses plus intermediate transition frames. Repeat after reload and with a glass predecessor draw. Run with ordinary production LOD/depth settings and authored camera start.
- [ ] For each expected unoccluded segment, verify projected endpoint alignment, connected silhouette and depth agreement. Normalize pixel tolerances by projection and measured cable width; exclude legitimate house occlusion using scene depth. Do not require every segment to remain visible through walls.
- [ ] Compare tile/world transforms across camera poses: identical authored/animation state must produce identical geometry. Require no unexplained submission losses and no startup-only malformed frame after load completes.
- [ ] Validate two routes separately, one railway spline, one vertical lift path and a mixed opaque/glass building. Confirm selection/picking and camera navigation still agree with displayed geometry.

```powershell
cmake --build build --config Release --target igi_tests igi-editor --parallel 1
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& .\bin\Release\igi_tests.exe --gtest_filter='SplineGeometry.*' --gtest_color=no
if ($LASTEXITCODE -ne 0) { throw 'Spline regression failed' }
# Use the new harness only after its implementation and fixture creation.
# Its camera path must be captured from the reproduced ordinary scene.
```

- [ ] Launch the editor through WMI with game root as working directory, verify SessionId 1, Responding true, memory >30 MB, and executable hash matching the build. Keep runtime game launch strictly on the prescribed `D:\IGI1\igi.exe` WMI method.
- [ ] Verify game-data hashes unchanged, remove temporary `[CABLE-TRACE]` logs, close the launched process, and preserve before/after artifacts. Do not start overlapping builds if a tool response times out; inspect the original process first.

**Acceptance:** Startup cable shape matches authored/reference route; entering, passing and reversing through WinchHouse does not deform or unexpectedly erase it; legitimate depth occlusion works; endpoint/continuity/state regressions pass; ordinary-scene screenshots and spline-specific portable evidence agree; corpus unchanged. Record established cause and bug `IGIED-LEVEL12-WINCHHOUSE-CABLE-001` in the final report and mem0 after actual verified implementation. Keep this separate from the prior WinchHouse wall/attachment issue.
