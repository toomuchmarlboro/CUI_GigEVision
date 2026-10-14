#include "a70/engine.h"

#include <chrono>
#include <cmath>

namespace a70 {

const char* toString(Command c) {
    switch (c) {
        case Command::Snapshot: return "snapshot";
        case Command::StartRecording: return "start-recording";
        case Command::StopRecording: return "stop-recording";
        case Command::Nuc: return "nuc";
        case Command::StartInterval: return "start-interval";
        case Command::StopInterval: return "stop-interval";
        case Command::Reconnect: return "reconnect";
        default: return "none";
    }
}

CaptureEngine::CaptureEngine(EngineConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.backend.empty()) cfg_.backend = defaultBackend();
}

CaptureEngine::~CaptureEngine() { stop(); }

void CaptureEngine::log(const std::string& level, const std::string& msg) {
    LogCallback cb;
    {
        std::lock_guard<std::mutex> lk(m_);
        cb = logCb_;
    }
    if (cb) cb(level, msg);
}

void CaptureEngine::setError(int code, const std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(m_);
        lastError_ = msg;
        lastErrorCode_ = code;
    }
    log("error", msg);
}

void CaptureEngine::setFrameCallback(FrameCallback cb) {
    std::lock_guard<std::mutex> lk(m_);
    frameCb_ = std::move(cb);
}
void CaptureEngine::setLogCallback(LogCallback cb) {
    std::lock_guard<std::mutex> lk(m_);
    logCb_ = std::move(cb);
}

// ------------------------------------------------------------------ lifecycle
bool CaptureEngine::start(std::string* err) {
    if (running_) return true;
    {
        std::lock_guard<std::mutex> lk(camLifeMtx_);
        cam_ = std::make_unique<A70Camera>(cfg_.backend, cfg_.packetSize);
    }
    std::string e;
    bool ok = openCamera(&e);
    if (!ok) {
        if (err) *err = e;
        if (!cfg_.reconnect) {
            std::lock_guard<std::mutex> lk(camLifeMtx_);
            cam_.reset();
            return false;
        }
    }
    running_ = true;
    ioTh_ = std::thread(&CaptureEngine::ioWorker, this);
    th_ = std::thread(&CaptureEngine::loop, this);
    return ok;
}

void CaptureEngine::stop() {
    if (!running_ && !th_.joinable()) {
        std::lock_guard<std::mutex> lk(camLifeMtx_);
        cam_.reset();
        return;
    }
    running_ = false;
    ioCv_.notify_all();
    if (th_.joinable()) th_.join();
    if (ioTh_.joinable()) ioTh_.join();
    rec_.stop();
    closeCamera();
    std::lock_guard<std::mutex> lk(camLifeMtx_);
    cam_.reset();
}

A70Camera* CaptureEngine::camera() {
    // cam_ lives from start() to stop(); only its connection state changes in between,
    // so no lock is needed here (and the GUI never blocks behind a slow reconnect).
    return connected_ ? cam_.get() : nullptr;
}

bool CaptureEngine::openCamera(std::string* err) {
    std::lock_guard<std::mutex> lk(camLifeMtx_);
    if (!cam_) return false;
    try {
        DeviceInfo d = cam_->open(cfg_.device, cfg_.irFormat);
        for (auto& kv : cfg_.params) {
            try {
                cam_->setParam(kv.first, kv.second);
            } catch (const std::exception& e) {
                log("warning", "Could not set " + kv.first + ": " + e.what());
            }
        }
        if (cfg_.frameRate) {
            try {
                cam_->setFrameRate(*cfg_.frameRate);
            } catch (const std::exception& e) {
                log("warning", std::string("Frame rate: ") + e.what());
            }
        }
        if (!cfg_.range.empty()) {
            try {
                cam_->setRange(cfg_.range);
            } catch (const std::exception& e) {
                log("warning", std::string("Range: ") + e.what());
            }
        }
        if (cfg_.nucOnConnect && cam_->hasNuc()) cam_->nuc();
        for (auto& w : cam_->takeWarnings()) log("warning", w);
        cam_->start();
        connected_ = true;
        {
            std::lock_guard<std::mutex> l2(m_);
            lastFrameMono_ = monoSeconds();  // start the stale timer now
            devInfo_ = d;
            irFormatNow_ = cam_->irFormat();
            scaleNow_ = cam_->scaleK();
        }
        log("info", "Connected: " + d.label() + " [" + cfg_.backend + ", " + cam_->irFormat() + "]");
        return true;
    } catch (const std::exception& e) {
        try {
            cam_->close();
        } catch (...) {
        }
        connected_ = false;
        std::string msg = std::string("Connect failed: ") + e.what();
        if (err) *err = msg;
        setError(ErrConnect, msg);
        return false;
    }
}

void CaptureEngine::closeCamera() {
    std::lock_guard<std::mutex> lk(camLifeMtx_);
    connected_ = false;
    if (cam_) {
        try {
            cam_->close();
        } catch (...) {
        }
    }
}

// ------------------------------------------------------------------ acquisition loop
void CaptureEngine::loop() {
    ThermalFrame f;
    double backoff = 1.0, nextTry = 0;
    while (running_) {
        ++loopCounter_;
        if (!connected_) {
            if (!cfg_.reconnect) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                periodic();
                continue;
            }
            double now = monoSeconds();
            if (now >= nextTry) {
                if (openCamera(nullptr)) {
                    backoff = 1.0;
                    std::lock_guard<std::mutex> lk(m_);
                    ++reconnects_;
                } else {
                    nextTry = now + backoff;
                    backoff = std::min(backoff * 2, 30.0);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            periodic();
            continue;
        }
        bool got = false;
        try {
            std::lock_guard<std::mutex> lk(acqMtx_);
            A70Camera* c = cam_.get();
            if (c && c->streaming()) got = c->grab(f, 1.0);
            else std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } catch (const std::exception& e) {
            setError(ErrCamera, std::string("Grab failed: ") + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (got) {
            process(f);
        } else {
            double age;
            {
                std::lock_guard<std::mutex> lk(m_);
                age = monoSeconds() - lastFrameMono_;
            }
            if (age > cfg_.reconnectAfterS && cfg_.reconnect) {
                setError(ErrNoFrames, "No frames for " + std::to_string(int(age)) + " s — reconnecting");
                std::lock_guard<std::mutex> lk(acqMtx_);
                closeCamera();
                nextTry = monoSeconds() + 1.0;
            }
        }
        periodic();
    }
}

void CaptureEngine::process(const ThermalFrame& f) {
    auto sp = std::make_shared<const ThermalFrame>(f);
    FrameStats st = computeStats(f);
    double now = monoSeconds();
    FrameCallback cb;
    {
        std::lock_guard<std::mutex> lk(m_);
        double dt = now - lastFrameMono_;
        if (frames_ > 0 && dt > 0) fps_ = fps_ > 0 ? 0.9 * fps_ + 0.1 / dt : 1 / dt;
        lastFrameMono_ = now;
        ++frames_;
        latest_ = sp;
        stats_ = st;
        roiRes_ = measure(f, rois_);
        if (lastErrorCode_ == ErrNoFrames || lastErrorCode_ == ErrConnect) lastErrorCode_ = ErrNone;
        if (frameCb_ && now - lastCbMono_ >= 1.0 / std::max(0.5, cfg_.displayHz)) {
            lastCbMono_ = now;
            cb = frameCb_;
        }
    }
    if (rec_.active()) rec_.push(f);
    if (cb) cb(sp);
}

void CaptureEngine::periodic() {
    double now = monoSeconds();
    bool intervalDue = false, checkDisk = false, stopRec = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (intervalS_ > 0 && now >= nextInterval_) {
            intervalDue = true;
            nextInterval_ = now + intervalS_;
        }
        if (now - lastPeriodic_ > 5.0) {
            lastPeriodic_ = now;
            checkDisk = true;
        }
        if (recordStopAt_ > 0 && now >= recordStopAt_) {
            recordStopAt_ = 0;
            stopRec = true;
        }
    }
    if (stopRec && rec_.active()) postIo([this] { stopRecording(); });
    if (intervalDue) postIo([this] { snapshot(nullptr, true); });
    if (checkDisk) {
        double mb = freeSpaceMB(cfg_.folder);
        bool low = mb >= 0 && mb < cfg_.minFreeMB;
        {
            std::lock_guard<std::mutex> lk(m_);
            freeMB_ = mb;
        }
        if (low && rec_.active()) {
            postIo([this] { stopRecording(); });
            setError(ErrDiskLow, "Disk space low (" + std::to_string(int(mb)) + " MB) — recording stopped");
        }
    }
}

// ------------------------------------------------------------------ io worker (snapshots off the grab thread)
void CaptureEngine::postIo(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lk(ioMtx_);
        if (ioQ_.size() > 16) return;  // never pile up work if the disk is stuck
        ioQ_.push_back(std::move(job));
    }
    ioCv_.notify_one();
}

void CaptureEngine::ioWorker() {
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(ioMtx_);
            ioCv_.wait(lk, [&] { return !ioQ_.empty() || !running_; });
            if (ioQ_.empty()) return;
            job = std::move(ioQ_.front());
            ioQ_.pop_front();
        }
        try {
            job();
        } catch (const std::exception& e) {
            setError(ErrCommand, e.what());
        }
    }
}

// ------------------------------------------------------------------ queries
std::shared_ptr<const ThermalFrame> CaptureEngine::latest() const {
    std::lock_guard<std::mutex> lk(m_);
    return latest_;
}

EngineStatus CaptureEngine::status() const {
    EngineStatus s;
    double now = monoSeconds();
    {
        std::lock_guard<std::mutex> lk(m_);
        s.fps = fps_;
        s.frameAgeS = frames_ ? now - lastFrameMono_ : 1e9;
        s.fresh = connected_ && frames_ > 0 && s.frameAgeS <= cfg_.staleAfterS;
        if (s.frameAgeS > 2.0) s.fps = 0;
        s.framesTotal = frames_;
        s.reconnects = reconnects_;
        s.stats = stats_;
        s.rois = rois_;
        s.roiResults = roiRes_;
        for (auto& r : roiRes_) s.anyAlarm |= r.alarm;
        s.intervalActive = intervalS_ > 0;
        s.intervalS = intervalS_;
        s.snapshots = snapshots_;
        s.lastSnapshot = lastSnapshot_;
        s.lastError = lastError_;
        s.lastErrorCode = lastErrorCode_;
        s.freeMB = freeMB_;
        s.diskLow = freeMB_ >= 0 && freeMB_ < cfg_.minFreeMB;
    }
    s.loopCounter = loopCounter_;
    s.connected = connected_;
    s.recording = rec_.active();
    s.recWritten = rec_.written();
    s.recDropped = rec_.dropped();
    s.recBytes = rec_.bytes();
    s.recPath = rec_.path();
    s.recError = rec_.error();
    if (s.connected) {
        std::lock_guard<std::mutex> lk(m_);
        s.streaming = s.connected && s.frameAgeS < cfg_.staleAfterS;
        s.device = devInfo_;
        s.irFormat = irFormatNow_;
        s.scaleK = scaleNow_;
    }
    return s;
}

// ------------------------------------------------------------------ tools
void CaptureEngine::setRois(std::vector<Roi> rois) {
    std::lock_guard<std::mutex> lk(m_);
    rois_ = std::move(rois);
    roiRes_ = latest_ ? measure(*latest_, rois_) : std::vector<RoiResult>{};
}

std::vector<Roi> CaptureEngine::rois() const {
    std::lock_guard<std::mutex> lk(m_);
    return rois_;
}

void CaptureEngine::setRoiAlarm(size_t i, double above) {
    std::lock_guard<std::mutex> lk(m_);
    if (i < rois_.size()) rois_[i].alarmAboveC = above;
}

// ------------------------------------------------------------------ control
bool CaptureEngine::changeIrFormat(const std::string& fmt, std::string* err) {
    if (rec_.active()) {
        if (err) *err = "Stop recording before changing resolution";
        return false;
    }
    std::lock_guard<std::mutex> lk(acqMtx_);  // acquisition paused while we hold this
    A70Camera* c = camera();
    if (!c) {
        if (err) *err = "Not connected";
        return false;
    }
    try {
        c->setupRadiometric(fmt);
        cfg_.irFormat = fmt;
        {
            std::lock_guard<std::mutex> l2(m_);
            irFormatNow_ = c->irFormat();
            scaleNow_ = c->scaleK();
        }
        for (auto& w : c->takeWarnings()) log("warning", w);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

void CaptureEngine::setParamPersistent(const std::string& key, double v) {
    A70Camera* c = camera();
    if (!c) throw CameraError("Not connected");
    c->setParam(key, v);
    cfg_.params[key] = v;
}

bool CaptureEngine::captureVisual(std::vector<uint8_t>& rgb, int& w, int& h, std::string* err) {
    std::lock_guard<std::mutex> lk(acqMtx_);  // same mutex the acquisition loop holds around grab()
    A70Camera* c = camera();
    if (!c) {
        if (err) *err = "Not connected";
        return false;
    }
    return c->grabVisual(rgb, w, h, err);
}

std::optional<SnapshotPaths> CaptureEngine::snapshot(std::string* err, bool logMeasurements) {
    auto f = latest();
    auto fail = [&](const std::string& m) -> std::optional<SnapshotPaths> {
        if (err) *err = m;
        setError(ErrSnapshot, m);
        return std::nullopt;
    };
    if (!f) return fail("Snapshot: no frame yet");
    double age;
    {
        std::lock_guard<std::mutex> lk(m_);
        age = monoSeconds() - lastFrameMono_;
    }
    if (age > cfg_.staleAfterS) return fail("Snapshot refused: last frame is " + std::to_string(int(age)) + " s old");
    double mb = freeSpaceMB(cfg_.folder);
    if (mb >= 0 && mb < cfg_.minFreeMB) return fail("Snapshot refused: disk space low");
    try {
        Json md = Json::object();
        if (A70Camera* c = camera()) md = c->metadata();
        SnapshotOptions o;
        o.folder = cfg_.folder;
        o.prefix = cfg_.prefix;
        o.palette = cfg_.palette;
        o.csv = cfg_.csvWithSnapshots;
        std::vector<Roi> r = rois();
        std::vector<uint8_t> vrgb;
        int vw = 0, vh = 0;
        if (cfg_.captureVisualOnSnapshot) {
            std::string verr;
            if (!captureVisual(vrgb, vw, vh, &verr)) {
                vw = vh = 0;
                log("warning", "Visual companion capture skipped: " + verr);
            }
        }
        SnapshotPaths p = saveSnapshot(*f, md, o, r, vw > 0 ? vrgb.data() : nullptr, vw, vh);
        if (logMeasurements) {
            auto res = measure(*f, r);
            std::string lp = joinPath(cfg_.folder, cfg_.prefix + "_measurements_log.csv");
            std::lock_guard<std::mutex> lk(m_);
            measurementLog_ = appendMeasurementLog(lp, *f, computeStats(*f), r, res, p.tiff);
        }
        {
            std::lock_guard<std::mutex> lk(m_);
            ++snapshots_;
            lastSnapshot_ = p.tiff;
            if (lastErrorCode_ == ErrSnapshot) lastErrorCode_ = ErrNone;
        }
        log("info", "Saved " + p.tiff);
        return p;
    } catch (const std::exception& e) {
        return fail(std::string("Snapshot failed: ") + e.what());
    }
}

bool CaptureEngine::startRecording(int decimate, const std::string& compression, double durationS, std::string* err) {
    A70Camera* c = camera();
    if (!c) {
        if (err) *err = "Not connected";
        return false;
    }
    double mb = freeSpaceMB(cfg_.folder);
    if (mb >= 0 && mb < cfg_.minFreeMB) {
        if (err) *err = "Disk space low (" + std::to_string(int(mb)) + " MB)";
        setError(ErrDiskLow, "Recording refused: disk space low");
        return false;
    }
    try {
        Json md = c->metadata();
        md["decimation"] = decimate;
        std::string p = rec_.start(cfg_.folder, md, cfg_.prefix, decimate, compression);
        {
            std::lock_guard<std::mutex> lk(m_);
            recordStopAt_ = durationS > 0 ? monoSeconds() + durationS : 0;
            recOpen_ = true;
        }
        log("info", "Recording to " + p);
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        setError(ErrRecord, std::string("Record failed: ") + e.what());
        return false;
    }
}

void CaptureEngine::stopRecording() {
    rec_.stop();
    bool was;
    {
        std::lock_guard<std::mutex> lk(m_);
        recordStopAt_ = 0;
        was = recOpen_;
        recOpen_ = false;
    }
    if (was) {
        std::string msg = "Recording saved: " + rec_.path() + " (" + std::to_string(rec_.written()) + " frames";
        if (rec_.dropped()) msg += ", " + std::to_string(rec_.dropped()) + " dropped";
        msg += ")";
        if (!rec_.error().empty()) setError(ErrRecord, "Recording error: " + rec_.error());
        log("info", msg);
    }
}

void CaptureEngine::startInterval(double s) {
    std::lock_guard<std::mutex> lk(m_);
    intervalS_ = std::max(0.5, s);
    nextInterval_ = monoSeconds();  // first one immediately
}

void CaptureEngine::stopInterval() {
    std::lock_guard<std::mutex> lk(m_);
    intervalS_ = 0;
}

bool CaptureEngine::command(Command c, std::string* err) {
    try {
        switch (c) {
            case Command::Snapshot: return snapshot(err).has_value();
            case Command::StartRecording: return startRecording(1, "gzip", 0, err);
            case Command::StopRecording: stopRecording(); return true;
            case Command::Nuc: {
                A70Camera* cam = camera();
                if (!cam) throw CameraError("Not connected");
                cam->nuc();
                return true;
            }
            case Command::StartInterval: {
                double s;
                {
                    std::lock_guard<std::mutex> lk(m_);
                    s = intervalS_ > 0 ? intervalS_ : 60;
                }
                startInterval(s);
                return true;
            }
            case Command::StopInterval: stopInterval(); return true;
            case Command::Reconnect: {
                std::lock_guard<std::mutex> lk(acqMtx_);
                closeCamera();
                return true;
            }
            default:
                if (err) *err = "Unknown command";
                return false;
        }
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        setError(ErrCommand, std::string(toString(c)) + ": " + e.what());
        return false;
    }
}

}  // namespace a70
