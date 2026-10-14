// a70_gui — FLIR A70 radiometric capture (Qt Widgets, Qt5 or Qt6)
//   a70_gui                                   pick backend / camera in the window
//   a70_gui --backend sim --connect           try it without a camera
//   a70_gui --backend aravis --device 192.168.1.50 --connect
//   a70_gui --selftest OUTDIR                 offscreen smoke test (CI): connects to the simulator,
//                                             adds tools, snapshots, records, saves screenshots
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QPalette>
#include <QStyleFactory>
#include <QThread>
#include <QTimer>

#include "main_window.h"
#include "thermal_view.h"

static void darkTheme(QApplication& app) {
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    p.setColor(QPalette::Window, QColor(37, 37, 40));
    p.setColor(QPalette::WindowText, QColor(225, 225, 225));
    p.setColor(QPalette::Base, QColor(27, 27, 30));
    p.setColor(QPalette::AlternateBase, QColor(45, 45, 48));
    p.setColor(QPalette::Text, QColor(225, 225, 225));
    p.setColor(QPalette::Button, QColor(50, 50, 54));
    p.setColor(QPalette::ButtonText, QColor(225, 225, 225));
    p.setColor(QPalette::Highlight, QColor(230, 120, 30));
    p.setColor(QPalette::HighlightedText, QColor(0, 0, 0));
    p.setColor(QPalette::ToolTipBase, QColor(50, 50, 54));
    p.setColor(QPalette::ToolTipText, QColor(225, 225, 225));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    app.setPalette(p);
}

static void pump(QApplication& app, int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

static int selfTest(QApplication& app, MainWindow& w, const QString& out) {
    QDir().mkpath(out);
    w.setOutput(out + "/captures", "PIPE-101");
    w.connectCamera();
    pump(app, 1500);
    if (!w.engine()) return 2;
    a70::Roi box;
    box.name = "Bx1";
    box.x0 = 160; box.y0 = 230; box.x1 = 290; box.y1 = 325; box.alarmAboveC = 38.0;
    a70::Roi s1, s2;
    s1.name = "Sp1"; s1.kind = a70::Roi::Kind::Spot; s1.x0 = 470; s1.y0 = 250;
    s2.name = "Sp2"; s2.kind = a70::Roi::Kind::Spot; s2.x0 = 100; s2.y0 = 400;
    w.view()->setRois({box, s1, s2});
    emit w.view()->roisEdited({box, s1, s2});
    w.setBoxRange(true);
    pump(app, 1500);
    w.snapshot();
    w.enableModbus(15020);
    pump(app, 1200);
    w.grab().save(out + "/gui.png");
    w.setPanelVisible(true);  // drawer defaults to hidden; show it so this grab isn't blank
    pump(app, 200);
    w.panel()->grab().save(out + "/panel.png");
    auto s = w.engine()->status();
    std::printf("selftest: fresh=%d fps=%.1f snapshots=%llu alarm=%d\n", s.fresh, s.fps,
                (unsigned long long)s.snapshots, s.anyAlarm);
    w.disconnectCamera();
    return s.fresh && s.snapshots >= 1 ? 0 : 1;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("a70_gui");
    QCommandLineParser p;
    p.addHelpOption();
    QCommandLineOption oBackend("backend", "aravis | spinnaker | sim", "name");
    QCommandLineOption oDevice("device", "camera id / IP / serial", "id");
    QCommandLineOption oConnect("connect", "connect on start-up");
    QCommandLineOption oLight("light", "default (light) Qt theme");
    QCommandLineOption oSelf("selftest", "offscreen smoke test, writes screenshots to DIR", "dir");
    p.addOptions({oBackend, oDevice, oConnect, oLight, oSelf});
    p.process(app);
    if (!p.isSet(oLight)) darkTheme(app);

    GuiOptions o;
    o.backend = p.value(oBackend);
    o.device = p.value(oDevice);
    o.autoConnect = p.isSet(oConnect) && !p.isSet(oSelf);
    if (p.isSet(oSelf)) o.backend = "sim";
    MainWindow w(o);
    w.show();
    if (p.isSet(oSelf)) return selfTest(app, w, p.value(oSelf));
    return app.exec();
}
