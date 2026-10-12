#include "a70/thermal.h"

#include <algorithm>
#include <map>

namespace a70 {

void ThermalFrame::toCelsius(std::vector<float>& out) const {
    out.resize(counts.size());
    const float s = float(scaleK), k = float(KELVIN);
    for (size_t i = 0; i < counts.size(); ++i) out[i] = counts[i] * s - k;
}

FrameStats computeStats(const ThermalFrame& f) {
    FrameStats st;
    if (f.counts.empty()) return st;
    uint16_t mn = 65535, mx = 0;
    size_t imn = 0, imx = 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < f.counts.size(); ++i) {
        uint16_t c = f.counts[i];
        sum += c;
        if (c > mx) { mx = c; imx = i; }
        if (c < mn) { mn = c; imn = i; }
    }
    st.maxC = f.celsius(mx);
    st.minC = f.celsius(mn);
    st.meanC = double(sum) / double(f.counts.size()) * f.scaleK - KELVIN;
    st.maxX = int(imx % f.width);
    st.maxY = int(imx / f.width);
    st.minX = int(imn % f.width);
    st.minY = int(imn / f.width);
    return st;
}

Roi clampRoi(Roi r, int w, int h) {
    if (r.kind == Roi::Kind::Spot) { r.x1 = r.x0; r.y1 = r.y0; }
    auto cx = [&](int v) { return std::clamp(v, 0, w - 1); };
    auto cy = [&](int v) { return std::clamp(v, 0, h - 1); };
    r.x0 = cx(r.x0); r.x1 = cx(r.x1); r.y0 = cy(r.y0); r.y1 = cy(r.y1);
    if (r.x1 < r.x0) std::swap(r.x0, r.x1);
    if (r.y1 < r.y0) std::swap(r.y0, r.y1);
    return r;
}

std::vector<RoiResult> measure(const ThermalFrame& f, const std::vector<Roi>& rois) {
    std::vector<RoiResult> out;
    out.reserve(rois.size());
    for (const Roi& r0 : rois) {
        RoiResult res;
        res.name = r0.name;
        res.kind = r0.kind;
        if (f.counts.empty()) { out.push_back(res); continue; }
        Roi r = clampRoi(r0, f.width, f.height);
        uint16_t mn = 65535, mx = 0;
        uint64_t sum = 0, n = 0;
        int mxx = r.x0, mxy = r.y0;
        for (int y = r.y0; y <= r.y1; ++y)
            for (int x = r.x0; x <= r.x1; ++x) {
                uint16_t c = f.counts[size_t(y) * f.width + x];
                sum += c; ++n;
                if (c > mx) { mx = c; mxx = x; mxy = y; }
                if (c < mn) mn = c;
            }
        res.maxC = f.celsius(mx);
        res.minC = f.celsius(mn);
        res.meanC = double(sum) / double(n) * f.scaleK - KELVIN;
        res.maxX = mxx;
        res.maxY = mxy;
        res.alarm = !std::isnan(r0.alarmAboveC) && res.maxC > r0.alarmAboveC;
        res.valid = true;
        out.push_back(res);
    }
    return out;
}

// ------------------------------------------------------------------ palettes
namespace {
using Lut = std::array<std::array<uint8_t, 3>, 256>;
struct Stop { double p; int r, g, b; };

Lut makeLut(const std::vector<Stop>& s) {
    Lut l{};
    for (int i = 0; i < 256; ++i) {
        double x = i / 255.0;
        size_t k = 0;
        while (k + 2 < s.size() && x > s[k + 1].p) ++k;
        double t = (x - s[k].p) / std::max(1e-9, s[k + 1].p - s[k].p);
        t = std::clamp(t, 0.0, 1.0);
        l[i] = {uint8_t(std::lround(s[k].r + t * (s[k + 1].r - s[k].r))),
                uint8_t(std::lround(s[k].g + t * (s[k + 1].g - s[k].g))),
                uint8_t(std::lround(s[k].b + t * (s[k + 1].b - s[k].b)))};
    }
    return l;
}

const std::vector<std::pair<std::string, Lut>>& luts() {
    static const std::vector<std::pair<std::string, Lut>> v = {
        {"Iron", makeLut({{0, 0, 0, 0}, {0.15, 30, 0, 90}, {0.35, 140, 0, 150}, {0.55, 220, 50, 40},
                          {0.75, 250, 150, 0}, {0.9, 255, 220, 60}, {1, 255, 255, 230}})},
        {"Rainbow", makeLut({{0, 0, 0, 120}, {0.2, 0, 60, 255}, {0.4, 0, 220, 220}, {0.55, 0, 200, 0},
                             {0.7, 255, 255, 0}, {0.85, 255, 120, 0}, {1, 230, 0, 0}})},
        {"Lava", makeLut({{0, 0, 0, 4}, {0.25, 80, 18, 123}, {0.5, 182, 54, 121}, {0.75, 251, 136, 97},
                          {1, 252, 253, 191}})},
        {"White hot", makeLut({{0, 0, 0, 0}, {1, 255, 255, 255}})},
        {"Black hot", makeLut({{0, 255, 255, 255}, {1, 0, 0, 0}})},
    };
    return v;
}
}  // namespace

std::vector<std::string> paletteNames() {
    std::vector<std::string> n;
    for (auto& p : luts()) n.push_back(p.first);
    return n;
}

const Lut& paletteLut(const std::string& name) {
    for (auto& p : luts())
        if (p.first == name) return p.second;
    return luts().front().second;
}

void colorize(const float* t, size_t n, double lo, double hi, const std::string& palette, uint8_t* rgb) {
    const Lut& lut = paletteLut(palette);
    const float span = float(std::max(hi - lo, 1e-6)), flo = float(lo), k = 255.f / span;
    for (size_t i = 0; i < n; ++i) {
        float v = (t[i] - flo) * k;
        int idx = v <= 0.f ? 0 : (v >= 255.f ? 255 : int(v));
        const auto& c = lut[idx];
        rgb[3 * i] = c[0];
        rgb[3 * i + 1] = c[1];
        rgb[3 * i + 2] = c[2];
    }
}

void autoRange(const float* t, int w, int h, double& lo, double& hi, double pct, const Roi* box) {
    std::vector<float> s;
    int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
    if (box) {
        Roi r = clampRoi(*box, w, h);
        x0 = r.x0; y0 = r.y0; x1 = r.x1; y1 = r.y1;
    }
    long area = long(x1 - x0 + 1) * (y1 - y0 + 1);
    int step = area > 20000 ? 4 : 1;
    s.reserve(size_t(area / (step * step) + 16));
    for (int y = y0; y <= y1; y += step)
        for (int x = x0; x <= x1; x += step) s.push_back(t[size_t(y) * w + x]);
    if (s.empty()) { lo = 0; hi = 1; return; }
    size_t a = size_t(pct / 100.0 * (s.size() - 1)), b = size_t((100 - pct) / 100.0 * (s.size() - 1));
    std::nth_element(s.begin(), s.begin() + a, s.end());
    lo = s[a];
    std::nth_element(s.begin(), s.begin() + b, s.end());
    hi = s[b];
    if (hi - lo < 0.5) {
        double m = (hi + lo) / 2;
        lo = m - 0.25;
        hi = m + 0.25;
    }
}

}  // namespace a70
