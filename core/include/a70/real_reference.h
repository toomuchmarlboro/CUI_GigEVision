#pragma once
// Real (not simulated) radiometric reference capture, served by the simulator backend's
// "RealReferenceCapture" feature. See core/src/real_reference_data.cpp for provenance —
// that one file is GPL-3.0 licensed (derived from a real FLIR camera capture); nothing
// else in this project is.
#include <cstdint>

namespace a70 {

constexpr int kRealReferenceW = 640;
constexpr int kRealReferenceH = 480;

// 640*480 counts in this project's documented 0.01 K/count format
// (T[C] = counts * 0.01 - 273.15), i.e. interchangeable with TemperatureLinear10mK data.
extern const uint16_t kRealReferenceCounts640x480[kRealReferenceW * kRealReferenceH];

}  // namespace a70
