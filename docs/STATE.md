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
