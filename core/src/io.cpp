#include "a70/io.h"

#include <zlib.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace a70 {

static std::tm localTm(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string localTimeString(int64_t ns) {
    std::tm tm = localTm(std::time_t(ns / 1000000000LL));
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

std::string timestampStem(const std::string& prefix, int64_t ns) {
    if (ns <= 0) ns = nowNs();
    std::tm tm = localTm(std::time_t(ns / 1000000000LL));
    char b[64];
    std::strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tm);
    char ms[8];
    std::snprintf(ms, sizeof ms, "_%03d", int((ns / 1000000) % 1000));
    return prefix + "_" + b + ms;
}

std::string joinPath(const std::string& a, const std::string& b) { return (fs::path(a) / b).string(); }

void makeDirs(const std::string& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec && !fs::is_directory(p)) throw std::runtime_error("Cannot create folder " + p + ": " + ec.message());
}

double freeSpaceMB(const std::string& p) {
    std::error_code ec;
    auto s = fs::space(fs::exists(p) ? fs::path(p) : fs::path(p).parent_path(), ec);
    if (ec) return -1;
    return double(s.available) / 1e6;
}

// ------------------------------------------------------------------ TIFF (baseline, uncompressed, LE)
void writeTiff16(const std::string& path, const uint16_t* data, int w, int h, const std::string& desc) {
    std::string d = desc;
    d.push_back('\0');
    const uint32_t nTags = 11;
    const uint32_t ifdOff = 8;
    const uint32_t ifdSize = 2 + nTags * 12 + 4;
    const uint32_t descOff = ifdOff + ifdSize;
    uint32_t dataOff = descOff + uint32_t(d.size());
    dataOff = (dataOff + 15) & ~15u;
    const uint32_t dataBytes = uint32_t(w) * uint32_t(h) * 2;

    std::vector<uint8_t> b;
    auto u16 = [&](uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); };
    auto tag = [&](uint16_t id, uint16_t type, uint32_t count, uint32_t value) {
        u16(id); u16(type); u32(count);
        if (type == 3 && count == 1) { u16(uint16_t(value)); u16(0); } else u32(value);
    };
    b.insert(b.end(), {'I', 'I', 42, 0});
    u32(ifdOff);
    u16(uint16_t(nTags));
    tag(256, 4, 1, uint32_t(w));                 // ImageWidth
    tag(257, 4, 1, uint32_t(h));                 // ImageLength
    tag(258, 3, 1, 16);                          // BitsPerSample
    tag(259, 3, 1, 1);                           // Compression = none
    tag(262, 3, 1, 1);                           // Photometric = BlackIsZero
    tag(270, 2, uint32_t(d.size()), descOff);    // ImageDescription
    tag(273, 4, 1, dataOff);                     // StripOffsets
    tag(277, 3, 1, 1);                           // SamplesPerPixel
    tag(278, 4, 1, uint32_t(h));                 // RowsPerStrip
    tag(279, 4, 1, dataBytes);                   // StripByteCounts
    tag(339, 3, 1, 1);                           // SampleFormat = uint
    u32(0);                                      // next IFD
    b.insert(b.end(), d.begin(), d.end());
    b.resize(dataOff, 0);
    std::ofstream o(path, std::ios::binary);
    if (!o) throw std::runtime_error("Cannot write " + path);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    for (size_t i = 0; i < size_t(w) * h; ++i) {  // explicit little-endian
        char px[2] = {char(data[i] & 0xff), char(data[i] >> 8)};
        o.write(px, 2);
    }
    if (!o) throw std::runtime_error("Write failed (disk full?): " + path);
}

bool readTiff16(const std::string& path, std::vector<uint16_t>& data, int& w, int& h, std::string& desc) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 8 || b[0] != 'I' || b[1] != 'I') return false;  // little-endian only (ours)
    auto r16 = [&](size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); };
    auto r32 = [&](size_t o) { return uint32_t(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (uint32_t(b[o + 3]) << 24)); };
    uint32_t ifd = r32(4);
    if (ifd + 2 > b.size()) return false;
    uint16_t n = r16(ifd);
    uint32_t off = 0, bps = 16, comp = 1;
    w = h = 0;
    for (uint16_t i = 0; i < n; ++i) {
        size_t e = ifd + 2 + size_t(i) * 12;
        if (e + 12 > b.size()) return false;
        uint16_t id = r16(e), type = r16(e + 2);
        uint32_t cnt = r32(e + 4);
        uint32_t val = type == 3 ? r16(e + 8) : r32(e + 8);
        if (id == 256) w = int(val);
        else if (id == 257) h = int(val);
        else if (id == 258) bps = val;
        else if (id == 259) comp = val;
        else if (id == 273) off = val;
        else if (id == 270 && val + cnt <= b.size()) desc.assign(reinterpret_cast<char*>(&b[val]), cnt ? cnt - 1 : 0);
    }
    if (bps != 16 || comp != 1 || w <= 0 || h <= 0 || off + size_t(w) * h * 2 > b.size()) return false;
    data.resize(size_t(w) * h);
    for (size_t i = 0; i < data.size(); ++i) data[i] = r16(off + 2 * i);
    return true;
}

// ------------------------------------------------------------------ PNG (RGB8 via zlib)
void writePngRgb(const std::string& path, const uint8_t* rgb, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + size_t(y) * w * 3, rgb + size_t(y + 1) * w * 3);
    }
    uLongf zlen = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), uLong(raw.size()), 6) != Z_OK)
        throw std::runtime_error("PNG compression failed");
    z.resize(zlen);
    std::ofstream o(path, std::ios::binary);
    if (!o) throw std::runtime_error("Cannot write " + path);
    auto be32 = [](uint32_t v, uint8_t* p) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); };
    auto chunk = [&](const char* type, const uint8_t* d, uint32_t n) {
        uint8_t hdr[8];
        be32(n, hdr);
        std::memcpy(hdr + 4, type, 4);
        o.write(reinterpret_cast<char*>(hdr), 8);
        if (n) o.write(reinterpret_cast<const char*>(d), n);
        uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
        if (n) crc = crc32(crc, d, n);
        uint8_t c[4];
        be32(uint32_t(crc), c);
        o.write(reinterpret_cast<char*>(c), 4);
    };
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    o.write(reinterpret_cast<const char*>(sig), 8);
    uint8_t ihdr[13];
    be32(uint32_t(w), ihdr);
    be32(uint32_t(h), ihdr + 4);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), uint32_t(z.size()));
    chunk("IEND", nullptr, 0);
}

void writeCsvCelsius(const std::string& path, const ThermalFrame& f) {
    FILE* fp = std::fopen(path.c_str(), "w");
    if (!fp) throw std::runtime_error("Cannot write " + path);
    for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x) std::fprintf(fp, x ? ",%.2f" : "%.2f", f.at(x, y));
        std::fputc('\n', fp);
    }
    std::fclose(fp);
}

static double r3(double v) { return std::round(v * 1000.0) / 1000.0; }

Json statsJson(const FrameStats& s) {
    Json j = Json::object();
    j["min_C"] = r3(s.minC);
    j["max_C"] = r3(s.maxC);
    j["mean_C"] = r3(s.meanC);
    Json mx = Json::array(); mx.push(s.maxX); mx.push(s.maxY);
    Json mn = Json::array(); mn.push(s.minX); mn.push(s.minY);
    j["max_xy"] = mx;
    j["min_xy"] = mn;
    return j;
}

Json roiResultsJson(const std::vector<Roi>& rois, const std::vector<RoiResult>& res) {
    Json j = Json::object();
    for (size_t i = 0; i < rois.size() && i < res.size(); ++i) {
        const Roi& r = rois[i];
        Json o = Json::object();
        if (r.kind == Roi::Kind::Spot) {
            o["type"] = "spot";
            o["x"] = r.x0;
            o["y"] = r.y0;
            o["value"] = r3(res[i].maxC);
        } else {
            o["type"] = "box";
            Json rc = Json::array();
            for (int v : {r.x0, r.y0, r.x1, r.y1}) rc.push(v);
            o["rect"] = rc;
            o["max"] = r3(res[i].maxC);
            o["min"] = r3(res[i].minC);
            o["mean"] = r3(res[i].meanC);
            Json mx = Json::array(); mx.push(res[i].maxX); mx.push(res[i].maxY);
            o["max_xy"] = mx;
        }
        if (!std::isnan(r.alarmAboveC)) {
            o["alarm_above_C"] = r.alarmAboveC;
            o["alarm"] = res[i].alarm;
        }
        j[r.name] = o;
    }
    return j;
}

SnapshotPaths saveSnapshot(const ThermalFrame& f, Json md, const SnapshotOptions& opt, const std::vector<Roi>& rois) {
    makeDirs(opt.folder);
    std::string stem = joinPath(opt.folder, timestampStem(opt.prefix, f.hostTimeNs));
    FrameStats st = computeStats(f);
    md["frame_host_time_ns"] = static_cast<long long>(f.hostTimeNs);
    md["camera_timestamp"] = static_cast<unsigned long long>(f.cameraTimestamp);
    md["frame_id"] = static_cast<unsigned long long>(f.frameId);
    md["stats"] = statsJson(st);
    if (!rois.empty()) md["rois"] = roiResultsJson(rois, measure(f, rois));
    SnapshotPaths p;
    p.tiff = stem + ".tiff";
    writeTiff16(p.tiff, f.counts.data(), f.width, f.height, md.dump());
    p.json = stem + ".json";
    {
        std::ofstream o(p.json);
        o << md.dump(2) << "\n";
        if (!o) throw std::runtime_error("Write failed: " + p.json);
    }
    std::vector<float> t;
    f.toCelsius(t);
    double lo = std::isnan(opt.lo) ? st.minC : opt.lo, hi = std::isnan(opt.hi) ? st.maxC : opt.hi;
    std::vector<uint8_t> rgb(t.size() * 3);
    colorize(t.data(), t.size(), lo, hi, opt.palette, rgb.data());
    p.png = stem + ".png";
    writePngRgb(p.png, rgb.data(), f.width, f.height);
    if (opt.csv) {
        p.csv = stem + "_degC.csv";
        writeCsvCelsius(p.csv, f);
    }
    return p;
}

std::string appendMeasurementLog(const std::string& path0, const ThermalFrame& f, const FrameStats& st,
                                 const std::vector<Roi>& rois, const std::vector<RoiResult>& res,
                                 const std::string& snapshotFile) {
    std::string header = "local_time,unix_time,file,frame_min_C,frame_max_C,frame_mean_C,max_x,max_y";
    for (auto& r : rois) {
        if (r.kind == Roi::Kind::Spot) header += "," + r.name + "_C";
        else header += "," + r.name + "_max_C," + r.name + "_min_C," + r.name + "_mean_C";
        if (!std::isnan(r.alarmAboveC)) header += "," + r.name + "_alarm";
    }
    std::string path = path0;
    bool isNew = !fs::exists(path);
    if (!isNew) {
        std::ifstream in(path);
        std::string first;
        std::getline(in, first);
        if (trim(first) != header) {  // tools changed: start a new log rather than misalign columns
            fs::path p(path0);
            path = (p.parent_path() / (p.stem().string() + "_" + timestampStem("", f.hostTimeNs).substr(1) +
                                       p.extension().string())).string();
            isNew = !fs::exists(path);
        }
    }
    FILE* fp = std::fopen(path.c_str(), "a");
    if (!fp) throw std::runtime_error("Cannot write " + path);
    if (isNew) std::fprintf(fp, "%s\n", header.c_str());
    std::fprintf(fp, "%s,%.3f,%s,%.2f,%.2f,%.3f,%d,%d", localTimeString(f.hostTimeNs).c_str(), f.hostTimeNs / 1e9,
                 fs::path(snapshotFile).filename().string().c_str(), st.minC, st.maxC, st.meanC, st.maxX, st.maxY);
    for (size_t i = 0; i < rois.size() && i < res.size(); ++i) {
        if (rois[i].kind == Roi::Kind::Spot) std::fprintf(fp, ",%.2f", res[i].maxC);
        else std::fprintf(fp, ",%.2f,%.2f,%.3f", res[i].maxC, res[i].minC, res[i].meanC);
        if (!std::isnan(rois[i].alarmAboveC)) std::fprintf(fp, ",%d", res[i].alarm ? 1 : 0);
    }
    std::fputc('\n', fp);
    bool ok = std::ferror(fp) == 0;
    std::fclose(fp);
    if (!ok) throw std::runtime_error("Write failed (disk full?): " + path);
    return path;
}

}  // namespace a70
