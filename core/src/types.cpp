#include "a70/types.h"

#include <chrono>
#include <cstdio>

namespace a70 {

std::string DeviceInfo::label() const {
    std::string s;
    if (!vendor.empty()) s += vendor;
    if (!model.empty()) s += (s.empty() ? "" : " ") + model;
    if (s.empty()) s = id;
    if (!serial.empty()) s += "  SN " + serial;
    if (!address.empty()) s += "  @ " + address;
    return s;
}

const char* toString(FeatureType t) {
    switch (t) {
        case FeatureType::Integer: return "Integer";
        case FeatureType::Float: return "Float";
        case FeatureType::Boolean: return "Boolean";
        case FeatureType::String: return "String";
        case FeatureType::Enumeration: return "Enumeration";
        case FeatureType::Command: return "Command";
        case FeatureType::Category: return "Category";
        case FeatureType::Register: return "Register";
        default: return "Other";
    }
}

int64_t nowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

double monoSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string fmtDouble(double v, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    return buf;
}

}  // namespace a70
