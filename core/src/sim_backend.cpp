// Simulated FLIR A70: lets the whole application run before the hardware arrives.
// Emulates the GenICam features this app uses (Mono16 TemperatureLinear output,
// object parameters, NUC, frame rate, metadata rows below the 640x480 image).
// Scene: insulated pipe with a slowly breathing wet-insulation (CUI) patch,
// a hot uninsulated flange, and a moving hotspot so live view visibly updates.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#include "a70/backend.h"

namespace a70 {
namespace {

constexpr int W = 640, H = 480, META = 3;
constexpr double kAmbientK = 303.15;
// Fault injection for reliability tests: "SimCableUnplug" makes the camera vanish for
// 4 s (grabs time out, re-open fails), exactly like a pulled cable or a camera reboot.
std::atomic<double> g_faultUntil{0.0};
bool faulted() { return monoSeconds() < g_faultUntil.load(); }

class SimBackend : public Backend {
public:
    std::string name() const override { return "Simulator (no camera)"; }

    std::vector<DeviceInfo> listDevices() override {
        return {DeviceInfo{"sim0", "FLIR A70 (simulated)", "FLIR Systems", "SIM-0001", "127.0.0.1"}};
    }

    DeviceInfo open(const std::string&) override {
        if (faulted()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            throw CameraError("Simulated camera not reachable");
        }
        std::lock_guard<std::mutex> lk(m_);
        feats_.clear();
        order_.clear();
        add("DeviceVendorName", FeatureType::String, "FLIR Systems", "DeviceControl", false);
        add("DeviceModelName", FeatureType::String, "FLIR A70 (simulated)", "DeviceControl", false);
        add("DeviceSerialNumber", FeatureType::String, "SIM-0001", "DeviceControl", false);
        add("DeviceFirmwareVersion", FeatureType::String, "sim-1.0", "DeviceControl", false);
        add("DeviceTemperature", FeatureType::Float, "309.2", "DeviceControl", false, {}, "K");
        add("Width", FeatureType::Integer, "640", "ImageFormatControl", false);
        add("WidthMax", FeatureType::Integer, "640", "ImageFormatControl", false);
        add("Height", FeatureType::Integer, std::to_string(H + META), "ImageFormatControl", true);
        add("HeightMax", FeatureType::Integer, std::to_string(H + META), "ImageFormatControl", false);
        add("PixelFormat", FeatureType::Enumeration, "Mono16", "ImageFormatControl", true,
            {"Mono8", "Mono16", "YUV422_8_UYVY"});
        add("ImageMode", FeatureType::Enumeration, "Thermal", "IRControl", true, {"Thermal", "Visual"});
        add("IRFormat", FeatureType::Enumeration, "TemperatureLinear10mK", "IRControl", true,
            {"Radiometric", "TemperatureLinear10mK", "TemperatureLinear100mK"});
        add("CurrentCase", FeatureType::Enumeration, "-20C to 175C", "IRControl", true,
            {"-20C to 175C", "-20C to 250C", "175C to 1000C"}, "",
            "Simulated measurement range selector (real name may differ)");
        add("ObjectEmissivity", FeatureType::Float, "0.95", "ObjectParameters", true);
        add("ReflectedTemperature", FeatureType::Float, "303.15", "ObjectParameters", true, {}, "K");
        add("AtmosphericTemperature", FeatureType::Float, "303.15", "ObjectParameters", true, {}, "K");
        add("ObjectDistance", FeatureType::Float, "3.0", "ObjectParameters", true, {}, "m");
        add("RelativeHumidity", FeatureType::Float, "0.75", "ObjectParameters", true);
        add("AcquisitionFrameRate", FeatureType::Float, "30", "AcquisitionControl", true, {}, "Hz");
        add("NUCAction", FeatureType::Command, "", "IRControl", true);
        add("GevSCPSPacketSize", FeatureType::Integer, "1500", "TransportLayerControl", true);
        add("SimCableUnplug", FeatureType::Command, "", "Simulator", true, {}, "",
            "Fault injection: camera disappears for 4 s");
        buildStatic();
        open_ = true;
        t0_ = monoSeconds();
        return listDevices()[0];
    }

    void close() override {
        std::lock_guard<std::mutex> lk(m_);
        streaming_ = false;
        open_ = false;
    }
    bool isOpen() const override { return open_; }

    bool has(const std::string& n) override {
        std::lock_guard<std::mutex> lk(m_);
        return feats_.count(n) > 0;
    }
    FeatureType type(const std::string& n) override { return get(n).type; }

    std::string getString(const std::string& n) override {
        std::lock_guard<std::mutex> lk(m_);
        return get(n).value;
    }

    void setString(const std::string& n, const std::string& v) override {
        std::lock_guard<std::mutex> lk(m_);
        Feature& f = get(n);
        if (!f.writable || f.type == FeatureType::Command) throw CameraError(n + " is read-only");
        if (f.type == FeatureType::Enumeration &&
            std::find(f.choices.begin(), f.choices.end(), v) == f.choices.end())
            throw CameraError(n + ": '" + v + "' is not a valid value");
        if (f.type == FeatureType::Integer || f.type == FeatureType::Float) {
            double d;
            try {
                d = std::stod(v);
            } catch (...) {
                throw CameraError(n + ": not a number");
            }
            if (n == "Height" && (d < 1 || d > H + META)) throw CameraError("Height out of range");
            if (n == "AcquisitionFrameRate" && (d < 1 || d > 30)) throw CameraError("Frame rate out of range 1..30");
            if (n == "ObjectEmissivity" && (d < 0.01 || d > 1.0)) throw CameraError("Emissivity out of range");
            f.value = f.type == FeatureType::Integer ? std::to_string(static_cast<long long>(d)) : fmtDouble(d, 10);
        } else {
            f.value = v;
        }
    }

    void execute(const std::string& n) override {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (get(n).type != FeatureType::Command) throw CameraError(n + " is not a command");
            if (n == "NUCAction")
                for (auto& v : fpn_) v *= 0.2f;  // NUC mostly removes fixed-pattern noise
            if (n == "SimCableUnplug") {
                g_faultUntil = monoSeconds() + 4.0;
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    std::vector<std::string> choices(const std::string& n) override {
        std::lock_guard<std::mutex> lk(m_);
        return get(n).choices;
    }

    std::vector<Feature> features() override {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<Feature> v;
        for (auto& n : order_) v.push_back(feats_[n]);
        return v;
    }

    void start() override {
        if (getString("PixelFormat") != "Mono16") throw CameraError("Simulator only streams Mono16");
        streaming_ = true;
        next_ = monoSeconds();
    }
    void stop() override { streaming_ = false; }

    bool grab(RawFrame& out, double timeoutS) override {
        if (!streaming_) throw CameraError("Not streaming");
        if (faulted()) {
            std::this_thread::sleep_for(std::chrono::duration<double>(std::min(timeoutS, 1.0)));
            return false;
        }
        double fps, eps, tr, step;
        int h;
        {
            std::lock_guard<std::mutex> lk(m_);
            fps = std::max(1.0, std::stod(feats_["AcquisitionFrameRate"].value));
            eps = std::stod(feats_["ObjectEmissivity"].value);
            tr = std::stod(feats_["ReflectedTemperature"].value);
            step = feats_["IRFormat"].value == "TemperatureLinear10mK" ? 0.01 : 0.1;
            h = std::stoi(feats_["Height"].value);
        }
        double now = monoSeconds();
        if (next_ > now) std::this_thread::sleep_for(std::chrono::duration<double>(next_ - now));
        next_ = std::max(next_ + 1.0 / fps, monoSeconds() - 1.0 / fps);
        double t = monoSeconds() - t0_;

        // dynamic scene on top of the static base
        std::vector<float>& T = work_;
        T = base_;
        double r = 30 + 10 * std::sin(t / 20);
        for (int y = 180; y <= 320; ++y)  // wet patch only inside the pipe band
            for (int x = std::max(0, int(220 - 5 * r)); x < std::min(W, int(220 + 5 * r)); ++x) {
                double dx = (x - 220) / (1.6 * r), dy = (y - 275) / r;
                T[y * W + x] += float(5.5 * std::exp(-(dx * dx + dy * dy)));
            }
        double cx = 320 + 200 * std::cos(t / 3), cy = 90 + 20 * std::sin(t / 2);
        for (int y = std::max(0, int(cy) - 30); y < std::min(H, int(cy) + 30); ++y)
            for (int x = std::max(0, int(cx) - 30); x < std::min(W, int(cx) + 30); ++x) {
                double d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                T[y * W + x] += float(25 * std::exp(-d2 / 72.0));
            }

        out.width = W;
        out.height = h;
        out.data.assign(size_t(W) * h, 0);
        const double eT = 0.95, trT = kAmbientK, tr4 = trT * trT * trT * trT, ts4 = tr * tr * tr * tr;
        size_t noff = static_cast<size_t>(rng_()) % (noise_.size() - size_t(W) * H);
        std::lock_guard<std::mutex> lk(m_);
        for (int y = 0; y < std::min(h, H); ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = size_t(y) * W + x;
                // mild lens/atmosphere vignetting: attenuates the delta-from-ambient, not ambient itself
                double Ti = kAmbientK + (T[i] - kAmbientK) * vignette_[i];
                double W4 = eT * Ti * Ti * Ti * Ti + (1 - eT) * tr4;
                double obj4 = (W4 - (1 - eps) * ts4) / std::max(eps, 0.01);
                double Ta = std::pow(std::max(obj4, 1.0), 0.25) + fpn_[i] + noise_[noff + i];
                out.data[i] = static_cast<uint16_t>(std::clamp(std::lround(Ta / step), 0L, 65535L));
            }
        for (int y = H; y < h; ++y)
            for (int x = 0; x < W; ++x) out.data[size_t(y) * W + x] = static_cast<uint16_t>(x);
        out.hostTimeNs = nowNs();
        out.cameraTimestamp = static_cast<uint64_t>(t * 1e9);
        out.frameId = ++n_;
        return true;
    }

    std::string stats() override { return "frames " + std::to_string(n_); }

private:
    Feature& get(const std::string& n) {
        auto it = feats_.find(n);
        if (it == feats_.end()) throw CameraError("Feature " + n + " not found");
        return it->second;
    }

    void add(const std::string& n, FeatureType t, const std::string& v, const std::string& cat, bool w,
             std::vector<std::string> ch = {}, const std::string& unit = "", const std::string& desc = "") {
        Feature f;
        f.name = n;
        f.type = t;
        f.value = v;
        f.category = cat;
        f.writable = w;
        f.choices = std::move(ch);
        f.unit = unit;
        f.description = desc;
        feats_[n] = f;
        order_.push_back(n);
    }

    // Builds the static (non-time-varying) scene: ambient gradient, pipe band, flange,
    // plus the texture a real radiometric capture of an insulated line would show —
    // mottled jacket surface, circumferential cladding seams, and support-clamp cold
    // spots from thermal bridging. Also precomputes the per-pixel vignette and the
    // fixed-pattern/sensor noise fields. Everything here is seeded deterministically
    // (fixed RNG seeds, no wall-clock input), so two freshly-opened simulator instances
    // produce bit-identical static scenes — tests/test_core.cpp relies on this.
    void buildStatic() {
        base_.assign(size_t(W) * H, 0.f);
        vignette_.assign(size_t(W) * H, 1.f);
        const double amb = kAmbientK;
        const double cx0 = W / 2.0, cy0 = H / 2.0, rmax2 = cx0 * cx0 + cy0 * cy0;
        constexpr double kVignetteStrength = 0.08;  // attenuation of the ambient delta at the frame corners
        // two support-clamp cold bands: thermal bridging through the insulation at the strap location,
        // kept clear of the demo flange (x in 458..482) and of tests/test_core.cpp's alarm box (x in 160..290)
        constexpr double kClampX[2] = {90.0, 400.0};
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                double v = amb - 0.004 * y;
                double dy = (y - 250) / 70.0;
                if (std::fabs(dy) < 1) {
                    double shade = 1 - dy * dy;
                    v = amb + 1.5 + 2.5 * shade;
                    if (std::abs(x - 470) < 12) v = 358.15 - 6 * (1 - shade);
                    // mottled insulation jacket texture: low-frequency, not per-pixel noise
                    v += 0.08 * std::sin(x / 17.0 + y / 23.0) + 0.07 * std::sin(x / 41.0 - y / 11.0 + 1.3);
                    // circumferential cladding seams, roughly every 42 px along the pipe
                    double seamPhase = std::fmod(double(y - 180), 42.0);
                    v -= 0.25 * std::exp(-std::pow((seamPhase - 1.0) / 1.2, 2.0));
                    // support-clamp thermal bridging: localized cool band, strongest mid-band
                    for (double scx : kClampX) {
                        double dxs = (x - scx) / 6.0, dys = (y - 250) / 80.0;
                        v -= 1.5 * std::exp(-(dxs * dxs + dys * dys));
                    }
                }
                base_[size_t(y) * W + x] = float(v);
                double ddx = x - cx0, ddy = y - cy0;
                vignette_[size_t(y) * W + x] = float(1.0 - kVignetteStrength * (ddx * ddx + ddy * ddy) / rmax2);
            }
        std::mt19937 g(0);
        std::normal_distribution<float> fp(0.f, 0.06f), nz(0.f, 0.04f), colBiasD(0.f, 0.03f);
        fpn_.resize(size_t(W) * H);
        for (auto& v : fpn_) v = fp(g);
        std::vector<float> colBias(W);  // microbolometer column-wise fixed-pattern non-uniformity
        for (auto& v : colBias) v = colBiasD(g);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) fpn_[size_t(y) * W + x] += colBias[x];
        noise_.resize(size_t(W) * H * 3);
        for (auto& v : noise_) v = nz(g);
        // one-time spatial correlation (3x3 cross blur) so sensor noise reads as grain,
        // not independent salt-and-pepper per pixel — cheap because it runs once, not per frame
        std::vector<float> blurred(noise_.size());
        const size_t total = noise_.size();
        for (size_t i = 0; i < total; ++i) {
            int x = int(i % W);
            float sum = noise_[i];
            int cnt = 1;
            if (x > 0) { sum += noise_[i - 1]; ++cnt; }
            if (x < W - 1) { sum += noise_[i + 1]; ++cnt; }
            if (i >= size_t(W)) { sum += noise_[i - W]; ++cnt; }
            if (i + W < total) { sum += noise_[i + W]; ++cnt; }
            blurred[i] = sum / cnt;
        }
        noise_.swap(blurred);
    }

    std::mutex m_;
    std::map<std::string, Feature> feats_;
    std::vector<std::string> order_;
    std::vector<float> base_, work_, fpn_, noise_, vignette_;
    std::mt19937 rng_{1};
    bool open_ = false, streaming_ = false;
    double t0_ = 0, next_ = 0;
    uint64_t n_ = 0;
};

}  // namespace

std::unique_ptr<Backend> makeSimBackend() { return std::make_unique<SimBackend>(); }

}  // namespace a70
