// Unit / integration tests that need no camera (simulator backend).
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "a70/engine.h"
#include "a70/io.h"
#include "a70/modbus_server.h"
#include "a70/recorder.h"

#ifdef A70_HAVE_ARAVIS
#include <arv.h>
#endif

using namespace a70;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                         \
        }                                                                     \
    } while (0)

static void testCameraSim() {
    A70Camera cam("sim");
    cam.open("");
    CHECK(cam.sensorWidth() == 640 && cam.sensorHeight() == 480);
    cam.start();
    ThermalFrame f;
    CHECK(cam.grab(f));
    CHECK(f.width == 640 && f.height == 480 && f.counts.size() == 640u * 480u);
    auto s = computeStats(f);
    CHECK(s.maxC > 80 && s.maxC < 95);   // flange
    CHECK(s.minC > 20 && s.minC < 32);   // ambient
    cam.setupRadiometric("TemperatureLinear100mK");
    ThermalFrame g;
    CHECK(cam.grab(g));
    CHECK(std::fabs(computeStats(g).meanC - s.meanC) < 1.0);
    // parameters round-trip in °C even though the camera stores kelvin
    cam.setParam("reflected_temp", 35.0);
    CHECK(std::fabs(*cam.getParam("reflected_temp") - 35.0) < 1e-6);
    CHECK(std::fabs(cam.backend().getFloat("ReflectedTemperature") - (35.0 + KELVIN)) < 1e-6);
    cam.close();
}

static void testFiles(const std::string& dir) {
    A70Camera cam("sim");
    cam.open("");
    cam.start();
    ThermalFrame f;
    cam.grab(f);
    std::vector<Roi> rois(2);
    rois[0].name = "Bx1"; rois[0].x0 = 160; rois[0].y0 = 230; rois[0].x1 = 290; rois[0].y1 = 325; rois[0].alarmAboveC = 30;
    rois[1].name = "Sp1"; rois[1].kind = Roi::Kind::Spot; rois[1].x0 = 470; rois[1].y0 = 250;
    SnapshotOptions o;
    o.folder = dir;
    o.prefix = "t";
    o.csv = true;
    auto p = saveSnapshot(f, cam.metadata(), o, rois);
    CHECK(fs::exists(p.tiff) && fs::exists(p.png) && fs::exists(p.json) && fs::exists(p.csv));
    std::vector<uint16_t> d;
    int w, h;
    std::string desc;
    CHECK(readTiff16(p.tiff, d, w, h, desc));
    CHECK(w == 640 && h == 480 && d == f.counts);
    double sc = 0;
    CHECK(jsonFindNumber(desc, "scale_K_per_count", sc) && std::fabs(sc - 0.01) < 1e-12);
    auto res = measure(f, rois);
    CHECK(res[0].alarm && res[0].maxC > res[0].meanC);
    CHECK(std::fabs(res[1].maxC - f.at(470, 250)) < 1e-9);
    // log column change rolls to a new file
    std::string l1 = appendMeasurementLog(joinPath(dir, "log.csv"), f, computeStats(f), rois, res, p.tiff);
    std::string l2 = appendMeasurementLog(joinPath(dir, "log.csv"), f, computeStats(f), {}, {}, p.tiff);
    CHECK(l1 != l2);
}

static void testRecorder(const std::string& dir) {
    if (!hdf5Available()) return;
    A70Camera cam("sim");
    cam.open("");
    cam.backend().setFloat("AcquisitionFrameRate", 30);
    cam.start();
    Recorder rec;
    rec.start(dir, cam.metadata(), "rec", 2, "gzip");
    ThermalFrame f, first;
    for (int i = 0; i < 10; ++i) {
        cam.grab(f);
        if (i == 0) first = f;
        rec.push(f);
    }
    rec.stop();
    CHECK(rec.written() == 5 && rec.error().empty());
    RecordingReader r(rec.path());
    CHECK(r.info().frames == 5 && r.info().width == 640 && r.info().height == 480);
    CHECK(std::fabs(r.info().scaleK - 0.01) < 1e-12);
    ThermalFrame g;
    r.readFrame(0, g);
    CHECK(g.counts == first.counts);
    CHECK(r.info().metadataJson.find("camera_model") != std::string::npos);
}

static void testReconnect() {
    EngineConfig c;
    c.backend = "sim";
    c.reconnectAfterS = 1.5;
    c.staleAfterS = 1.0;
    c.params["emissivity"] = 0.92;
    CaptureEngine e(c);
    CHECK(e.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    e.camera()->setParam("emissivity", 0.5);  // will be lost by the "reboot"...
    e.camera()->backend().execute("SimCableUnplug");
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    auto s = e.status();
    CHECK(!s.fresh);                           // data flagged stale while camera is gone
    std::string err;
    CHECK(!e.snapshot(&err));                  // and stale data is never saved
    for (int i = 0; i < 100 && !(e.status().fresh && e.status().reconnects > 0); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    s = e.status();
    CHECK(s.fresh && s.reconnects >= 1);
    CHECK(std::fabs(*e.camera()->getParam("emissivity") - 0.92) < 1e-9);  // ...config re-applied
    e.stop();
}

static void testEngine(const std::string& dir) {
    EngineConfig c;
    c.backend = "sim";
    c.folder = dir;
    c.prefix = "eng";
    c.minFreeMB = 1;
    c.params["emissivity"] = 0.9;
    CaptureEngine e(c);
    std::string err;
    CHECK(e.start(&err));
    for (int i = 0; i < 100 && e.status().framesTotal < 5; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto s = e.status();
    CHECK(s.connected && s.fresh && s.framesTotal >= 5);
    CHECK(std::fabs(*e.camera()->getParam("emissivity") - 0.9) < 1e-9);
    Roi r;
    r.name = "Bx1"; r.x0 = 160; r.y0 = 230; r.x1 = 290; r.y1 = 325; r.alarmAboveC = 30;
    e.setRois({r});
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(e.status().anyAlarm);
    CHECK(e.command(Command::Snapshot, &err));
    CHECK(e.command(Command::Nuc, &err));
    if (hdf5Available()) {
        CHECK(e.startRecording(1, "none", 0, &err));
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        e.stopRecording();
        CHECK(e.status().recWritten >= 5);
    }
    e.startInterval(0.5);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    e.stopInterval();
    CHECK(e.status().snapshots >= 3);
    CHECK(fs::exists(joinPath(dir, "eng_measurements_log.csv")));
    CHECK(e.changeIrFormat("TemperatureLinear100mK", &err));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(e.latest() && std::fabs(e.latest()->scaleK - 0.1) < 1e-12);
    e.stop();
    CHECK(modbusTemp(12.34) == 123 && modbusTemp(-5.0) == -50 && modbusTemp(NAN) == -32768);
}

static void testAravisFake() {
#ifdef A70_HAVE_ARAVIS
    // Aravis ships an in-process fake GenICam camera: exercises the real Aravis code path.
    arv_enable_interface("Fake");
    A70Camera cam("aravis");
    bool found = false;
    for (auto& d : cam.listDevices()) found |= d.id == "Fake_1";
    CHECK(found);
    cam.open("Fake_1");
    auto w = cam.takeWarnings();
    CHECK(!w.empty());  // no IRFormat on the fake camera -> warned, not crashed
    CHECK(cam.backend().getString("PixelFormat") == "Mono16");
    auto feats = cam.backend().features();
    CHECK(feats.size() > 10);
    cam.start();
    ThermalFrame f;
    int got = 0;
    for (int i = 0; i < 5; ++i) got += cam.grab(f, 2.0) ? 1 : 0;
    CHECK(got == 5 && f.width > 0 && f.counts.size() == size_t(f.width) * f.height);
    cam.close();
    std::printf("aravis fake camera: %d frames %dx%d, %zu features\n", got, f.width, f.height, feats.size());
#endif
}

int main() {
    std::string dir = (fs::temp_directory_path() / "a70_tests").string();
    fs::remove_all(dir);
    fs::create_directories(dir);
    testCameraSim();
    testFiles(dir);
    testRecorder(dir);
    testEngine(dir);
    testReconnect();
    testAravisFake();
    std::printf(g_fail ? "%d FAILED\n" : "all tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
