#pragma once
// CaptureEngine: everything between the camera and the user interfaces.
//   - acquisition thread (grab, crop, stats, measurement tools, recorder feed)
//   - automatic reconnect with back-off when the camera drops (cable, power, reboot)
//   - snapshots, interval capture + CSV log, HDF5 recording with disk-space guard
//   - command interface shared by GUI, CLI and the Modbus server
// The GUI, the headless CLI and the Modbus server all drive this same object.
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "a70/camera.h"
#include "a70/io.h"
#include "a70/recorder.h"

namespace a70 {

struct EngineConfig {
    std::string backend;                  // "" = defaultBackend()
    std::string device;                   // id / IP / serial, "" = first camera
    int packetSize = 0;                   // 0 = auto
    std::string irFormat = "TemperatureLinear10mK";
    std::string folder = "a70_captures";
    std::string prefix = "a70";
    std::string palette = "Iron";         // for snapshot PNG previews
    bool csvWithSnapshots = false;
    bool reconnect = true;                // keep retrying after a camera loss
    double reconnectAfterS = 10.0;        // no frames this long -> reopen camera
    double staleAfterS = 3.0;             // no frames this long -> data flagged stale
    double minFreeMB = 500.0;             // refuse/stop capture below this free disk space
    std::map<std::string, double> params; // object parameters applied on every (re)connect
    std::optional<double> frameRate;
    std::string range;
    bool nucOnConnect = false;
    double displayHz = 15.0;              // max frame-callback rate
    // Best-effort visual-light companion image on every snapshot (see A70Camera::grabVisual).
    // Off by default: unlike everything else in this engine, it briefly pauses live
    // acquisition to switch ImageMode, so it is opt-in rather than silently changing the
    // reliability behavior of existing deployments.
    bool captureVisualOnSnapshot = false;
};

enum class Command : int {
    None = 0,
    Snapshot = 1,
    StartRecording = 2,
    StopRecording = 3,
    Nuc = 4,
    StartInterval = 5,
    StopInterval = 6,
    Reconnect = 7,
};
const char* toString(Command c);

struct EngineStatus {
    bool connected = false;
    bool streaming = false;
    bool fresh = false;              // a frame arrived within staleAfterS
    double fps = 0;
    double frameAgeS = 1e9;
    uint64_t framesTotal = 0;
    uint64_t loopCounter = 0;        // advances every acquisition-loop pass (watchdog)
    uint32_t reconnects = 0;
    FrameStats stats;
    std::vector<Roi> rois;
    std::vector<RoiResult> roiResults;
    bool anyAlarm = false;
    bool recording = false;
    uint64_t recWritten = 0, recDropped = 0, recBytes = 0;
    std::string recPath, recError;
    bool intervalActive = false;
    double intervalS = 0;
    uint64_t snapshots = 0;
    std::string lastSnapshot;
    std::string lastError;
    int lastErrorCode = 0;           // see ErrorCode
    double freeMB = -1;
    bool diskLow = false;
    DeviceInfo device;
    std::string irFormat;
    double scaleK = 0.01;
};

enum ErrorCode : int {
    ErrNone = 0, ErrConnect = 1, ErrNoFrames = 2, ErrSnapshot = 3, ErrRecord = 4, ErrDiskLow = 5,
    ErrCamera = 6, ErrCommand = 7,
};

class CaptureEngine {
public:
    using FrameCallback = std::function<void(std::shared_ptr<const ThermalFrame>)>;
    using LogCallback = std::function<void(const std::string& level, const std::string& msg)>;

    explicit CaptureEngine(EngineConfig cfg);
    ~CaptureEngine();
    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    // Opens the camera now (error -> false + *err). With cfg.reconnect the
    // acquisition thread is started even if this first attempt fails.
    bool start(std::string* err = nullptr);
    void stop();
    bool running() const { return running_; }

    void setFrameCallback(FrameCallback cb);
    void setLogCallback(LogCallback cb);

    // camera control (thread-safe). Returns nullptr while disconnected.
    A70Camera* camera();
    bool changeIrFormat(const std::string& fmt, std::string* err);
    void setParamPersistent(const std::string& key, double v);  // also re-applied on reconnect

    std::shared_ptr<const ThermalFrame> latest() const;
    EngineStatus status() const;

    // measurement tools
    void setRois(std::vector<Roi> rois);
    std::vector<Roi> rois() const;
    void setRoiAlarm(size_t index, double aboveC);  // NaN disables

    // Visual (non-radiometric) companion frame — see A70Camera::grabVisual. Briefly pauses
    // live acquisition (holds the same mutex every grab() takes); callers don't need to.
    bool captureVisual(std::vector<uint8_t>& rgb, int& w, int& h, std::string* err = nullptr);

    // capture
    std::optional<SnapshotPaths> snapshot(std::string* err = nullptr, bool logMeasurements = false);
    bool startRecording(int decimate, const std::string& compression, double durationS, std::string* err);
    void stopRecording();
    void startInterval(double seconds);
    void stopInterval();

    bool command(Command c, std::string* err = nullptr);

    EngineConfig& config() { return cfg_; }  // change folder/prefix/palette between captures

private:
    void loop();
    bool openCamera(std::string* err);
    void closeCamera();
    void process(const ThermalFrame& f);
    void periodic();
    void setError(int code, const std::string& msg);
    void log(const std::string& level, const std::string& msg);
    void ioWorker();
    void postIo(std::function<void()> job);

    EngineConfig cfg_;
    std::unique_ptr<A70Camera> cam_;
    mutable std::mutex camLifeMtx_;   // guards cam_ pointer (open/close vs. users)
    std::mutex acqMtx_;               // held by acquisition thread around each grab
    std::thread th_, ioTh_;
    std::atomic<bool> running_{false}, connected_{false};

    mutable std::mutex m_;            // guards everything below
    std::shared_ptr<const ThermalFrame> latest_;
    FrameStats stats_;
    std::vector<Roi> rois_;
    std::vector<RoiResult> roiRes_;
    double lastFrameMono_ = 0, fps_ = 0, lastCbMono_ = 0, lastPeriodic_ = 0, nextInterval_ = 0;
    double intervalS_ = 0;
    uint64_t frames_ = 0, snapshots_ = 0;
    std::atomic<uint64_t> loopCounter_{0};
    uint32_t reconnects_ = 0;
    std::string lastError_, lastSnapshot_, measurementLog_;
    int lastErrorCode_ = 0;
    double freeMB_ = -1;
    double recordStopAt_ = 0;
    bool recOpen_ = false;
    DeviceInfo devInfo_;
    std::string irFormatNow_;
    double scaleNow_ = 0.01;
    FrameCallback frameCb_;
    LogCallback logCb_;

    Recorder rec_;

    std::mutex ioMtx_;
    std::condition_variable ioCv_;
    std::deque<std::function<void()>> ioQ_;
};

}  // namespace a70
