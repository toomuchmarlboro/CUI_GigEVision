#pragma once
// Basic types shared by every part of the A70 toolkit.
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace a70 {

struct CameraError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct DeviceInfo {
    std::string id;       // what you pass to open()
    std::string model;
    std::string vendor;
    std::string serial;
    std::string address;  // IP if known
    std::string label() const;
};

enum class FeatureType { Integer, Float, Boolean, String, Enumeration, Command, Category, Register, Other };
const char* toString(FeatureType t);

struct Feature {
    std::string name;
    std::string category;
    FeatureType type = FeatureType::Other;
    std::string value;
    bool readable = true;
    bool writable = false;
    std::vector<std::string> choices;
    std::string unit;
    std::string description;
};

// One frame as delivered by a transport: raw Mono16 payload, possibly with metadata rows.
struct RawFrame {
    std::vector<uint16_t> data;
    int width = 0;
    int height = 0;
    int64_t hostTimeNs = 0;
    uint64_t cameraTimestamp = 0;
    uint64_t frameId = 0;
};

int64_t nowNs();                       // wall clock, ns since epoch
double monoSeconds();                  // steady clock, seconds
std::string trim(const std::string& s);
std::string fmtDouble(double v, int prec = 6);

}  // namespace a70
