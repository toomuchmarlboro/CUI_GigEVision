#pragma once
// Temperature frames, statistics, measurement tools and false-colour palettes.
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace a70 {

constexpr double KELVIN = 273.15;

struct ThermalFrame {
    std::vector<uint16_t> counts;  // image rows only, row-major
    int width = 0;
    int height = 0;
    double scaleK = 0.01;          // kelvin per count (TemperatureLinear10mK)
    int64_t hostTimeNs = 0;
    uint64_t cameraTimestamp = 0;
    uint64_t frameId = 0;

    double celsius(uint16_t c) const { return c * scaleK - KELVIN; }
    double at(int x, int y) const { return celsius(counts[size_t(y) * width + x]); }
    void toCelsius(std::vector<float>& out) const;
};

struct FrameStats {
    double minC = 0, maxC = 0, meanC = 0;
    int maxX = 0, maxY = 0, minX = 0, minY = 0;
};
FrameStats computeStats(const ThermalFrame& f);

// Spot or box measurement tool. Coordinates in sensor pixels, inclusive.
struct Roi {
    enum class Kind { Spot, Box };
    std::string name;
    Kind kind = Kind::Box;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // spot: x0,y0
    double alarmAboveC = std::numeric_limits<double>::quiet_NaN();  // NaN = no alarm
};

struct RoiResult {
    std::string name;
    Roi::Kind kind = Roi::Kind::Box;
    double maxC = 0, minC = 0, meanC = 0;
    int maxX = 0, maxY = 0;
    bool alarm = false;
    bool valid = false;
};
std::vector<RoiResult> measure(const ThermalFrame& f, const std::vector<Roi>& rois);
Roi clampRoi(Roi r, int w, int h);

// Palettes
std::vector<std::string> paletteNames();
const std::array<std::array<uint8_t, 3>, 256>& paletteLut(const std::string& name);
// temp (°C, w*h) -> RGB888 (w*h*3)
void colorize(const float* tempC, size_t n, double lo, double hi, const std::string& palette, uint8_t* rgbOut);
// robust display range ignoring the hottest/coldest pct %, optionally inside a box
void autoRange(const float* tempC, int w, int h, double& lo, double& hi, double pct = 0.5,
               const Roi* box = nullptr);

}  // namespace a70
