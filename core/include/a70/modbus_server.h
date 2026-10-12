#pragma once
// Modbus TCP server exposing live temperatures, health and alarms, plus a command register.
// Register map (0-based addresses; temperatures are signed int16 in 0.1 °C, 0x8000 = no data):
//
// INPUT REGISTERS (FC 04, read-only)
//   0  service heartbeat      +1 every second while the service runs (detects a hung service)
//   1  frame counter          +1 per received frame, wraps at 65535 (detects a frozen camera)
//   2  status bits            b0 camera connected   b1 data fresh       b2 recording
//                             b3 interval active    b4 any ROI alarm    b5 disk space low
//                             b6 error active       b7 recorder error
//   3  last error code        0 none 1 connect 2 no frames 3 snapshot 4 record 5 disk low 6 camera 7 command
//   4  data age               0.1 s units (65535 = never)
//   5  frame rate             0.1 Hz units
//   6  frame max  °C×10       7 frame min °C×10      8 frame mean °C×10
//   9  hotspot x              10 hotspot y
//   11 ROI count              12 snapshots taken (low 16 bits)
//   13 recorded frames (low)  14 dropped frames (low)
//   15 free disk GB×10        16 reconnect count     17 IR format (1 = 0.01 K, 2 = 0.1 K)
//   20 + 8·i  ROI i (i = 0..7):  +0 type (0 none, 1 spot, 2 box)  +1 max  +2 min  +3 mean (°C×10)
//                                +4 alarm (0/1)  +5 max x  +6 max y  +7 reserved
//
// HOLDING REGISTERS (FC 03 / 06 / 16, read-write)
//   0  command        write: 1 snapshot  2 start recording  3 stop recording  4 NUC
//                            5 start interval  6 stop interval  7 reconnect camera
//                     the server clears it to 0 once executed
//   1  command result 0 idle  1 ok  2 failed  3 unknown command
//   2  command sequence (+1 per executed command — poll this to know it finished)
//   3  interval seconds (used by command 5; shows the active interval)
//   4  recording decimation (keep 1 of N frames, used by command 2)
//   10..17  ROI 1..8 alarm threshold, °C×10 (alarm when ROI max > threshold), 0x8000 = disabled
//
// Modbus TCP has no authentication: bind it to the control-network interface only and keep it
// inside the OT zone (IEC 62443 zones/conduits).
#include <atomic>
#include <string>
#include <thread>

namespace a70 {

class CaptureEngine;

constexpr int kModbusInputRegs = 84;
constexpr int kModbusHoldingRegs = 18;
constexpr int kModbusRoiBase = 20;
constexpr int kModbusMaxRois = 8;

bool modbusAvailable();

class ModbusServer {
public:
    ModbusServer(CaptureEngine& engine, std::string bindAddress = "0.0.0.0", int port = 502, int maxClients = 8);
    ~ModbusServer();
    bool start(std::string* err = nullptr);
    void stop();
    bool running() const { return running_; }
    int clients() const { return clients_; }
    uint64_t requests() const { return requests_; }
    int port() const { return port_; }

private:
    void run();
    CaptureEngine& eng_;
    std::string bind_;
    int port_, maxClients_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::atomic<int> clients_{0};
    std::atomic<uint64_t> requests_{0};
    void* ctx_ = nullptr;
    void* map_ = nullptr;
    int listenFd_ = -1;
};

// pack helpers (also used by tests)
int16_t modbusTemp(double c);  // °C -> int16 0.1 °C, 0x8000 for NaN / out of range

}  // namespace a70
