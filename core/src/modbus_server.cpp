#include "a70/modbus_server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "a70/engine.h"

#ifdef A70_HAVE_MODBUS
#include <modbus.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <unistd.h>
#endif
#endif

namespace a70 {

int16_t modbusTemp(double c) {
    if (std::isnan(c) || c * 10 > 32767 || c * 10 < -32767) return int16_t(-32768);
    return int16_t(std::lround(c * 10));
}

static uint16_t sat16(double v) { return uint16_t(std::clamp(v, 0.0, 65535.0)); }

#ifdef A70_HAVE_MODBUS
bool modbusAvailable() { return true; }

ModbusServer::ModbusServer(CaptureEngine& e, std::string bind, int port, int maxClients)
    : eng_(e), bind_(std::move(bind)), port_(port), maxClients_(maxClients) {}

ModbusServer::~ModbusServer() { stop(); }

bool ModbusServer::start(std::string* err) {
    if (running_) return true;
    modbus_t* ctx = modbus_new_tcp(bind_ == "0.0.0.0" ? nullptr : bind_.c_str(), port_);
    if (!ctx) {
        if (err) *err = "modbus_new_tcp failed";
        return false;
    }
    modbus_mapping_t* map = modbus_mapping_new_start_address(0, 0, 0, 0, 0, kModbusHoldingRegs, 0, kModbusInputRegs);
    if (!map) {
        modbus_free(ctx);
        if (err) *err = "modbus_mapping_new failed";
        return false;
    }
    int fd = modbus_tcp_listen(ctx, maxClients_);
    if (fd < 0) {
        if (err) *err = std::string("Cannot listen on ") + bind_ + ":" + std::to_string(port_) + " — " +
                        modbus_strerror(errno) + (port_ < 1024 ? " (port < 1024 needs root/CAP_NET_BIND_SERVICE)" : "");
        modbus_mapping_free(map);
        modbus_free(ctx);
        return false;
    }
    // defaults
    map->tab_registers[4] = 1;
    for (int i = 0; i < kModbusMaxRois; ++i) map->tab_registers[10 + i] = uint16_t(0x8000);
    ctx_ = ctx;
    map_ = map;
    listenFd_ = fd;
    running_ = true;
    th_ = std::thread(&ModbusServer::run, this);
    return true;
}

void ModbusServer::stop() {
    running_ = false;
    if (th_.joinable()) th_.join();
    if (listenFd_ >= 0) {
#ifdef _WIN32
        closesocket(listenFd_);
#else
        ::close(listenFd_);
#endif
        listenFd_ = -1;
    }
    if (map_) modbus_mapping_free(static_cast<modbus_mapping_t*>(map_));
    if (ctx_) modbus_free(static_cast<modbus_t*>(ctx_));
    map_ = ctx_ = nullptr;
}

namespace {
void fillInputs(modbus_mapping_t* m, const EngineStatus& s, uint16_t heartbeat) {
    uint16_t* r = m->tab_input_registers;
    for (int i = 0; i < kModbusInputRegs; ++i) r[i] = 0;
    r[0] = heartbeat;
    r[1] = uint16_t(s.framesTotal & 0xffff);
    uint16_t bits = 0;
    if (s.connected) bits |= 1u << 0;
    if (s.fresh) bits |= 1u << 1;
    if (s.recording) bits |= 1u << 2;
    if (s.intervalActive) bits |= 1u << 3;
    if (s.anyAlarm && s.fresh) bits |= 1u << 4;
    if (s.diskLow) bits |= 1u << 5;
    if (s.lastErrorCode != 0) bits |= 1u << 6;
    if (!s.recError.empty()) bits |= 1u << 7;
    r[2] = bits;
    r[3] = uint16_t(s.lastErrorCode);
    r[4] = sat16(s.frameAgeS * 10);
    r[5] = sat16(s.fps * 10);
    bool have = s.framesTotal > 0 && s.fresh;
    auto T = [&](double c) { return uint16_t(have ? modbusTemp(c) : int16_t(-32768)); };
    r[6] = T(s.stats.maxC);
    r[7] = T(s.stats.minC);
    r[8] = T(s.stats.meanC);
    r[9] = uint16_t(s.stats.maxX);
    r[10] = uint16_t(s.stats.maxY);
    r[11] = uint16_t(s.rois.size());
    r[12] = uint16_t(s.snapshots & 0xffff);
    r[13] = uint16_t(s.recWritten & 0xffff);
    r[14] = uint16_t(s.recDropped & 0xffff);
    r[15] = s.freeMB < 0 ? 0 : sat16(s.freeMB / 100.0);
    r[16] = uint16_t(s.reconnects & 0xffff);
    r[17] = s.irFormat == "TemperatureLinear100mK" ? 2 : (s.irFormat.empty() ? 0 : 1);
    for (size_t i = 0; i < s.rois.size() && i < size_t(kModbusMaxRois); ++i) {
        uint16_t* b = r + kModbusRoiBase + 8 * i;
        b[0] = s.rois[i].kind == Roi::Kind::Spot ? 1 : 2;
        if (i < s.roiResults.size() && s.roiResults[i].valid) {
            const RoiResult& rr = s.roiResults[i];
            b[1] = T(rr.maxC);
            b[2] = T(rr.minC);
            b[3] = T(rr.meanC);
            b[4] = (rr.alarm && have) ? 1 : 0;
            b[5] = uint16_t(rr.maxX);
            b[6] = uint16_t(rr.maxY);
        } else {
            b[1] = b[2] = b[3] = uint16_t(0x8000);
        }
    }
}
}  // namespace

void ModbusServer::run() {
    auto* ctx = static_cast<modbus_t*>(ctx_);
    auto* map = static_cast<modbus_mapping_t*>(map_);
    std::vector<int> conns;
    std::vector<uint8_t> query(MODBUS_TCP_MAX_ADU_LENGTH);
    uint16_t heartbeat = 0;
    auto lastBeat = std::chrono::steady_clock::now();
    uint16_t cmdSeq = 0;
    uint16_t pendingInterval = 60;  // what command 5 will use if no interval is running

    auto syncHoldingFromEngine = [&] {
        EngineStatus s = eng_.status();
        map->tab_registers[3] = s.intervalActive ? uint16_t(std::lround(s.intervalS)) : pendingInterval;
        for (int i = 0; i < kModbusMaxRois; ++i) {
            double a = i < int(s.rois.size()) ? s.rois[i].alarmAboveC : NAN;
            map->tab_registers[10 + i] = uint16_t(modbusTemp(a));
        }
    };
    syncHoldingFromEngine();

    while (running_) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastBeat >= std::chrono::seconds(1)) {
            ++heartbeat;
            lastBeat = now;
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listenFd_, &rd);
        int maxfd = listenFd_;
        for (int c : conns) {
            FD_SET(c, &rd);
            maxfd = std::max(maxfd, c);
        }
        timeval tv{0, 200000};
        int n = select(maxfd + 1, &rd, nullptr, nullptr, &tv);
        if (n <= 0) continue;
        if (FD_ISSET(listenFd_, &rd)) {
            int s = listenFd_;
            int c = modbus_tcp_accept(ctx, &s);
            if (c >= 0) {
                if (int(conns.size()) >= maxClients_) {
#ifdef _WIN32
                    closesocket(c);
#else
                    ::close(c);
#endif
                } else {
                    conns.push_back(c);
                }
            }
        }
        for (size_t i = 0; i < conns.size();) {
            int c = conns[i];
            if (!FD_ISSET(c, &rd)) { ++i; continue; }
            modbus_set_socket(ctx, c);
            int rc = modbus_receive(ctx, query.data());
            if (rc > 0) {
                ++requests_;
                syncHoldingFromEngine();
                std::vector<uint16_t> before(map->tab_registers, map->tab_registers + kModbusHoldingRegs);
                fillInputs(map, eng_.status(), heartbeat);
                map->tab_registers[2] = cmdSeq;
                modbus_reply(ctx, query.data(), rc, map);
                // apply writes from the client
                for (int k = 0; k < kModbusMaxRois; ++k) {
                    uint16_t v = map->tab_registers[10 + k];
                    if (v != before[10 + k]) eng_.setRoiAlarm(size_t(k), v == 0x8000 ? NAN : int16_t(v) / 10.0);
                }
                uint16_t interval = map->tab_registers[3];
                if (interval != before[3] && interval > 0) {
                    pendingInterval = interval;
                    if (eng_.status().intervalActive) eng_.startInterval(interval);
                }
                uint16_t cmd = map->tab_registers[0];
                if (cmd != 0) {
                    std::string err;
                    bool ok;
                    if (cmd == uint16_t(Command::StartRecording)) {
                        ok = eng_.startRecording(std::max<int>(1, map->tab_registers[4]), "gzip", 0, &err);
                    } else if (cmd == uint16_t(Command::StartInterval)) {
                        eng_.startInterval(interval > 0 ? interval : 60);
                        ok = true;
                    } else if (cmd >= 1 && cmd <= 7) {
                        ok = eng_.command(Command(cmd), &err);
                    } else {
                        map->tab_registers[0] = 0;
                        map->tab_registers[1] = 3;
                        ++cmdSeq;
                        ++i;
                        continue;
                    }
                    map->tab_registers[0] = 0;
                    map->tab_registers[1] = ok ? 1 : 2;
                    map->tab_registers[2] = ++cmdSeq;
                }
                ++i;
            } else if (rc == -1) {
#ifdef _WIN32
                closesocket(c);
#else
                ::close(c);
#endif
                conns.erase(conns.begin() + long(i));
            } else {
                ++i;  // rc == 0: request for another unit id / ignored
            }
            clients_ = int(conns.size());
        }
    }
    for (int c : conns) {
#ifdef _WIN32
        closesocket(c);
#else
        ::close(c);
#endif
    }
    clients_ = 0;
}

#else  // no libmodbus

bool modbusAvailable() { return false; }
ModbusServer::ModbusServer(CaptureEngine& e, std::string bind, int port, int maxClients)
    : eng_(e), bind_(std::move(bind)), port_(port), maxClients_(maxClients) {}
ModbusServer::~ModbusServer() = default;
bool ModbusServer::start(std::string* err) {
    if (err) *err = "This build has no libmodbus support";
    return false;
}
void ModbusServer::stop() {}
void ModbusServer::run() {}

#endif

}  // namespace a70
