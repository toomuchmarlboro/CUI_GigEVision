#include "a70/camera.h"

#include <chrono>
#include <ctime>

namespace a70 {

const std::vector<ParamSpec>& paramSpecs() {
    static const std::vector<ParamSpec> v = {
        {"emissivity", {"ObjectEmissivity", "Emissivity"}, false, "Emissivity", "", 0.01, 1.0, 0.01, 2},
        {"reflected_temp", {"ReflectedTemperature", "ReflectedTemp"}, true, "Reflected temp", "°C", -60, 600, 0.5, 1},
        {"atmospheric_temp", {"AtmosphericTemperature", "AtmTemperature"}, true, "Atmospheric temp", "°C", -60, 200, 0.5, 1},
        {"distance", {"ObjectDistance", "Distance"}, false, "Distance", "m", 0, 10000, 0.5, 1},
        {"humidity", {"RelativeHumidity", "Humidity"}, false, "Rel. humidity", "", 0, 100, 0.05, 2},
        {"ext_optics_temp", {"ExtOpticsTemperature", "WindowTemperature"}, true, "Ext. optics temp", "°C", -60, 600, 0.5, 1},
        {"ext_optics_trans", {"ExtOpticsTransmission", "WindowTransmission"}, false, "Ext. optics transm.", "", 0.01, 1.0, 0.01, 2},
    };
    return v;
}
const std::vector<std::string> kNucCommands = {"NUCAction", "NUCActionLong", "NucAction", "PerformNUC"};
const std::vector<std::string> kRangeFeatures = {"CurrentCase", "MeasurementRange", "TemperatureRange", "CaseSelector"};
const std::vector<std::string> kFrameRateFeatures = {"AcquisitionFrameRate", "FrameRate"};

static const ParamSpec& spec(const std::string& key) {
    for (auto& s : paramSpecs())
        if (s.key == key) return s;
    throw CameraError("Unknown parameter " + key);
}

double irFormatScale(const std::string& f) {
    if (f == "TemperatureLinear10mK") return 0.01;
    if (f == "TemperatureLinear100mK") return 0.1;
    throw CameraError("Unsupported IRFormat " + f + " (use TemperatureLinear10mK or TemperatureLinear100mK)");
}

static int sensorHeightForWidth(int w) {
    switch (w) {
        case 640: return 480;
        case 464: return 348;
        case 320: return 240;
        case 160: return 120;
        default: return 0;
    }
}

A70Camera::A70Camera(const std::string& backendKey, int packetSize)
    : backendKey_(backendKey), be_(makeBackend(backendKey)), packetSize_(packetSize) {}

A70Camera::~A70Camera() {
    try {
        close();
    } catch (...) {
    }
}

std::vector<DeviceInfo> A70Camera::listDevices() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return be_->listDevices();
}

DeviceInfo A70Camera::open(const std::string& id, const std::string& irFormat) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    info_ = be_->open(id);
    auto fill = [&](std::string& field, const char* feat) {
        if (field.empty() && be_->has(feat)) {
            try {
                field = be_->getString(feat);
            } catch (const CameraError&) {
            }
        }
    };
    fill(info_.model, "DeviceModelName");
    fill(info_.serial, "DeviceSerialNumber");
    fill(info_.vendor, "DeviceVendorName");
    resolved_.clear();
    tempIsK_.clear();
    setupRadiometric(irFormat);
    return info_;
}

void A70Camera::close() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    stop();
    if (be_->isOpen()) be_->close();
}

bool A70Camera::isOpen() const { return be_->isOpen(); }

std::string A70Camera::first(const std::vector<std::string>& names) {
    std::string key;
    for (auto& n : names) key += n + "|";
    auto it = resolved_.find(key);
    if (it != resolved_.end()) return it->second;
    std::string found;
    for (auto& n : names)
        if (be_->has(n)) {
            found = n;
            break;
        }
    resolved_[key] = found;
    return found;
}

bool A70Camera::trySet(const std::string& name, const std::string& value) {
    if (!be_->has(name)) return false;
    try {
        be_->setString(name, value);
        return true;
    } catch (const CameraError& e) {
        warnings_.push_back(e.what());
        return false;
    }
}

std::vector<std::string> A70Camera::takeWarnings() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto w = std::move(warnings_);
    warnings_.clear();
    return w;
}

void A70Camera::setupRadiometric(const std::string& irFormat) {
    double scale = irFormatScale(irFormat);
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    bool was = streaming_;
    if (was) stop();
    trySet("ImageMode", "Thermal");
    if (be_->has("IRFormat")) {
        be_->setString("IRFormat", irFormat);
    } else {
        warnings_.push_back("Camera has no IRFormat feature — is GigE Vision streaming enabled? "
                            "Temperatures may be wrong.");
    }
    if (be_->has("PixelFormat")) be_->setString("PixelFormat", "Mono16");
    irFormat_ = irFormat;
    scaleK_ = scale;
    sensorW_ = be_->has("Width") ? int(be_->getInt("Width")) : 640;
    int h = sensorHeightForWidth(sensorW_);
    if (h == 0) h = be_->has("Height") ? int(be_->getInt("Height")) : 480;
    sensorH_ = h;
    trySet("Height", std::to_string(sensorH_));
    if (packetSize_ > 0) trySet("GevSCPSPacketSize", std::to_string(packetSize_));
    if (was) start();
}

void A70Camera::start() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (streaming_) return;
    be_->configureStream();
    be_->start();
    streaming_ = true;
}

void A70Camera::stop() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!streaming_) return;
    streaming_ = false;
    be_->stop();
}

bool A70Camera::grab(ThermalFrame& out, double timeoutS) {
    if (!be_->grab(raw_, timeoutS)) return false;
    int w = std::min(raw_.width, sensorW_), h = std::min(raw_.height, sensorH_);
    out.width = w;
    out.height = h;
    out.scaleK = scaleK_;
    out.hostTimeNs = raw_.hostTimeNs;
    out.cameraTimestamp = raw_.cameraTimestamp;
    out.frameId = raw_.frameId;
    out.counts.resize(size_t(w) * h);
    if (w == raw_.width) {
        std::copy(raw_.data.begin(), raw_.data.begin() + size_t(w) * h, out.counts.begin());
    } else {
        for (int y = 0; y < h; ++y)
            std::copy(raw_.data.begin() + size_t(y) * raw_.width, raw_.data.begin() + size_t(y) * raw_.width + w,
                      out.counts.begin() + size_t(y) * w);
    }
    return true;
}

std::vector<std::string> A70Camera::paramKeys() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<std::string> v;
    for (auto& s : paramSpecs())
        if (!first(s.candidates).empty()) v.push_back(s.key);
    return v;
}

std::optional<double> A70Camera::getParam(const std::string& key) {
    const ParamSpec& s = spec(key);
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string n = first(s.candidates);
    if (n.empty()) return std::nullopt;
    double v = be_->getFloat(n);
    if (s.isTemperature) {
        auto it = tempIsK_.find(n);
        bool isK = it != tempIsK_.end() ? it->second : (tempIsK_[n] = v > 150.0);
        return isK ? v - KELVIN : v;
    }
    return v;
}

void A70Camera::setParam(const std::string& key, double value) {
    const ParamSpec& s = spec(key);
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string n = first(s.candidates);
    if (n.empty()) throw CameraError("Camera has no " + key + " feature");
    if (s.isTemperature) {
        if (!tempIsK_.count(n)) getParam(key);
        if (tempIsK_[n]) value += KELVIN;
    }
    be_->setFloat(n, value);
}

std::map<std::string, double> A70Camera::getParams() {
    std::map<std::string, double> m;
    for (auto& k : paramKeys()) {
        try {
            if (auto v = getParam(k)) m[k] = *v;
        } catch (const CameraError&) {
        }
    }
    return m;
}

bool A70Camera::hasNuc() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return !first(kNucCommands).empty();
}

void A70Camera::nuc() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string n = first(kNucCommands);
    if (n.empty()) throw CameraError("No NUC command found on this camera");
    be_->execute(n);
}

std::optional<double> A70Camera::getFrameRate() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string n = first(kFrameRateFeatures);
    if (n.empty()) return std::nullopt;
    return be_->getFloat(n);
}

void A70Camera::setFrameRate(double fps) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string n = first(kFrameRateFeatures);
    if (n.empty()) throw CameraError("Frame rate is not adjustable on this camera");
    be_->setFloat(n, fps);
}

std::vector<std::string> A70Camera::ranges() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string n = first(kRangeFeatures);
    return n.empty() ? std::vector<std::string>{} : be_->choices(n);
}

std::optional<std::string> A70Camera::getRange() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string n = first(kRangeFeatures);
    if (n.empty()) return std::nullopt;
    return be_->getString(n);
}

void A70Camera::setRange(const std::string& v) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string n = first(kRangeFeatures);
    if (n.empty()) throw CameraError("No measurement-range feature found");
    be_->setString(n, v);
}

static std::string isoUtcNow() {
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char b[40];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

Json A70Camera::metadata() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    Json md = Json::object();
    md["camera_model"] = info_.model;
    md["camera_serial"] = info_.serial;
    md["camera_vendor"] = info_.vendor;
    md["backend"] = backendKey_;
    md["ir_format"] = irFormat_;
    md["scale_K_per_count"] = scaleK_;
    md["offset_K"] = 0.0;
    md["conversion"] = "T_degC = counts * scale_K_per_count - 273.15";
    md["width"] = sensorW_;
    md["height"] = sensorH_;
    md["captured_utc"] = isoUtcNow();
    md["software"] = std::string("a70_thermal C++ ") + A70_VERSION;
    Json p = Json::object();
    for (auto& kv : getParams()) p[kv.first] = kv.second;
    md["object_parameters"] = p;
    try {
        if (auto f = getFrameRate()) md["frame_rate_hz"] = *f;
    } catch (const CameraError&) {
    }
    try {
        if (auto r = getRange()) md["measurement_range"] = *r;
    } catch (const CameraError&) {
    }
    for (const char* n : {"DeviceFirmwareVersion", "DeviceTemperature"}) {
        try {
            if (be_->has(n)) md[n] = be_->getString(n);
        } catch (const CameraError&) {
        }
    }
    return md;
}

}  // namespace a70
