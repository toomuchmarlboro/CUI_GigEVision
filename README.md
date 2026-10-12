# A70 Thermal (C++): FLIR A70 radiometric capture, Qt GUI, 24/7 Modbus service

A C++17 port of the Python toolkit, built for unattended use on oil & gas sites. It runs on Windows, x86 Linux, Jetson and Raspberry Pi.

| Part | What it does |
|---|---|
| `a70_gui` (Qt 5 or 6) | Live view in **absolute °C** with spot/box tools, alarm thresholds and hot/cold markers. Radiometric snapshots and HDF5 video, interval capture with a CSV log, object parameters, NUC, measurement range, a GenICam feature browser, and an optional Modbus server |
| `a70_cli serve` | Headless service: **auto-reconnect**, **Modbus TCP**, **systemd watchdog**, disk-space guard, interval capture |
| `a70_cli` other commands | `list`, `features`, `snap`, `record`, `timelapse`, plus file tools `info`, `csv`, `trend`, `frames` |
| `liba70core` | Shared engine: backends, A70 logic, file formats, recorder, Modbus |

The files are **identical in format to the Python tool**. A TIFF or HDF5 file from either one opens in the other, and in h5py, tifffile, MATLAB and ImageJ.

## How temperatures are obtained

The camera streams over GigE Vision as `Mono16` with `IRFormat = TemperatureLinear10mK`, so every pixel is already an absolute temperature:

```
T[°C] = counts × 0.01 − 273.15      (0.01 K steps, max 382 °C)
T[°C] = counts × 0.1  − 273.15      (TemperatureLinear100mK, for the 175–1000 °C range)
```

The extra metadata rows the A70 adds below the 640×480 image are removed. **Object parameters are applied inside the camera**, so set them before capturing. They're stored in every file and re-applied after every reconnect.

## Reliability features

- **Auto-reconnect.** If no frames arrive for 10 s (pulled cable, camera reboot, PoE blip), the camera is closed and reopened with back-off (1 s → 30 s). Configured parameters, frame rate and range are then **re-applied**.
- **Stale-data protection.** If no frame arrives within 3 s, the data is flagged *stale*. The GUI says so on the image, Modbus clears the "data fresh" bit and outputs `0x8000` instead of temperatures, alarm bits drop, and **snapshots are refused**. Old data is never presented as live.
- **Two heartbeats on Modbus.** The *service heartbeat* shows the process is alive. The *frame counter* shows the camera is streaming. A PLC can tell a hung service from a lost camera.
- **systemd watchdog** (`Type=notify`, `WatchdogSec=60`). The service only pings while its acquisition loop is turning. A driver hang leads to an automatic restart.
- **Disk guard.** Snapshots and recordings are refused below `min-free-mb`, and a recording in progress is stopped cleanly.
- **Never blocks acquisition.** HDF5 writing runs on its own thread with a bounded queue; frames dropped there are counted and reported. Snapshot and interval I/O runs on a separate worker thread.
- **Crash-tolerant recordings.** HDF5 is flushed every second, so a power cut loses at most about 1 s.
- **Measurement log integrity.** If the tool layout changes, a new CSV is started instead of misaligning columns.
- **Fault injection for testing.** The simulator's `SimCableUnplug` command makes the camera disappear for 4 s. The unit tests use it to prove that reconnect, stale flags and parameter re-application all work.

## Modbus TCP register map

Temperatures are signed int16 in **0.1 °C**; `0x8000` means no or stale data. Addresses are 0-based.

**Input registers (FC 04)**

| Addr | Meaning |
|---|---|
| 0 | service heartbeat (+1 per second) |
| 1 | frame counter (+1 per frame, wraps) |
| 2 | status bits: b0 connected · b1 data fresh · b2 recording · b3 interval active · b4 any alarm · b5 disk low · b6 error · b7 recorder error |
| 3 | last error: 0 none · 1 connect · 2 no frames · 3 snapshot · 4 record · 5 disk low · 6 camera · 7 command |
| 4 | data age (0.1 s) |
| 5 | frame rate (0.1 Hz) |
| 6 / 7 / 8 | frame max / min / mean |
| 9 / 10 | hotspot x / y |
| 11 | number of tools |
| 12 / 13 / 14 | snapshots · recorded frames · dropped frames (low 16 bits) |
| 15 | free disk (0.1 GB) |
| 16 | reconnect count |
| 17 | IR format (1 = 0.01 K, 2 = 0.1 K) |
| 20 + 8·i | tool i (0–7): +0 type (1 spot, 2 box) · +1 max · +2 min · +3 mean · +4 alarm · +5/+6 max x/y |

**Holding registers (FC 03/06/16)**

| Addr | Meaning |
|---|---|
| 0 | command: 1 snapshot · 2 start recording · 3 stop recording · 4 NUC · 5 start interval · 6 stop interval · 7 reconnect (cleared to 0 when done) |
| 1 | result: 1 ok · 2 failed · 3 unknown |
| 2 | command sequence (+1 per executed command; poll this to know it finished) |
| 3 | interval seconds (for command 5) |
| 4 | recording decimation (keep 1 of N, for command 2) |
| 10–17 | tool 1–8 alarm threshold (0.1 °C; alarm when tool max > threshold; `0x8000` = off) |

Modbus TCP has no authentication. Bind it to the control-network interface (`modbus-bind`) and keep it inside the OT zone (IEC 62443 zones and conduits).

## Build

Dependencies:
- **Required:** CMake ≥ 3.16, a C++17 compiler, zlib.
- **Optional:**
  - Qt 5.12+ or Qt 6 (GUI)
  - HDF5 (video recording)
  - libmodbus (Modbus server)
  - **Aravis 0.8** and/or the **Spinnaker SDK** (camera transport)

Without any camera transport you get the simulator only.

**Jetson (JetPack 6) / Raspberry Pi OS Bookworm / Ubuntu 22.04+**
```
sudo apt install build-essential cmake pkg-config zlib1g-dev libhdf5-dev libmodbus-dev \
                 libaravis-dev libusb-1.0-0-dev aravis-tools qtbase5-dev   # or qt6-base-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
ctest --test-dir build          # simulator + Aravis fake-camera tests
```
JetPack 5 (Ubuntu 20.04) only packages Aravis 0.6. Build Aravis 0.8 from source, or use Spinnaker ARM64.

**Windows (Visual Studio 2022 + vcpkg)**
```
vcpkg install zlib hdf5 libmodbus qtbase --triplet x64-windows
:: install the Spinnaker SDK (includes the GigE Vision filter driver)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
      -DSPINNAKER_DIR="C:/Program Files/Teledyne/Spinnaker"
cmake --build build --config Release
```
Run `windeployqt build\Release\a70_gui.exe` to bundle Qt.

CMake options:
- `-DA70_WITH_ARAVIS=AUTO|ON|OFF`, `-DA70_WITH_SPINNAKER=AUTO|ON|OFF`
- `-DA70_WITH_HDF5=ON|OFF`, `-DA70_WITH_MODBUS=ON|OFF`
- `-DA70_BUILD_GUI=ON|OFF` (turn the GUI off for a headless edge build)

## Run

```
./a70_gui --backend sim --connect                      # no camera needed
./a70_gui --backend aravis --device 192.168.10.50 --connect
./a70_cli features --backend aravis --out-file a70_features.csv   # first thing on the real camera
./a70_cli snap --emissivity 0.93 --reflected 30 --box CUI,160,230,290,325,45 --csv
./a70_cli record --duration 600 --decimate 30          # 1 Hz for 10 min
./a70_cli serve --config deploy/a70.conf               # 24/7 service
./a70_cli trend rec.h5 --box CUI,160,230,290,325 --spot 470,250
./a70_cli frames rec.h5 --every 10 && ffmpeg -framerate 30 -i rec_png/frame_%06d.png out.mp4
```

**Install as a service:** see the header of `deploy/a70.service`. It runs as an unprivileged user, may bind port 502, and uses a hardened filesystem and the watchdog. Check `systemctl status a70` for a live status line, and `journalctl -u a70` for logs.

## Network checklist (when the camera arrives)

1. **Power:** PoE 802.3af from the kit's injector or a PoE switch (Jetson and Pi don't supply PoE), or 24 V DC.
2. **Stream wired.** GigE Vision over Wi-Fi drops packets.
3. **Same subnet:** give the host NIC a static IP on the camera's subnet. `arv-tool-0.8` or `a70_cli list` should find it.
4. **Bandwidth:** about 150 Mbit/s at 30 Hz, so MTU 1500 is fine. Jumbo frames reduce CPU load if the NIC supports them. Aravis negotiates packet size. With Spinnaker, use `--packet-size` if frames time out.
5. **Linux UDP buffers:** `net.core.rmem_max=33554432` and `net.core.rmem_default=33554432` in `/etc/sysctl.d/60-gige.conf`.
6. **Windows:** allow `a70_gui` / `a70_cli` through the firewall, or frames never arrive.
7. **Find the real feature names:** run `a70_cli features --out-file a70_features.csv`. `IRFormat`, `ImageMode`, `ObjectEmissivity`, `ReflectedTemperature` and `AtmosphericTemperature` are documented names. Distance, humidity, NUC and range names are best-guess candidate lists in `core/src/camera.cpp`.

## Verification status

| Item | Status |
|---|---|
| Core, engine, recorder, file formats, Modbus, watchdog, CLI | Tested (simulator, unit tests, real Modbus client, systemd notify socket) |
| Aravis backend | Tested against Aravis's built-in fake GenICam camera |
| Qt GUI | Built and smoke-tested offscreen with **Qt 5.15 and Qt 6.4** |
| Python interop | h5py, tifffile and the Python tool read the C++ files |
| **Spinnaker backend** | **Written against the Spinnaker 3.x/4.x API but not compiled here** (SDK not redistributable). Expect small fixes on first build |
| **Windows build** | Not built here; CMake + vcpkg instructions above |
| **Real A70** | Not yet; run `features` first and adjust the candidate names if needed |

## Files

```
core/include/a70/  types · backend · camera · thermal · io · recorder · engine · modbus_server · watchdog · json
core/src/          implementations + aravis_backend / spinnaker_backend / sim_backend
gui/               Qt main window, thermal view, feature browser
cli/main.cpp       a70_cli
tests/             a70_tests (ctest)
deploy/            a70.service (systemd), a70.conf (example config)
```

## Not covered

- The visual 1280×960 camera and FLIR's compressed radiometric RTSP stream (that needs the FLIR Atlas SDK).
- `IRFormat=Radiometric` raw counts, which need FLIR's calibration constants.
- CUI screening relies on **relative ΔT**. Keep emissivity and reflected temperature consistent between captures, and confirm suspect areas with another inspection method.
