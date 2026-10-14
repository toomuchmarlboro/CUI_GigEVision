# STATE

Handoff log between agents/sessions, per AGENTS.md's batch protocol. Newest entries at
the bottom. This file did not exist before the batch below; it was created by the first
increment that needed it.

---

## Batch: settings drawer + realistic simulator + Pertamina RU IV branding

Approved plan: 4 increments (gui/ drawer+tabs, core/+tests/ sim realism, gui/+toolchain
branding). Built and verified in order, no blockers hit.

### Increment 1 — toolbar + hidden settings drawer (gui/)
- Built: always-visible `QToolBar` (Settings toggle, Connect, Snapshot, Record). The
  existing Connection/Display/Camera/Capture/Modbus/Measurements group-box panel moved
  behind the Settings toggle (button or `Ctrl+,`), hidden by default, state persisted in
  `QSettings` key `ui/panelVisible`. `btnConnect_`/`btnRecord_` are still constructed and
  wired inside `buildConnection()`/`buildCapture()` but are now parented into the toolbar,
  not their group box.
- Also fixed (pre-existing bug, found while testing, not part of the plan): the offscreen
  Qt platform plugin (`qoffscreend.dll`) had never been copied into `build/platforms/`
  locally, so `--selftest` crashed (exit code 3) before printing anything. Copied it in;
  not a code change, just a local build-output gap — a fresh `windeployqt --debug` run
  would also need `--no-compiler-runtime` style full plugin set, or this manual copy step.
- Files: `gui/main_window.h`, `gui/main_window.cpp`, `gui/main.cpp`.
- Verified: `cmake --build build` clean (no new warnings); `a70_gui.exe --selftest` exit 0,
  `fresh=1 fps=27.0 snapshots=1 alarm=1`; `panel.png`/`gui.png` inspected visually.
- Not verified: interactive on-screen run (only offscreen selftest).

### Increment 2 — reorganize drawer into tabs (gui/)
- Built: drawer content changed from a vertically-stacked `QVBoxLayout` of 6 group boxes to
  a `QTabWidget` (Connection, Display, Camera, Capture, Modbus, Measurements).
- Deviation from the original plan: kept a **Connection** tab (backend/device/resolution
  picker) instead of dropping it — that content still lives in the drawer, not the
  toolbar, so it needs a tab.
- Known cosmetic point, not a defect: the tab bar shows Qt/Fusion-style overflow arrows
  rather than fitting all 6 labels in the fixed-width drawer (widened 500→600px, didn't
  fully resolve it — root cause looks like `QTabWidget`'s own sizeHint under Fusion style,
  not the container width). Same pattern as browser tab overflow; left as-is.
- Files: `gui/main_window.cpp`.
- Verified: clean build; selftest exit 0; panel screenshot shows correct tab content.

### Increment 3 — realistic simulated thermal image + test (core/, tests/)
- Built: `sim_backend.cpp`'s static scene gained insulation jacket texture (2 low-freq sine
  components), circumferential cladding seams (~every 42px), two support-clamp
  thermal-bridging cold bands (x=90, x=400 — kept clear of the flange at x≈458–482 and the
  test suite's alarm box at x=160–290), a precomputed radial vignette applied to the
  delta-from-ambient (not ambient itself), column-wise fixed-pattern noise bias (mimics
  real microbolometer column non-uniformity), and a one-time spatial blur on the sensor
  noise field (correlated grain instead of independent per-pixel static). Fully
  deterministic — no wall-clock input feeds the static scene or its RNG seeds.
- Built: `testSimRealism()` in `tests/test_core.cpp` — two freshly-opened sim instances
  must agree pixel-for-pixel at (20, 400) (a point the dynamic wet-patch/hotspot overlays
  never reach), and the whole frame must stay within a -50..500°C envelope.
- Files: `core/src/sim_backend.cpp`, `tests/test_core.cpp`.
- Verified: `a70_tests.exe` → all tests passed, including the pre-existing tight
  flange(80-95°C)/ambient(20-32°C) bound checks in `testCameraSim()` — confirmed the new
  features don't shift those bounds. Also verified the headless build
  (`-DA70_BUILD_GUI=OFF`) still configures and builds clean, and its own `a70_tests.exe`
  passes (core/ has no Qt dependency, unaffected by Increment 4's Qt Svg addition either).

### Increment 4 — Pertamina Cilacap RU IV branding (gui/, toolchain)
- Toolchain: `vcpkg install qtsvg --triplet x64-windows` (explicit operator approval
  obtained before running). CMakeLists.txt treats Qt Svg as optional
  (`Qt6Svg_FOUND` → `A70_HAVE_SVG` define; without it the header falls back to a text
  "PERTAMINA" label instead of failing the build) — follows the project's existing
  optional-dependency convention.
- Built: `gui/assets/Pertamina_Logo.svg` and `Pertamina-white_Logo.svg` (copied from the
  operator's Downloads folder into the repo), `gui/resources.qrc`, `gui/theme.qss`. New
  `MainWindow::buildBrandHeader()` renders the white logo via `QSvgRenderer` (not
  `QPixmap(":...svg")`, to avoid needing the separate `imageformats/qsvg` plugin deployed
  at runtime) plus the title "PT Pertamina (Persero) — Refinery Unit IV Cilacap" / "FLIR
  A70 Radiometric CUI Monitor". Brand colors (`#006CB8` blue, `#ED1B2F` red, `#ACC42A`
  green) were read directly from the logo SVG's own fill colors, not guessed. The
  LIVE/ALARM status label colors were changed to the exact brand green/red — only the
  color constants changed, the fresh/alarm logic driving which one shows is untouched.
- Files: `CMakeLists.txt`, `gui/main_window.h`, `gui/main_window.cpp`, `gui/main.cpp`,
  `gui/resources.qrc`, `gui/theme.qss`, `gui/assets/*.svg`.
- Verified: reconfigure printed `a70: GUI with Qt 6.11.2 (Pertamina logo enabled)`; clean
  build; `a70_tests.exe` still all-pass; selftest screenshots show the logo rendering
  crisply and brand colors applied to toolbar/tabs.
- Not verified: Qt5 fallback path (only Qt6 exercised); real interactive on-screen render.

### Cross-cutting, not yet verified for any increment above
- No real FLIR A70 camera involved anywhere in this batch — simulator only, consistent
  with AGENTS.md (camera has never been connected to this software).
- No interactive windowed run was performed by the agent (only offscreen
  `QT_QPA_PLATFORM=offscreen --selftest`), per the AI test limits in AGENTS.md. The
  operator should do one windowed run (`build\a70_gui.exe --backend sim --connect`) to
  confirm the real on-screen appearance (font rendering, exact tab-bar fit, logo scaling)
  before considering this done end-to-end.

### Open / no further increments planned
Nothing from the approved 4-increment plan was skipped. No next-batch plan proposed —
awaiting operator direction on what's next.

---

## Follow-up: reference thermal image, About panel, window scaling (gui/)

Operator asked for "a realistic thermal image/video, a real thermal capture" in the demo.
Flagged a real constraint before building: a downloaded thermal photo has no calibrated
per-pixel temperature data, so feeding it into the simulator's radiometric pipeline would
mean fabricating a color→temperature mapping and presenting it through the same path as
real camera data — exactly what AGENTS.md's prime directive forbids. Operator agreed to
use a real photo as a visual-only reference, kept separate from the live simulator.

- Built: `gui/assets/reference_thermal_aqua_tower.jpg` — a genuine FLIR camera photograph
  (EXIF-verified: `manufacturer=FLIR Systems AB, model=Flir b60`), CC0 1.0 Universal Public
  Domain, Jim D'Aloisio, 2010, sourced from Wikimedia Commons
  (https://commons.wikimedia.org/wiki/File:Aqua_Tower_thermal_imaging.jpg). FLIR's own
  official sample/dataset pages were checked first but are gated behind a login/registration
  form with no accessible direct download — not usable without the operator creating an
  account themselves.
- Built: new "About" toolbar button → `MainWindow::showAbout()` dialog showing the
  reference photo with an explicit caption: real FLIR camera photo, CC0-licensed, **not**
  data from this application. Kept structurally separate from `ThermalView` / the live
  radiometric pipeline — no code path lets this image reach the engine, Modbus, or any
  measurement.
- Built: window geometry now persists across restarts (`QSettings` key `ui/geometry`,
  saved in `closeEvent`); first run (no saved geometry) starts maximized so the camera view
  fills the screen by default; Qt5 build path gets `Qt::AA_EnableHighDpiScaling` explicitly
  (Qt6 already defaults this on) for multi-monitor/high-DPI scaling.
- Files: `gui/main_window.h`, `gui/main_window.cpp`, `gui/main.cpp`, `gui/resources.qrc`,
  `gui/assets/reference_thermal_aqua_tower.jpg`.
- Verified: clean build; `a70_tests.exe` all pass (one transient timing flake on
  `recWritten >= 5` — a pre-existing tight-timing test unrelated to these GUI-only changes,
  passed cleanly on rerun ×2); selftest screenshot confirms maximize-on-first-run and the
  About button's toolbar placement.
- Not verified: the About dialog's actual rendering (selftest doesn't open it — only
  code-reviewed); real interactive on-screen run.
- Open item, not part of this change: `testEngine`'s recording-frame-count check
  (`tests/test_core.cpp:183`) is timing-flaky under system load. Worth hardening separately
  (e.g. poll for frame count instead of a fixed sleep) if it recurs — out of scope here
  since it's pre-existing and untouched by this work.

---

## Follow-up: genuine real radiometric capture wired into the simulator

Operator pushed further: "why is it not a real photorealistic tiff" — wanted actual real
captured radiometric data playable through the app, not just a visual-only reference photo.
Found a real option: `gtatters/Thermimage` (GitHub, GPL-3.0) ships `IR_2412.jpg`, a genuine
FLIR SC660 capture at 640×480 (exact match to the A70's native resolution) with its full
raw radiometric payload (16-bit counts + Planck/atmospheric calibration constants) intact.
Flagged two real decisions before touching code — GPL-3.0 licensing of the source data, and
the real engineering work needed to parse FLIR's proprietary raw payload — operator
explicitly approved both ("proceed anyway").

- **Extraction (one-time, offline, not part of the shipped build):** installed `exiftool`
  (OliverBetz.ExifTool via winget — a one-time dev tool, not a runtime dependency),
  extracted the embedded `RawThermalImage` (itself a 640×480 16-bit TIFF) and its Planck
  constants, converted raw counts to real calibrated object temperatures using FLIR's
  published Planck/atmospheric-transmission equations (reimplemented from public
  documentation, not copied from the GPL repo's R code), verified the result is physically
  sane (22.4–34.9 °C) and visually resolves to a recognizable real thermal photo (a bird —
  genuine detail no procedural generator produces). Re-encoded into this project's own
  0.01 K/count format and embedded as a C++ array (`core/src/real_reference_data.cpp`,
  ~1.8 MB, compiled directly into `a70core` — no runtime file path dependency, works
  headless/CLI/GUI alike per the "behavior belongs in the engine" rule).
- **Built:** `SimBackend` gained a `RealReferenceCapture` boolean feature. When set, `grab()`
  serves the real decoded temperatures as a static single frame with **no** synthetic
  noise/vignette/emissivity reprocessing on top (it's already final calibrated data) —
  kept structurally separate from the procedural scene's code path.
- **Built (GUI):** new checkbox in the Display tab ("Real reference capture (static, not
  simulated)"), enabled only when the connected backend actually has the feature (simulator
  only). When active, the health indicator shows a distinct "● REAL CAPTURE (static)" in
  brand blue (not LIVE's green, not ALARM's red) plus an overlay banner — this status can
  never be confused with live or procedurally-simulated data.
- **Licensing:** `core/assets/THIRD_PARTY_NOTICES.md` documents both this GPL-3.0 asset and
  the earlier CC0 About-panel photo — provenance, license, and exactly what was derived vs.
  original. `core/assets/gpl_source_IR_2412.jpg` (the unmodified original) is kept alongside
  the derived data for traceability.
- **Files:** `core/include/a70/real_reference.h`, `core/src/real_reference_data.cpp`,
  `core/src/sim_backend.cpp`, `core/assets/gpl_source_IR_2412.jpg`,
  `core/assets/THIRD_PARTY_NOTICES.md`, `CMakeLists.txt`, `gui/main_window.h`,
  `gui/main_window.cpp`, `tests/test_core.cpp`.
- **Verified:** clean build (one encoding bug in the generator script caught and fixed —
  comment text written in the system codepage instead of UTF-8, causing MSVC C4828
  warnings; regenerated clean); `a70_tests` all pass, including a new
  `testRealReferenceCapture()` (feature toggle works, frame bounded and physically sane,
  and — unlike the procedural scene — identical across two consecutive grabs, proving it's
  genuinely static); end-to-end verified through the actual shipped code path with a
  throwaway standalone verification program (compiled against the real `a70core.lib`, not
  a mockup) confirming the exact same 22.41–34.91 °C range as the offline Python check, and
  the app's own `colorize()` rendering it into the same recognizable real photo.
- **Not verified:** the GUI checkbox's actual on-screen behavior (selftest doesn't exercise
  this toggle — verified by code review and the standalone backend-level check instead,
  since both reach the identical `grab()` code path).
- **Pre-existing issue noticed, not fixed (out of scope):** `build/` is fully tracked in
  git and no `.gitignore` exists, contradicting this file's own "build/ ... never
  committed" rule. Flagging for the operator; not touched here since repo/git hygiene
  wasn't part of what was asked.

---

## Follow-up: TIFF Captures browser + visual-image companion capture

Operator asked for (1) a feature to view TIFFs as they're added to the capture folder, and
(2) pulling the "regular picture" real thermal cameras capture alongside the thermal image.
(2) directly implicated **locked decision 0004** ("no FLIR Atlas SDK") — flagged this before
writing any code. Research found the Atlas SDK doesn't even support C++ natively (it's a
.NET wrapper, and its own GigE module just wraps Spinnaker, which this project already uses
directly) — so operator chose to reopen 0004 but build the capability through the
**already-approved** Aravis/Spinnaker transports instead of Atlas, using the `ImageMode`
GenICam feature (`sim_backend.cpp` already listed `{"Thermal","Visual"}` as valid choices,
anticipated but never wired through). This does not actually require installing anything new.

### Increment 1 — visual-frame capture (`core/`)
- Built: `A70Camera::grabVisual()` — switches `ImageMode→"Visual"` (only if the backend
  offers that choice), grabs one frame, **always** restores the original `ImageMode`
  (even on failure) so a session never gets stuck off Thermal mode. Treats the raw buffer
  generically as 8-bit grayscale — the real camera's actual visual pixel layout is
  **unverified**, same status as every other candidate GenICam name in `camera.cpp`.
- Built: `CaptureEngine::captureVisual()` holds `acqMtx_` — the same mutex the acquisition
  loop already holds around every `grab()` (identical pattern to `changeIrFormat`) — so it
  safely pauses live acquisition rather than racing it, with no new locking primitive.
- Built: `EngineConfig::captureVisualOnSnapshot` (**default false**) — this is the one
  engine operation that briefly pauses live streaming, so unlike everything else in
  `CaptureEngine` it's opt-in rather than silently changing the reliability behavior of
  existing deployments/tests. `snapshot()` best-effort captures+saves `<stem>_visual.png`
  (via the existing `writePngRgb`) when enabled; failure never blocks the main thermal
  snapshot, only logs a warning.
- Built: `saveSnapshot()` extended with optional `visualRgb/visualW/visualH` params and a
  new `SnapshotPaths::visual` field — file-naming logic stays centralized in `io.cpp`.
- Built: simulator renders a distinct plain-grayscale synthetic image for `ImageMode=Visual`
  (clearly simulator-illustrative, never claimed as a real photo).
- Files: `core/include/a70/camera.h`, `core/src/camera.cpp`, `core/include/a70/engine.h`,
  `core/src/engine.cpp`, `core/include/a70/io.h`, `core/src/io.cpp`, `core/src/sim_backend.cpp`.
- Verified: `a70_tests` (new `testVisualCapture`: grab succeeds, `ImageMode` restored after,
  snapshot's visual companion file actually written) passed twice in a row; headless
  (`-DA70_BUILD_GUI=OFF`) build + its own test run also pass.

### Increment 2 — Captures tab TIFF browser (`gui/`)
- Built: new "Captures" tab in the settings drawer. `QFileSystemWatcher` on the configured
  output folder (re-watched whenever the folder field changes) repopulates a sorted
  (newest-first, sorted explicitly by `QFileInfo::lastModified()` rather than relying on
  `QDir::Time`'s ambiguous default ordering) list of `.tiff` files live. Selecting one
  decodes via the existing `io::readTiff16` + `jsonFindNumber(desc, "scale_K_per_count", …)`
  — the exact same pattern `a70_cli csv` already uses, not new logic — colorizes with the
  current palette, and shows it next to the paired `<stem>_visual.png` when present (or a
  clear "(no visual companion)" placeholder when not).
- Added `MainWindow::showCapturesTabForTest()` (same precedent as Increment 1's
  `setPanelVisible`) so `--selftest` actually exercises this tab rather than leaving it
  screenshot-unverified.
- Files: `gui/main_window.h`, `gui/main_window.cpp`, `gui/main.cpp` (selftest hook only).
- Verified: selftest screenshot shows the tab selected, list populated, thermal preview
  correctly decoded/colorized (matches the live view's colors), stats line correct, and the
  "no visual companion" fallback rendering correctly (this run had
  `captureVisualOnSnapshot` at its default-off, so the fallback path is what's confirmed;
  the paired-image path is confirmed separately at the engine/file level by
  `testVisualCapture`). `a70_tests` passed twice in a row; headless build unaffected
  (gui/ changes only).
- Not verified: the real A70's actual visual-mode pixel format/behavior (stated up front —
  simulator-only until real hardware is available); an interactive on-screen run (only
  offscreen selftest was used).
