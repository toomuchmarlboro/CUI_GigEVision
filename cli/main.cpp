// a70_cli — headless FLIR A70 control, capture, Modbus service and file tools.
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "a70/engine.h"
#include "a70/io.h"
#include "a70/modbus_server.h"
#include "a70/recorder.h"
#include "a70/watchdog.h"

using namespace a70;

static const char* USAGE = R"(a70_cli — FLIR A70 radiometric capture (C++)

Usage: a70_cli <command> [options]

Camera commands
  list                         discover cameras
  features [--out-file F.csv]  dump every GenICam feature (send this file when the camera arrives)
  snap [--count N] [--interval S] [--csv]          radiometric snapshot(s)
  record [--duration S] [--decimate N] [--compression gzip|none]   radiometric video (HDF5)
  timelapse [--interval S] [--count N] [--nuc-every N]             periodic snapshots + CSV log
  serve [--modbus-port P] [--modbus-bind IP] [--no-modbus] [--interval S]
                               24/7 service: auto-reconnect, Modbus TCP, systemd watchdog

File commands
  info FILE.h5                 recording summary + metadata
  csv FILE.h5 [--frame N] | FILE.tiff  [--out-file F]   °C matrix as CSV
  trend FILE.h5 [--box ..] [--spot ..] [--every N]   temperature-vs-time CSV per tool
  frames FILE.h5 [--every N] [--palette P] [--range LO,HI]   PNG sequence (ffmpeg -> mp4)

Common options
  --config FILE                key=value lines, same names as the options below
  --backend aravis|spinnaker|sim   --device ID|IP|SERIAL   --packet-size N
  --irformat TemperatureLinear10mK|TemperatureLinear100mK
  --emissivity E  --reflected C  --atmospheric C  --distance M  --humidity H
  --fps F  --range NAME  --nuc
  --out DIR  --prefix TAG  --palette Iron|Rainbow|Lava|White hot|Black hot  --min-free-mb N
  --box [NAME,]x0,y0,x1,y1[,alarmC]   --spot [NAME,]x,y[,alarmC]   (repeatable)
)";

// ------------------------------------------------------------------ args
struct Args {
    std::string cmd;
    std::vector<std::string> positional;
    std::map<std::string, std::vector<std::string>> opt;
    std::set<std::string> flags;

    bool has(const std::string& k) const { return flags.count(k) || opt.count(k); }
    std::string get(const std::string& k, const std::string& d = "") const {
        auto it = opt.find(k);
        return it == opt.end() || it->second.empty() ? d : it->second.back();
    }
    double num(const std::string& k, double d) const {
        auto s = get(k);
        return s.empty() ? d : std::stod(s);
    }
    std::vector<std::string> all(const std::string& k) const {
        auto it = opt.find(k);
        return it == opt.end() ? std::vector<std::string>{} : it->second;
    }
};

static const std::set<std::string> kFlags = {"nuc", "csv", "no-modbus", "help", "verbose"};

static void parseInto(Args& a, const std::vector<std::string>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        const std::string& s = v[i];
        if (s.rfind("--", 0) == 0) {
            std::string k = s.substr(2), val;
            auto eq = k.find('=');
            if (eq != std::string::npos) {
                val = k.substr(eq + 1);
                k = k.substr(0, eq);
                a.opt[k].push_back(val);
            } else if (kFlags.count(k)) {
                a.flags.insert(k);
            } else if (i + 1 < v.size()) {
                a.opt[k].push_back(v[++i]);
            } else {
                throw std::runtime_error("Option --" + k + " needs a value");
            }
        } else if (a.cmd.empty()) {
            a.cmd = s;
        } else {
            a.positional.push_back(s);
        }
    }
}

static void loadConfig(Args& a, const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot read config " + path);
    std::string line;
    std::vector<std::string> v;
    while (std::getline(in, line)) {
        auto hash = line.find(" #");  // inline comment
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        std::string k = trim(line.substr(0, eq));
        if (eq == std::string::npos) {
            v.push_back("--" + k);
        } else {
            v.push_back("--" + k + "=" + trim(line.substr(eq + 1)));
        }
    }
    Args tmp;
    parseInto(tmp, v);
    for (auto& kv : tmp.opt)  // command line wins: only fill missing keys
        if (!a.opt.count(kv.first)) a.opt[kv.first] = kv.second;
    for (auto& f : tmp.flags) a.flags.insert(f);
}

static std::vector<std::string> split(const std::string& s, char d) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, d)) out.push_back(trim(t));
    return out;
}

static bool isNum(const std::string& s) {
    char* e = nullptr;
    std::strtod(s.c_str(), &e);
    return !s.empty() && e && *e == 0;
}

static std::vector<Roi> parseRois(const Args& a) {
    std::vector<Roi> rois;
    int nb = 0, ns = 0;
    for (auto& s : a.all("box")) {
        auto p = split(s, ',');
        Roi r;
        r.kind = Roi::Kind::Box;
        size_t k = 0;
        r.name = !p.empty() && !isNum(p[0]) ? p[k++] : "Bx" + std::to_string(nb + 1);
        if (p.size() - k < 4) throw std::runtime_error("--box needs x0,y0,x1,y1: " + s);
        r.x0 = std::stoi(p[k]); r.y0 = std::stoi(p[k + 1]); r.x1 = std::stoi(p[k + 2]); r.y1 = std::stoi(p[k + 3]);
        if (p.size() - k >= 5) r.alarmAboveC = std::stod(p[k + 4]);
        rois.push_back(r);
        ++nb;
    }
    for (auto& s : a.all("spot")) {
        auto p = split(s, ',');
        Roi r;
        r.kind = Roi::Kind::Spot;
        size_t k = 0;
        r.name = !p.empty() && !isNum(p[0]) ? p[k++] : "Sp" + std::to_string(ns + 1);
        if (p.size() - k < 2) throw std::runtime_error("--spot needs x,y: " + s);
        r.x0 = r.x1 = std::stoi(p[k]);
        r.y0 = r.y1 = std::stoi(p[k + 1]);
        if (p.size() - k >= 3) r.alarmAboveC = std::stod(p[k + 2]);
        rois.push_back(r);
        ++ns;
    }
    return rois;
}

static EngineConfig engineConfig(const Args& a) {
    EngineConfig c;
    c.backend = a.get("backend", defaultBackend());
    c.device = a.get("device");
    c.packetSize = int(a.num("packet-size", 0));
    c.irFormat = a.get("irformat", "TemperatureLinear10mK");
    c.folder = a.get("out", "a70_captures");
    c.prefix = a.get("prefix", "a70");
    c.palette = a.get("palette", "Iron");
    c.csvWithSnapshots = a.has("csv");
    c.minFreeMB = a.num("min-free-mb", 500);
    c.nucOnConnect = a.has("nuc");
    const std::pair<const char*, const char*> pm[] = {{"emissivity", "emissivity"}, {"reflected", "reflected_temp"},
                                                       {"atmospheric", "atmospheric_temp"}, {"distance", "distance"},
                                                       {"humidity", "humidity"}};
    for (auto& p : pm)
        if (a.opt.count(p.first)) c.params[p.second] = a.num(p.first, 0);
    if (a.opt.count("fps")) c.frameRate = a.num("fps", 30);
    c.range = a.get("range");
    return c;
}

static std::atomic<bool> g_stop{false};
static void onSignal(int) { g_stop = true; }

static void stderrLog(const std::string& lvl, const std::string& msg) {
    // journald picks up the <n> prefix as priority
    static const bool journal = std::getenv("JOURNAL_STREAM") != nullptr;
    int prio = lvl == "error" ? 3 : lvl == "warning" ? 4 : 6;
    if (journal) std::fprintf(stderr, "<%d>%s\n", prio, msg.c_str());
    else std::fprintf(stderr, "%s: %s\n", lvl.c_str(), msg.c_str());
    std::fflush(stderr);
}

static std::unique_ptr<CaptureEngine> startEngine(const Args& a, bool reconnect) {
    EngineConfig c = engineConfig(a);
    c.reconnect = reconnect;
    auto e = std::make_unique<CaptureEngine>(c);
    e->setLogCallback(stderrLog);
    e->setRois(parseRois(a));
    std::string err;
    if (!e->start(&err) && !reconnect) throw std::runtime_error(err);
    return e;
}

static bool waitFrame(CaptureEngine& e, double timeoutS = 10) {
    double t0 = monoSeconds();
    uint64_t start = e.status().framesTotal;
    while (monoSeconds() - t0 < timeoutS && !g_stop) {
        if (e.status().framesTotal > start) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// ------------------------------------------------------------------ camera commands
static int cmdList(const Args& a) {
    A70Camera cam(a.get("backend", defaultBackend()));
    auto devs = cam.listDevices();
    for (auto& d : devs) std::printf("%s\t%s\n", d.id.c_str(), d.label().c_str());
    if (devs.empty()) std::fprintf(stderr, "No cameras found (check cable, subnet, firewall)\n");
    return devs.empty() ? 1 : 0;
}

static std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) o += c == '"' ? std::string("\"\"") : std::string(1, c);
    return o + "\"";
}

static int cmdFeatures(const Args& a) {
    A70Camera cam(a.get("backend", defaultBackend()), int(a.num("packet-size", 0)));
    cam.open(a.get("device"), a.get("irformat", "TemperatureLinear10mK"));
    std::vector<Feature> f;
    {
        std::lock_guard<std::recursive_mutex> lk(cam.mutex());
        f = cam.backend().features();
    }
    std::string path = a.get("out-file");
    std::ofstream file;
    std::ostream* o = &std::cout;
    if (!path.empty()) {
        file.open(path);
        o = &file;
    }
    *o << "category,name,type,value,unit,access,choices,description\n";
    for (auto& x : f) {
        std::string ch;
        for (auto& c : x.choices) ch += (ch.empty() ? "" : "|") + c;
        *o << csvField(x.category) << ',' << csvField(x.name) << ',' << toString(x.type) << ','
           << csvField(x.value) << ',' << csvField(x.unit) << ',' << (x.readable ? "R" : "") << (x.writable ? "W" : "")
           << ',' << csvField(ch) << ',' << csvField(x.description) << '\n';
    }
    if (!path.empty()) std::fprintf(stderr, "%zu features -> %s\n", f.size(), path.c_str());
    return 0;
}

static int cmdSnap(const Args& a) {
    auto e = startEngine(a, false);
    int n = int(a.num("count", 1));
    double iv = a.num("interval", 1.0);
    for (int i = 0; i < n && !g_stop; ++i) {
        if (!waitFrame(*e)) throw std::runtime_error("No frames received — check network/packet size/firewall");
        std::string err;
        auto p = e->snapshot(&err, false);
        if (!p) throw std::runtime_error(err);
        auto st = e->status().stats;
        std::printf("%s  min %.2f  max %.2f  mean %.2f °C\n", p->tiff.c_str(), st.minC, st.maxC, st.meanC);
        if (i + 1 < n) std::this_thread::sleep_for(std::chrono::duration<double>(iv));
    }
    return 0;
}

static int cmdRecord(const Args& a) {
    auto e = startEngine(a, false);
    if (!waitFrame(*e)) throw std::runtime_error("No frames received");
    std::string err;
    double dur = a.num("duration", 0);
    if (!e->startRecording(int(a.num("decimate", 1)), a.get("compression", "gzip"), dur, &err))
        throw std::runtime_error(err);
    std::fprintf(stderr, "Recording%s (Ctrl+C to stop)\n", dur > 0 ? "" : " until stopped");
    double last = 0;
    while (!g_stop && e->status().recording) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (monoSeconds() - last > 2) {
            last = monoSeconds();
            auto s = e->status();
            std::fprintf(stderr, "\r%llu frames, %llu dropped   ", (unsigned long long)s.recWritten,
                         (unsigned long long)s.recDropped);
        }
    }
    e->stopRecording();
    auto s = e->status();
    std::fprintf(stderr, "\nSaved %s: %llu frames, %llu dropped %s\n", s.recPath.c_str(),
                 (unsigned long long)s.recWritten, (unsigned long long)s.recDropped, s.recError.c_str());
    return s.recError.empty() ? 0 : 1;
}

static int cmdTimelapse(const Args& a) {
    auto e = startEngine(a, true);  // long-running: survive camera drops
    double iv = a.num("interval", 60);
    int count = int(a.num("count", 0)), nucEvery = int(a.num("nuc-every", 0));
    waitFrame(*e, 30);
    e->startInterval(iv);
    uint64_t done = 0, base = e->status().snapshots;
    while (!g_stop && (count == 0 || done < uint64_t(count))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        uint64_t now = e->status().snapshots - base;
        if (now != done) {
            done = now;
            auto s = e->status();
            std::fprintf(stderr, "[%llu] %s  max %.2f °C\n", (unsigned long long)done, s.lastSnapshot.c_str(), s.stats.maxC);
            if (nucEvery && done % uint64_t(nucEvery) == 0) e->command(Command::Nuc);
        }
    }
    e->stopInterval();
    return 0;
}

static int cmdServe(const Args& a) {
    SystemdNotifier sd;
    auto e = startEngine(a, true);
    if (a.opt.count("interval")) e->startInterval(a.num("interval", 60));
    std::unique_ptr<ModbusServer> mb;
    if (!a.has("no-modbus")) {
        mb = std::make_unique<ModbusServer>(*e, a.get("modbus-bind", "0.0.0.0"), int(a.num("modbus-port", 502)));
        std::string err;
        if (!mb->start(&err)) throw std::runtime_error("Modbus: " + err);
        stderrLog("info", "Modbus TCP listening on " + a.get("modbus-bind", "0.0.0.0") + ":" +
                              std::to_string(mb->port()));
    }
    sd.ready();
    stderrLog("info", "Service running" + std::string(sd.watchdogIntervalS() > 0 ? " (systemd watchdog on)" : ""));
    uint64_t lastLoop = 0;
    double lastLoopChange = monoSeconds(), lastStatus = 0;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        EngineStatus s = e->status();
        // Only ping the watchdog while the acquisition loop is still turning; if it hangs
        // (driver deadlock etc.) systemd restarts the whole service.
        if (s.loopCounter != lastLoop) {
            lastLoop = s.loopCounter;
            lastLoopChange = monoSeconds();
        }
        if (monoSeconds() - lastLoopChange < 30) sd.ping();
        if (monoSeconds() - lastStatus > 10) {
            lastStatus = monoSeconds();
            char b[256];
            std::snprintf(b, sizeof b, "%s, %.1f fps, max %.1f C, %llu snapshots%s%s",
                          s.connected ? (s.fresh ? "streaming" : "stale") : "disconnected", s.fps, s.stats.maxC,
                          (unsigned long long)s.snapshots, s.anyAlarm ? ", ALARM" : "",
                          mb ? (", modbus clients " + std::to_string(mb->clients())).c_str() : "");
            sd.status(b);
            if (a.has("verbose")) stderrLog("info", b);
        }
    }
    sd.stopping();
    stderrLog("info", "Stopping");
    if (mb) mb->stop();
    e->stop();
    return 0;
}

// ------------------------------------------------------------------ file commands
static int cmdInfo(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("info needs FILE.h5");
    RecordingReader r(a.positional[0]);
    auto& i = r.info();
    std::printf("frames      %llu  (%dx%d)\n", (unsigned long long)i.frames, i.width, i.height);
    if (i.frames > 1 && i.hostTimeNs.size() == i.frames) {
        double d = (i.hostTimeNs.back() - i.hostTimeNs.front()) / 1e9;
        std::printf("duration    %.2f s   mean rate %.2f Hz\n", d, d > 0 ? (i.frames - 1) / d : 0);
    }
    std::printf("scale       %g K/count\ndropped     %llu\nmetadata    %s\n", i.scaleK,
                (unsigned long long)i.droppedFrames, i.metadataJson.c_str());
    if (i.frames) {
        ThermalFrame f;
        r.readFrame(0, f);
        auto s = computeStats(f);
        std::printf("frame 0     min %.2f  max %.2f  mean %.2f °C\n", s.minC, s.maxC, s.meanC);
    }
    return 0;
}

static std::string stemOf(const std::string& p) {
    auto dot = p.find_last_of('.');
    return dot == std::string::npos ? p : p.substr(0, dot);
}

static int cmdCsv(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("csv needs a file");
    std::string in = a.positional[0], out = a.get("out-file");
    ThermalFrame f;
    if (in.size() > 4 && (in.substr(in.size() - 4) == ".tif" || in.substr(in.size() - 5) == ".tiff")) {
        std::string desc;
        if (!readTiff16(in, f.counts, f.width, f.height, desc)) throw std::runtime_error("Not an A70 TIFF: " + in);
        double sc = 0.01;
        jsonFindNumber(desc, "scale_K_per_count", sc);
        f.scaleK = sc;
        if (out.empty()) out = stemOf(in) + "_degC.csv";
    } else {
        RecordingReader r(in);
        uint64_t idx = uint64_t(a.num("frame", 0));
        r.readFrame(idx, f);
        if (out.empty()) out = stemOf(in) + "_frame" + std::to_string(idx) + "_degC.csv";
    }
    writeCsvCelsius(out, f);
    std::printf("%s\n", out.c_str());
    return 0;
}

static int cmdTrend(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("trend needs FILE.h5");
    RecordingReader r(a.positional[0]);
    auto rois = parseRois(a);
    int every = std::max(1, int(a.num("every", 1)));
    std::string out = a.get("out-file", stemOf(a.positional[0]) + "_trend.csv");
    FILE* fp = std::fopen(out.c_str(), "w");
    if (!fp) throw std::runtime_error("Cannot write " + out);
    std::fprintf(fp, "t_s,frame_min_C,frame_max_C,frame_mean_C");
    for (auto& x : rois) {
        if (x.kind == Roi::Kind::Spot) std::fprintf(fp, ",%s_C", x.name.c_str());
        else std::fprintf(fp, ",%s_max_C,%s_min_C,%s_mean_C", x.name.c_str(), x.name.c_str(), x.name.c_str());
    }
    std::fputc('\n', fp);
    ThermalFrame f;
    auto& info = r.info();
    for (uint64_t i = 0; i < info.frames; i += uint64_t(every)) {
        r.readFrame(i, f);
        auto s = computeStats(f);
        double t = info.hostTimeNs.empty() ? 0 : (info.hostTimeNs[i] - info.hostTimeNs[0]) / 1e9;
        std::fprintf(fp, "%.3f,%.2f,%.2f,%.3f", t, s.minC, s.maxC, s.meanC);
        auto res = measure(f, rois);
        for (size_t k = 0; k < rois.size(); ++k) {
            if (rois[k].kind == Roi::Kind::Spot) std::fprintf(fp, ",%.2f", res[k].maxC);
            else std::fprintf(fp, ",%.2f,%.2f,%.3f", res[k].maxC, res[k].minC, res[k].meanC);
        }
        std::fputc('\n', fp);
    }
    std::fclose(fp);
    std::printf("%s\n", out.c_str());
    return 0;
}

static int cmdFrames(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("frames needs FILE.h5");
    RecordingReader r(a.positional[0]);
    auto& info = r.info();
    int every = std::max(1, int(a.num("every", 1)));
    std::string dir = a.get("out", stemOf(a.positional[0]) + "_png");
    makeDirs(dir);
    double lo = NAN, hi = NAN;
    if (a.opt.count("range")) {
        auto p = split(a.get("range"), ',');
        lo = std::stod(p.at(0));
        hi = std::stod(p.at(1));
    }
    ThermalFrame f;
    std::vector<float> t;
    std::vector<uint8_t> rgb;
    if (std::isnan(lo) && info.frames) {  // fixed range from first frame so the video doesn't flicker
        r.readFrame(0, f);
        f.toCelsius(t);
        autoRange(t.data(), f.width, f.height, lo, hi);
    }
    int n = 0;
    for (uint64_t i = 0; i < info.frames; i += uint64_t(every), ++n) {
        r.readFrame(i, f);
        f.toCelsius(t);
        rgb.resize(t.size() * 3);
        colorize(t.data(), t.size(), lo, hi, a.get("palette", "Iron"), rgb.data());
        char name[32];
        std::snprintf(name, sizeof name, "frame_%06d.png", n);
        writePngRgb(joinPath(dir, name), rgb.data(), f.width, f.height);
    }
    std::printf("%d PNGs -> %s  (range %.1f..%.1f °C)\nffmpeg -framerate 30 -i %s -pix_fmt yuv420p out.mp4\n", n,
                dir.c_str(), lo, hi, joinPath(dir, "frame_%06d.png").c_str());
    return 0;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    Args a;
    try {
        parseInto(a, std::vector<std::string>(argv + 1, argv + argc));
        if (a.opt.count("config")) loadConfig(a, a.get("config"));
        if (a.cmd.empty() || a.cmd == "help" || a.has("help")) {
            std::fputs(USAGE, stdout);
            return a.cmd.empty() ? 1 : 0;
        }
        static const std::map<std::string, int (*)(const Args&)> cmds = {
            {"list", cmdList},   {"features", cmdFeatures}, {"snap", cmdSnap}, {"record", cmdRecord},
            {"timelapse", cmdTimelapse}, {"serve", cmdServe}, {"info", cmdInfo}, {"csv", cmdCsv},
            {"trend", cmdTrend}, {"frames", cmdFrames}};
        auto it = cmds.find(a.cmd);
        if (it == cmds.end()) {
            std::fprintf(stderr, "Unknown command '%s'\n\n%s", a.cmd.c_str(), USAGE);
            return 2;
        }
        return it->second(a);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
