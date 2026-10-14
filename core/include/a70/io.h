#pragma once
// Lossless radiometric file formats.
//   Snapshot:  <stem>.tiff  uint16 counts, metadata JSON in ImageDescription (tifffile/ImageJ/MATLAB readable)
//              <stem>.json  metadata + stats + measurement tools
//              <stem>.png   false-colour preview (not for analysis)
//              <stem>_degC.csv (optional) full °C matrix
//   Recording: <stem>.h5    see recorder.h
//   Always:    T[°C] = counts * scale_K_per_count - 273.15
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "a70/json.h"
#include "a70/types.h"
#include "a70/thermal.h"

namespace a70 {

std::string timestampStem(const std::string& prefix, int64_t ns = 0);
std::string joinPath(const std::string& a, const std::string& b);
void makeDirs(const std::string& path);
double freeSpaceMB(const std::string& path);  // -1 if unknown

void writeTiff16(const std::string& path, const uint16_t* data, int w, int h, const std::string& description);
// Reads our own uint16-counts TIFFs, and also accepts externally-sourced 32-bit IEEE-float
// radiometric TIFFs (a common convention in other thermal software, where each pixel is
// already calibrated °C) — re-encoded transparently into our own 0.01 K/count counts, so
// callers always get the same uint16/description contract regardless of source.
bool readTiff16(const std::string& path, std::vector<uint16_t>& data, int& w, int& h, std::string& description);
void writePngRgb(const std::string& path, const uint8_t* rgb, int w, int h);
void writeCsvCelsius(const std::string& path, const ThermalFrame& f);

Json statsJson(const FrameStats& s);
Json roiResultsJson(const std::vector<Roi>& rois, const std::vector<RoiResult>& res);

struct SnapshotPaths {
    std::string tiff, json, png, csv;
    std::string visual;  // "<stem>_visual.png" — empty if no visual companion was captured
};

struct SnapshotOptions {
    std::string folder = ".";
    std::string prefix = "a70";
    std::string palette = "Iron";
    double lo = NAN, hi = NAN;  // NaN = frame min/max
    bool csv = false;
};

// visualRgb/visualW/visualH: optional visual-light companion image (see
// A70Camera::grabVisual) — pass visualW/visualH == 0 (the default) to skip it.
SnapshotPaths saveSnapshot(const ThermalFrame& f, Json metadata, const SnapshotOptions& opt,
                           const std::vector<Roi>& rois = {}, const uint8_t* visualRgb = nullptr,
                           int visualW = 0, int visualH = 0);

// Append a row to a CSV measurement log (creates the header on first write). If the
// existing file has different columns (tools changed) a new timestamped file is started.
// Returns the path actually written.
std::string appendMeasurementLog(const std::string& path, const ThermalFrame& f, const FrameStats& st,
                          const std::vector<Roi>& rois, const std::vector<RoiResult>& res,
                          const std::string& snapshotFile);

std::string localTimeString(int64_t ns);

}  // namespace a70
