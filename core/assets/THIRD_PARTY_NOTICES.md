# Third-party notices

Everything in this repository is covered by this project's own terms **except** the two
files named below, which carry a real real-world thermal capture and are GPL-3.0.

## Real radiometric reference capture

- **Files:**
  - `core/assets/gpl_source_IR_2412.jpg` — the original source JPEG, unmodified.
  - `core/src/real_reference_data.cpp` — data derived from it (see below).
- **License: GPL-3.0.** Operator approval was obtained before adding this (see
  `docs/STATE.md`) — including a GPL-licensed data file in an otherwise non-GPL codebase
  is a deliberate, known exception, not an oversight.
- **Original source:** [gtatters/Thermimage](https://github.com/gtatters/Thermimage),
  `inst/extdata/IR_2412.jpg`, GPL-3.0 licensed.
- **Original capture:** real FLIR SC660 thermal camera, 640×480, captured 2013-05-09.
- **What was derived:** the embedded FLIR raw radiometric payload (16-bit sensor counts +
  Planck/atmospheric calibration constants, all read from the source file's own metadata —
  none invented) was extracted with `exiftool -b -RawThermalImage`, converted to real
  calibrated object temperatures using FLIR's published Planck/atmospheric-transmission
  equations, then re-encoded into this project's own 0.01 K/count format
  (`T[°C] = counts × 0.01 − 273.15`, matching `core/include/a70/camera.h`'s documented
  `TemperatureLinear10mK` contract) so it can be served through the same pipeline as any
  other frame. The temperature *values* are real measurements; only the encoding changed.
- **How it's used:** served by `SimBackend`'s `RealReferenceCapture` feature as a single
  static frame — no synthetic noise, vignette, or emissivity reprocessing is applied on top
  (it's already final calibrated data). The GUI labels it distinctly ("● REAL CAPTURE
  (static)", a dedicated overlay) so it can never be mistaken for live camera data or for
  the procedural simulator's output — see `AGENTS.md`'s prime directive on never presenting
  simulated/other data as live.

## Reference thermal photograph (About panel)

- **File:** `gui/assets/reference_thermal_aqua_tower.jpg`.
- **License: CC0 1.0 Universal Public Domain Dedication** — no restrictions, attribution
  given here as a courtesy.
- **Source:** [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Aqua_Tower_thermal_imaging.jpg),
  "Aqua Tower thermal imaging.jpg", Jim D'Aloisio, 2010-12-18, captured with a real FLIR b60
  camera (confirmed via the file's own EXIF `Make`/`Model` tags).
- **How it's used:** shown only in the About dialog as a labeled visual reference. It is
  **not** radiometric (no embedded calibration data) and is never used as a data source —
  see the conversation record in `docs/STATE.md` for why that distinction matters here.
