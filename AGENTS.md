# AGENTS.md

Rules for any agent working in this repository — Claude Code, Codex, or anything else.
`CLAUDE.md` is a symlink to this file, so there is exactly one copy and it cannot drift.

This repository is the C++ toolkit for monitoring corrosion under insulation (CUI) with a
FLIR A70 thermal camera on oil and gas sites: a core library, a Qt GUI (`a70_gui`), and a
headless command-line tool and 24/7 service with a Modbus TCP server (`a70_cli`).

---

## Prime directive

**This is field software for a hazardous industry. A wrong temperature shown as right is
worse than no temperature.**

**Keep the human in the development loop. Automate nothing without explicit approval.**

Concretely, that means:

- Work in batches of eight increments (see the batch protocol below). Do not start a
  ninth increment because the eighth succeeded — stop at the batch boundary and report.
- Do not push to a remote, delete anything, install a heavy toolchain or SDK, or touch a
  real camera, PLC or network without asking. Those still require explicit approval every
  time, batch or no batch.
- Do not "while I was in there" — no drive-by refactors, no extra features, no
  helpful additions that were not requested.
- **Never present stale, simulated or guessed data as live camera data.** The engine's
  stale-data rules, the simulator's naming and the "not verified" labels exist for this.
  Weakening any of them is a decision for the operator, not an implementation detail.
- When something is ambiguous, choose the defensible default, **state the assumption
  in the report**, and keep building. Only a question whose wrong answer would be
  unsafe, would waste the whole batch, or needs a fact the agent cannot obtain —
  camera behavior, a GenICam feature name on the real unit, a site network fact, a
  client specification — blocks the work.
- **Surface open questions in the batch report, not silently in `docs/STATE.md`.** A
  decision recorded only in a state file is a decision made by default. Anything the
  agent assumed rather than knew is named in the report where the operator sees it.

---

## The batch protocol

Work proceeds in **batches of eight increments**. The operator approves a batch, not
each increment inside it. Approval of a batch plan is approval to design, implement,
test, and record all eight of its increments without stopping in between.

### Batch plan

Each batch report ends with the plan for the next eight increments. Each planned
increment states:

- the exact scope and explicit exclusions;
- the algorithm or design contract, including important state transitions and
  failure behavior (camera loss, stale data, disk full, Modbus client misbehavior);
- the files or interfaces expected to change;
- the behavior the operator will receive; and
- the verification commands and known information gaps.

The operator accepts, edits, or replaces the plan. "Continue", "go", or silence on the
detail is acceptance of the plan as written. Once accepted, execute all eight increments
without asking for another continuation or inserting an intermediate review checkpoint.

### Build and report

Inside an approved batch, for each increment: build only the planned scope, verify it,
record it in `docs/STATE.md`. Do not stop for review between increments of a batch.

At the end of the eighth increment — or immediately, if an increment turns out to be
blocked, unsafe, or materially different from what was planned — produce one batch
report stating, per increment:

1. **What was built** and what changed.
2. **What was verified**, with the exact commands the operator runs to check it.
3. **What is *not* verified**, and every assumption made in place of a question.
   "Tested on the simulator" and "tested on the real A70" are different claims; say which.
4. **What was skipped or deviated from the plan**, and why.

Then plan the next eight increments and **stop**.

A batch may be cut short only when continuing would be blocked, unsafe, or materially
different from the approved plan. If an early increment makes a later one wrong, report
at that point rather than build the wrong thing. Ordinary success does not create an
intermediate checkpoint: eight increments is both the approved target and the ceiling.

### Scope discipline still applies

- One increment should touch one area (`core/`, `gui/`, `cli/`, `deploy/`, `tests/`)
  wherever possible. If a step needs changes across four areas, it is too big — split it.
  A new core feature plus its test is one increment.
- No drive-by work. An increment does what its plan says and nothing else; the batch
  does not become a licence to refactor whatever the agent passes.
- Every increment is recorded in `docs/STATE.md` as it is completed, not batched up at
  the end. That file is the handoff between agents and between sessions; if you did
  work that is not in it, the next agent will not know it happened.
- `docs/STATE.md` and `docs/decisions/` do not exist yet. The first approved increment
  that needs them creates them; until then the decision table below is authoritative.

---

## Language

**English only.** Code, identifiers, comments, commit messages, documents, log messages,
Modbus register descriptions, file metadata keys — all English. The operator may write
to you in Indonesian; answer in the language they used, but everything committed to the
repository is English.

---

## Folder convention

The library owns the logic; every user interface is a thin client of it.

```
core/
  include/a70/          public headers — the library's contract
    types.h             DeviceInfo, Feature, RawFrame, CameraError
    backend.h           generic GenICam transport interface (no FLIR knowledge)
    camera.h            A70-specific layer: TemperatureLinear setup, row crop, object
                        parameters in °C, NUC, range, metadata
    thermal.h           ThermalFrame, statistics, spot/box tools, palettes
    io.h  recorder.h    snapshot formats, HDF5 recorder and reader
    engine.h            CaptureEngine: acquisition thread, reconnect, stale rules,
                        capture, commands — shared by GUI, CLI and Modbus
    modbus_server.h     Modbus TCP server and the register map (the contract lives here)
    watchdog.h          systemd notify / watchdog
  src/                  implementations; one backend per file
gui/                    Qt Widgets client (Qt 5.12+ or Qt 6)
cli/                    a70_cli: camera commands, `serve`, file tools
tests/                  a70_tests (ctest), simulator and Aravis fake camera only
deploy/                 systemd unit and example configuration
build/                  out-of-tree build output — never committed, never edited
```

Rules that keep it from rotting:

- **`backend.h` and its implementations know GenICam, not FLIR.** Anything about
  IRFormat, kelvin scaling, metadata rows or object parameters belongs in `camera.cpp`.
- **`core/` has no Qt.** The headless service must build and run without Qt installed.
  This is enforced by the build (`-DA70_BUILD_GUI=OFF`), not by good intentions.
- **Behavior belongs in the engine, not in a client.** If the GUI and the CLI both need
  it — stale checks, disk guard, re-applying parameters, capture rules — it goes in
  `CaptureEngine`, and both clients call it. A rule implemented in only one client is a
  bug waiting for the other client.
- **Optional dependencies stay optional.** Aravis, Spinnaker, HDF5 and libmodbus are each
  behind a `A70_HAVE_*` define with a working fallback (clear error or disabled control).
  Do not make any of them mandatory without approval.
- **Every new behavior gets a test in `tests/test_core.cpp`** that runs on the simulator.

Naming: `PascalCase` types, `camelCase` functions and members, trailing `_` on private
members, `snake_case` file names, `kName` constants. No abbreviations beyond the ones
already established (`fps`, `roi`, `nuc`, `cti`).

## Paths

No code builds a path from the source tree or the working directory. Output folders come
from configuration (`EngineConfig::folder`, `--out`, the GUI folder field). The installed
service writes only under `/var/lib/a70` (its systemd unit makes the rest of the
filesystem read-only). An installed GUI may sit in a folder the operator cannot write to.

---

## Hardware and simulator authority

**The real A70 has not been connected to this software yet.** Everything below follows
from that.

- **Documented facts** (FLIR datasheets, the MathWorks A70 example): GigE Vision, Mono16,
  `IRFormat = TemperatureLinear10mK | TemperatureLinear100mK`, `ImageMode`,
  `ObjectEmissivity`, `ReflectedTemperature`, `AtmosphericTemperature` (kelvin), extra
  metadata rows below the 640×480 image, M12 X-coded gigabit Ethernet, PoE 802.3af.
- **Guesses** (candidate lists in `core/src/camera.cpp`): the feature names for distance,
  humidity, NUC, measurement range and frame rate. Treat them as unverified.
- **The authority, once it exists, is the feature dump from the real unit**:
  `a70_cli features --out-file a70_features.csv`. When it arrives, store it under
  `docs/hardware/` and reconcile the candidate lists against it. Where prose, datasheet
  and dump disagree, follow the dump and record the conflict.
- Do not fill missing camera facts from memory. If a required fact is not established,
  ask the operator and mark it unverified in the report.
- **The simulator is not the camera.** `sim_backend.cpp` reproduces the documented
  contract (Mono16, scaling, metadata rows, kelvin parameters). Anything simulator-only
  must be named as such — `SimCableUnplug` is fault injection for tests and must never be
  described as camera behavior. Simulated temperatures, noise and frame timing are
  illustrative, not measured.
- **The Spinnaker backend has never been compiled** (the SDK is not redistributable).
  Do not claim it works. Changes to it are reported as "not compiled, not tested" until
  the operator builds it on a machine with the SDK.

---

## Interoperability contracts

These outputs are read by other software. Changing them silently breaks someone else.

- **File formats.** Snapshot TIFF (uint16 counts, JSON metadata in `ImageDescription`),
  sidecar JSON, HDF5 recordings (`/counts`, `/host_time_ns`, `/camera_timestamp`,
  `/frame_id`, `scale_K_per_count`, `metadata_json`) and the measurement-log CSV must stay
  readable by the Python toolkit (`a70_thermal`), h5py, tifffile and MATLAB. The
  conversion `T[°C] = counts × scale_K_per_count − 273.15` is permanent.
- **The Modbus register map** in `modbus_server.h` is what a PLC programmer codes
  against. Adding registers in unused addresses is allowed within an approved increment;
  moving, rescaling or reusing an existing address requires explicit approval and a note
  in the batch report. `0x8000` means "no or stale data" everywhere.
- **The configuration file keys** (`deploy/a70.conf`) and CLI options are used by
  installed services. Renaming one is a breaking change.

The Python toolkit is a sibling, not a dependency: never call it from C++, and do not copy
its code. Use it only to cross-check that files still interoperate.

---

## Agent workbench

User-requested one-off scripts, plots, generated images and investigation artifacts
belong under `workbench/`, not at the repository root and not in `core/`, `gui/`, `cli/`
or `tests/`.

- Choose `experiments/`, `troubleshooting/`, `plotting/` or `learning/`, then create a
  descriptive task folder. Keep its README, scripts and outputs together.
- Record inputs, commands, findings, limitations and every external-data dependency
  (camera recordings, site data) needed to reproduce it.
- Nothing in `workbench/` runs automatically. A script that changes data, settings,
  source, services, a camera or a network needs explicit operator approval before it runs.
- Product code never includes workbench code. Moving a finding into the product is
  separately planned and approved work.
- Site recordings may be confidential client data. Do not copy them outside the folder
  the operator named, and never commit them.

---

## Locked decisions

Settled in conversation with the operator; to be written up in `docs/decisions/`. Do not
reopen them without asking.

| | decision |
|---|---|
| 0001 | C++17, CMake ≥ 3.16; core library + Qt GUI + headless CLI/service |
| 0002 | English only, everywhere |
| 0003 | Images come over **GigE Vision** with `IRFormat = TemperatureLinear*`. Modbus carries numbers and commands only — never images |
| 0004 | Camera transports: Aravis (Linux/Jetson/Pi), Spinnaker (Windows and others), simulator. No FLIR Atlas SDK, no RTSP radiometric stream |
| 0005 | Qt Widgets, supporting both Qt 5.12+ and Qt 6 |
| 0006 | File formats identical to the Python toolkit (see Interoperability contracts) |
| 0007 | Modbus TCP server owned by the edge software, with the register map in `modbus_server.h`; bind to the control network only (IEC 62443 zones) |
| 0008 | Reliability rules: auto-reconnect with re-applied parameters, stale data never shown or saved as live, disk guard, writer thread never blocks acquisition, systemd watchdog pinged only while the acquisition loop turns |
| 0009 | The system is a CUI **screening and monitoring** tool, not a safety function. No SIL claims, and never wire it into a shutdown |
| 0010 | Relative ΔT over time is the primary CUI signal; interval capture and trending take priority over single-frame features |

---

## Commands

```
cmake -B build -DCMAKE_BUILD_TYPE=Release            # configure (prints which backends were found)
cmake --build build -j4                              # build everything
cmake -B build-headless -DA70_BUILD_GUI=OFF          # proves core/ and cli/ build without Qt
build/a70_tests                                      # unit tests: simulator + Aravis fake camera (~10 s)
QT_QPA_PLATFORM=offscreen build/a70_gui --selftest OUTDIR   # GUI smoke test, writes screenshots (~5 s)
build/a70_gui --backend sim --connect                # interactive, no camera needed
build/a70_cli serve --backend sim --modbus-port 15020 --out OUTDIR   # service on an unprivileged port
```

Check what was compiled in before debugging a missing feature: the configure step prints
`a70: Aravis … enabled`, `a70: HDF5 … enabled`, `a70: Modbus … enabled`, `a70: GUI with Qt …`.

### Formatting

No formatter is adopted yet (adopting clang-format is an open question for the operator).
Until then: match the surrounding style — 4-space indentation, braces on the same line,
lines up to about 120 characters. Do not reformat files or regions an increment does not
otherwise change.

### Compiler warnings

The build uses `-Wall -Wextra`. An increment must not add new warnings in the files it
touches. Do not silence a warning with a pragma or a cast without explaining why in the
report.

### AI test limits

Claude Code, Codex, and every other AI agent use only verification that is fast, local and
safe. AI agents must not run:

- anything that talks to a **real camera, PLC or site network**, including `a70_cli list`
  or `serve` with a real backend;
- the service on **port 502** or any privileged port, or anything requiring root;
- long or soak runs — no `serve`, `record` or `timelapse` run longer than 60 seconds, and
  no test command expected to take longer than 60 seconds; or
- `make install`, `systemctl`, or anything that changes the host outside `build/` and
  the output folder you were given.

`a70_tests` currently runs every test in one binary (about 10 s), which is within the
limit. When a test filter is added, use the smallest selection that verifies the change.

Run builds and tests in the background so independent work can continue. In Claude Code,
use `run_in_background: true`, not a trailing `&`. In another harness, redirect output to
a file and keep the process identifier. Do not poll; read the result once the current
independent work is finished, and never report a test as passing until its result has
actually been read.

Background services started for a test (`a70_cli serve`, a Modbus client) must be stopped
by process ID before the increment ends. Never stop them with `pkill -f` on a pattern —
it can match and kill the agent's own shell.

If adequate verification needs a prohibited step — a real camera, a Windows or Spinnaker
build, a long soak — record it as not run and give the operator the exact command.

A build is per-platform: build on Windows for Windows, on the Jetson or Pi for ARM64.
Configure output is not proof that a backend works: Aravis is proven only by
`a70_tests` (fake camera) or a real camera, and Spinnaker only by a real build and a
real camera.
