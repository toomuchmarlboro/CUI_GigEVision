#pragma once
// FLIR A50/A70-specific layer on top of a generic GenICam backend.
//
// With IRFormat = TemperatureLinear10mK / 100mK and PixelFormat = Mono16 the camera
// streams absolute temperature:  T[°C] = counts * 0.01 (or 0.1) - 273.15.
// Object parameters (emissivity, reflected temp, ...) are applied INSIDE the camera
// before encoding, so set them before capturing; they are stored in every file.
// The A70 appends a few metadata rows below the image; we request Height = sensor
// height and crop in software if the camera refuses.
//
// GenICam names: IRFormat, ImageMode, ObjectEmissivity, ReflectedTemperature,
// AtmosphericTemperature are documented for the A70. The rest are best-guess
// candidate lists — run `a70_cli features` on the real unit and extend if needed.
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "a70/backend.h"
#include "a70/json.h"
#include "a70/thermal.h"

namespace a70 {

struct ParamSpec {
    std::string key;                       // logical name
    std::vector<std::string> candidates;   // GenICam names to try
    bool isTemperature;                    // camera may use K or °C; we always expose °C
    std::string label, unit;
    double min, max, step;
    int decimals;
};
const std::vector<ParamSpec>& paramSpecs();
extern const std::vector<std::string> kNucCommands;
extern const std::vector<std::string> kRangeFeatures;
extern const std::vector<std::string> kFrameRateFeatures;

double irFormatScale(const std::string& irFormat);  // throws for unsupported formats

class A70Camera {
public:
    explicit A70Camera(const std::string& backendKey, int packetSize = 0);
    ~A70Camera();

    std::vector<DeviceInfo> listDevices();
    DeviceInfo open(const std::string& deviceId, const std::string& irFormat = "TemperatureLinear10mK");
    void close();
    bool isOpen() const;

    void setupRadiometric(const std::string& irFormat);
    void start();
    void stop();
    bool streaming() const { return streaming_; }
    bool grab(ThermalFrame& out, double timeoutS = 2.0);  // acquisition thread only

    // object parameters in °C / m / fraction
    std::vector<std::string> paramKeys();
    std::optional<double> getParam(const std::string& key);
    void setParam(const std::string& key, double value);
    std::map<std::string, double> getParams();

    bool hasNuc();
    void nuc();
    std::optional<double> getFrameRate();
    void setFrameRate(double fps);
    std::vector<std::string> ranges();
    std::optional<std::string> getRange();
    void setRange(const std::string& v);

    Json metadata();

    Backend& backend() { return *be_; }
    std::recursive_mutex& mutex() { return mtx_; }  // hold while using backend() directly

    const std::string& backendKey() const { return backendKey_; }
    const DeviceInfo& info() const { return info_; }
    const std::string& irFormat() const { return irFormat_; }
    double scaleK() const { return scaleK_; }
    double maxTempC() const { return 65535 * scaleK_ - KELVIN; }
    int sensorWidth() const { return sensorW_; }
    int sensorHeight() const { return sensorH_; }
    std::vector<std::string> takeWarnings();

private:
    std::string first(const std::vector<std::string>& names);
    bool trySet(const std::string& name, const std::string& value);

    std::string backendKey_;
    std::unique_ptr<Backend> be_;
    int packetSize_;
    DeviceInfo info_;
    std::string irFormat_ = "TemperatureLinear10mK";
    double scaleK_ = 0.01;
    int sensorW_ = 640, sensorH_ = 480;
    bool streaming_ = false;
    std::recursive_mutex mtx_;
    std::map<std::string, std::string> resolved_;
    std::map<std::string, bool> tempIsK_;
    std::vector<std::string> warnings_;
    RawFrame raw_;
};

}  // namespace a70
