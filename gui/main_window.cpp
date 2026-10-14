#include "main_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>
#ifdef A70_HAVE_SVG
#include <QPainter>
#include <QSvgRenderer>
#endif

#include "feature_browser.h"
#include "thermal_view.h"

using namespace a70;

static QWidget* row(std::initializer_list<QWidget*> ws, int stretchIndex = -1) {
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    int i = 0;
    for (auto* x : ws) l->addWidget(x, i++ == stretchIndex ? 1 : 0);
    return w;
}

MainWindow::MainWindow(const GuiOptions& opt, QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("FLIR A70 Radiometric Capture");
    resize(1400, 860);

    auto* central = new QWidget;
    auto* outerV = new QVBoxLayout(central);
    outerV->setContentsMargins(0, 0, 0, 0);
    outerV->setSpacing(0);
    outerV->addWidget(buildBrandHeader());

    auto* h = new QHBoxLayout;
    h->setContentsMargins(6, 6, 6, 6);
    auto* left = new QVBoxLayout;
    auto* imgRow = new QHBoxLayout;
    view_ = new ThermalView;
    cbar_ = new ColorBar;
    imgRow->addWidget(view_, 1);
    imgRow->addWidget(cbar_);
    left->addLayout(imgRow, 1);
    auto* info = new QHBoxLayout;
    lblCursor_ = new QLabel(QString::fromUtf8("Cursor: –"));
    lblStats_ = new QLabel(QString::fromUtf8("Frame: –"));
    lblFps_ = new QLabel(QString::fromUtf8("– fps"));
    lblHealth_ = new QLabel;
    for (auto* l : {lblCursor_, lblStats_, lblFps_}) l->setStyleSheet("font-family: monospace;");
    info->addWidget(lblHealth_);
    info->addWidget(lblCursor_);
    info->addStretch(1);
    info->addWidget(lblStats_);
    info->addStretch(1);
    info->addWidget(lblFps_);
    left->addLayout(info);
    auto* tip = new QLabel(QString::fromUtf8(
        "Click = spot · drag = box · right-click = remove nearest · Space/F5 = snapshot · Ctrl+R = record"));
    tip->setStyleSheet("color: gray;");
    left->addWidget(tip);
    h->addLayout(left, 1);

    auto* panel = new QTabWidget;
    panel_ = panel;
    panel->addTab(buildConnection(), "Connection");
    panel->addTab(buildDisplay(), "Display");
    panel->addTab(buildCamera(), "Camera");
    panel->addTab(buildCapture(), "Capture");
    panel->addTab(buildModbus(), "Modbus");
    panel->addTab(buildMeasurements(), "Measurements");
    auto* scroll = new QScrollArea;
    scroll->setWidget(panel);
    scroll->setWidgetResizable(true);
    scroll->setFixedWidth(600);  // wide enough that all 6 tab labels fit without overflow arrows
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    panelScroll_ = scroll;
    h->addWidget(scroll);
    outerV->addLayout(h, 1);
    setCentralWidget(central);
    statusBar()->showMessage(QString("Ready (Qt %1)").arg(qVersion()));

    // always-visible toolbar: day-to-day actions stay reachable with the settings
    // drawer closed. btnConnect_ / btnRecord_ were constructed (and wired to their
    // slots) inside buildConnection()/buildCapture() above; they live here instead.
    auto* tb = addToolBar("Main");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btnSettings_ = new QToolButton;
    btnSettings_->setText(QString::fromUtf8("⚙  Settings"));
    btnSettings_->setCheckable(true);
    btnSettings_->setToolTip("Show/hide connection, display, camera, capture, Modbus and measurement settings (Ctrl+,)");
    connect(btnSettings_, &QToolButton::toggled, this, &MainWindow::setPanelVisible);
    tb->addWidget(btnSettings_);
    tb->addSeparator();
    tb->addWidget(btnConnect_);
    btnSnapshot_ = new QPushButton(QString::fromUtf8("📷  Snapshot"));
    connect(btnSnapshot_, &QPushButton::clicked, this, &MainWindow::snapshot);
    tb->addWidget(btnSnapshot_);
    tb->addWidget(btnRecord_);
    new QShortcut(QKeySequence("Ctrl+,"), this, [this] { btnSettings_->toggle(); });

    const bool panelVisible = settings_.value("ui/panelVisible", false).toBool();
    btnSettings_->setChecked(panelVisible);
    setPanelVisible(panelVisible);  // toggled() above only fires on a change

    connect(view_, &ThermalView::hovered, this, [this](int x, int y, double t) {
        lblCursor_->setText(x < 0 ? QString::fromUtf8("Cursor: –")
                                  : QString::fromUtf8("(%1,%2) %3 °C").arg(x, 3).arg(y, 3).arg(t, 7, 'f', 2));
    });
    connect(view_, &ThermalView::roisEdited, this, &MainWindow::onRoisEdited);

    new QShortcut(QKeySequence(Qt::Key_Space), this, [this] { snapshot(); });
    new QShortcut(QKeySequence(Qt::Key_F5), this, [this] { snapshot(); });
    new QShortcut(QKeySequence("Ctrl+R"), this, [this] {
        if (btnRecord_->isEnabled()) btnRecord_->toggle();
    });

    statusTimer_ = new QTimer(this);
    connect(statusTimer_, &QTimer::timeout, this, &MainWindow::updateStatus);
    statusTimer_->start(500);

    if (!opt.backend.isEmpty()) {
        int i = cmbBackend_->findData(opt.backend);
        if (i >= 0) cmbBackend_->setCurrentIndex(i);
    }
    if (!opt.device.isEmpty()) cmbDevice_->setEditText(opt.device);
    setConnectedUi(false);
    if (opt.autoConnect) QTimer::singleShot(100, this, &MainWindow::connectCamera);
}

MainWindow::~MainWindow() { disconnectCamera(); }

// ================================================================ panels
QWidget* MainWindow::buildBrandHeader() {
    auto* header = new QWidget;
    header->setObjectName("brandHeader");
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(16, 10, 16, 10);
    hl->setSpacing(14);

    auto* logo = new QLabel;
#ifdef A70_HAVE_SVG
    QSvgRenderer renderer(QString(":/pertamina_white.svg"));
    if (renderer.isValid()) {
        QSize sz = renderer.defaultSize().scaled(140, 48, Qt::KeepAspectRatio);
        QPixmap pix(sz);
        pix.fill(Qt::transparent);
        QPainter p(&pix);
        renderer.render(&p, QRectF(0, 0, sz.width(), sz.height()));
        logo->setPixmap(pix);
    } else {
        logo->setText("PERTAMINA");
    }
#else
    logo->setText("PERTAMINA");
#endif
    logo->setObjectName("brandLogo");
    hl->addWidget(logo);

    auto* titles = new QVBoxLayout;
    titles->setSpacing(0);
    auto* line1 = new QLabel(QString::fromUtf8("PT Pertamina (Persero) — Refinery Unit IV Cilacap"));
    line1->setObjectName("brandTitle1");
    auto* line2 = new QLabel(QString::fromUtf8("FLIR A70 Radiometric CUI Monitor"));
    line2->setObjectName("brandTitle2");
    titles->addWidget(line1);
    titles->addWidget(line2);
    hl->addLayout(titles);
    hl->addStretch(1);
    return header;
}

QGroupBox* MainWindow::buildConnection() {
    auto* g = new QGroupBox("Connection");
    auto* f = new QFormLayout(g);
    cmbBackend_ = new QComboBox;
    for (auto& b : compiledBackends()) cmbBackend_->addItem(QString::fromStdString(b.label), QString::fromStdString(b.key));
    int bi = cmbBackend_->findData(settings_.value("backend", QString::fromStdString(defaultBackend())));
    if (bi >= 0) cmbBackend_->setCurrentIndex(bi);
    connect(cmbBackend_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { cmbDevice_->clear(); });
    f->addRow("Backend", cmbBackend_);
    cmbDevice_ = new QComboBox;
    cmbDevice_->setEditable(true);
    cmbDevice_->setToolTip("Pick a discovered camera, or type its IP address / serial number");
    cmbDevice_->setEditText(settings_.value("device").toString());
    auto* bDisc = new QToolButton;
    bDisc->setText(QString::fromUtf8("⟳"));
    bDisc->setToolTip("Discover cameras");
    connect(bDisc, &QToolButton::clicked, this, &MainWindow::refreshDevices);
    f->addRow("Camera", row({cmbDevice_, bDisc}, 0));
    cmbIrFmt_ = new QComboBox;
    cmbIrFmt_->addItem(QString::fromUtf8("0.01 K steps  (≤ 382 °C)"), "TemperatureLinear10mK");
    cmbIrFmt_->addItem(QString::fromUtf8("0.1 K steps  (high-temp range)"), "TemperatureLinear100mK");
    connect(cmbIrFmt_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::irFormatChanged);
    f->addRow("Resolution", cmbIrFmt_);
    btnConnect_ = new QPushButton("Connect");
    connect(btnConnect_, &QPushButton::clicked, this, [this] { eng_ ? disconnectCamera() : connectCamera(); });
    // placed on the always-visible toolbar, not here — see MainWindow::MainWindow
    return g;
}

QGroupBox* MainWindow::buildDisplay() {
    auto* g = new QGroupBox("Display");
    auto* f = new QFormLayout(g);
    cmbPalette_ = new QComboBox;
    for (auto& p : paletteNames()) cmbPalette_->addItem(QString::fromStdString(p));
    cmbPalette_->setCurrentText(settings_.value("palette", "Iron").toString());
    f->addRow("Palette", cmbPalette_);
    chkAuto_ = new QCheckBox("Auto range");
    chkAuto_->setChecked(true);
    chkBoxRange_ = new QCheckBox("from first box");
    chkBoxRange_->setToolTip("Scale colours to the first box only — brings out subtle insulation anomalies when "
                             "hot flanges or steam dominate the frame");
    f->addRow(row({chkAuto_, chkBoxRange_}));
    spLo_ = new QDoubleSpinBox;
    spHi_ = new QDoubleSpinBox;
    for (auto* s : {spLo_, spHi_}) {
        s->setRange(-60, 1500);
        s->setDecimals(1);
        s->setSuffix(QString::fromUtf8(" °C"));
        s->setEnabled(false);
    }
    spLo_->setValue(20);
    spHi_->setValue(40);
    f->addRow("Range", row({spLo_, new QLabel("to"), spHi_}));
    connect(chkAuto_, &QCheckBox::toggled, this, [this](bool on) {
        spLo_->setEnabled(!on);
        spHi_->setEnabled(!on);
    });
    chkExtremes_ = new QCheckBox("Hot/cold markers");
    chkExtremes_->setChecked(true);
    connect(chkExtremes_, &QCheckBox::toggled, view_, &ThermalView::setShowExtremes);
    auto* bClear = new QPushButton("Clear tools");
    connect(bClear, &QPushButton::clicked, this, [this] { onRoisEdited({}); });
    f->addRow(row({chkExtremes_, bClear}));
    return g;
}

QGroupBox* MainWindow::buildCamera() {
    grpCamera_ = new QGroupBox("Camera / object parameters");
    auto* v = new QVBoxLayout(grpCamera_);
    paramForm_ = new QFormLayout;
    v->addLayout(paramForm_);
    auto* bApply = new QPushButton("Apply");
    bApply->setToolTip("Write object parameters to the camera. They are applied inside the camera before "
                       "temperatures are encoded — set them BEFORE capturing. Re-applied after reconnects.");
    connect(bApply, &QPushButton::clicked, this, &MainWindow::applyParams);
    auto* bRead = new QPushButton("Read back");
    connect(bRead, &QPushButton::clicked, this, &MainWindow::readParams);
    v->addWidget(row({bApply, bRead}));
    auto* f2 = new QFormLayout;
    spFps_ = new QDoubleSpinBox;
    spFps_->setRange(0.5, 60);
    spFps_->setDecimals(1);
    spFps_->setSuffix(" Hz");
    connect(spFps_, &QDoubleSpinBox::editingFinished, this, [this] {
        A70Camera* c = eng_ ? eng_->camera() : nullptr;
        if (!c) return;
        try {
            c->setFrameRate(spFps_->value());
            eng_->config().frameRate = spFps_->value();
        } catch (const std::exception& e) {
            err(QString("Frame rate: ") + e.what());
        }
    });
    f2->addRow("Frame rate", spFps_);
    cmbRange_ = new QComboBox;
    connect(cmbRange_, QOverload<int>::of(&QComboBox::activated), this, [this] {
        A70Camera* c = eng_ ? eng_->camera() : nullptr;
        if (!c) return;
        try {
            c->setRange(cmbRange_->currentText().toStdString());
            eng_->config().range = cmbRange_->currentText().toStdString();
            if (c->maxTempC() < 400 && cmbRange_->currentText().contains("1000"))
                err(QString::fromUtf8("High range selected: switch Resolution to 0.1 K steps — 0.01 K tops out at 382 °C"),
                    true);
        } catch (const std::exception& e) {
            err(QString("Range: ") + e.what(), true);
        }
    });
    f2->addRow("Meas. range", cmbRange_);
    v->addLayout(f2);
    btnNuc_ = new QPushButton("NUC");
    btnNuc_->setToolTip("Non-uniformity correction. Do it after warm-up and before critical captures.");
    connect(btnNuc_, &QPushButton::clicked, this, [this] {
        std::string e;
        if (eng_ && eng_->command(Command::Nuc, &e)) statusBar()->showMessage("NUC performed", 4000);
        else err(QString("NUC: ") + QString::fromStdString(e), true);
    });
    auto* bFeat = new QPushButton(QString::fromUtf8("All features…"));
    connect(bFeat, &QPushButton::clicked, this, &MainWindow::showFeatures);
    v->addWidget(row({btnNuc_, bFeat}));
    return grpCamera_;
}

QGroupBox* MainWindow::buildCapture() {
    grpCapture_ = new QGroupBox("Capture");
    auto* f = new QFormLayout(grpCapture_);
    edFolder_ = new QLineEdit(settings_.value("folder", QDir::home().filePath("a70_captures")).toString());
    auto* bF = new QToolButton;
    bF->setText(QString::fromUtf8("…"));
    connect(bF, &QToolButton::clicked, this, [this] {
        QString p = QFileDialog::getExistingDirectory(this, "Output folder", edFolder_->text());
        if (!p.isEmpty()) edFolder_->setText(p);
    });
    f->addRow("Folder", row({edFolder_, bF}, 0));
    edPrefix_ = new QLineEdit(settings_.value("prefix", "a70").toString());
    edPrefix_->setToolTip("File name prefix — use the line / tag number, e.g. PIPE-101");
    f->addRow("Prefix / tag", edPrefix_);
    chkCsv_ = new QCheckBox(QString::fromUtf8("also °C CSV"));
    f->addRow(chkCsv_);
    chkInterval_ = new QCheckBox("Every");
    spInterval_ = new QSpinBox;
    spInterval_->setRange(1, 86400);
    spInterval_->setValue(60);
    spInterval_->setSuffix(" s");
    connect(chkInterval_, &QCheckBox::toggled, this, &MainWindow::toggleInterval);
    auto* iw = row({chkInterval_, spInterval_, new QLabel("snapshot + CSV log")});
    iw->setToolTip("Time-lapse: radiometric snapshot + spot/box readings appended to <prefix>_measurements_log.csv");
    f->addRow("Interval", iw);
    cmbComp_ = new QComboBox;
    cmbComp_->addItem("gzip (smaller)", "gzip");
    cmbComp_->addItem("none (fastest)", "none");
    spDecim_ = new QSpinBox;
    spDecim_->setRange(1, 1000);
    spDecim_->setPrefix("keep 1/");
    spDecim_->setToolTip("Save every Nth frame (30 Hz / 30 = 1 Hz)");
    f->addRow("Video", row({cmbComp_, spDecim_}));
    spDur_ = new QSpinBox;
    spDur_->setRange(0, 7 * 86400);
    spDur_->setSuffix(" s");
    spDur_->setSpecialValueText("unlimited");
    f->addRow("Duration", spDur_);
    btnRecord_ = new QPushButton(QString::fromUtf8("⏺  Record radiometric video"));
    btnRecord_->setCheckable(true);
    btnRecord_->setEnabled(hdf5Available());
    if (!hdf5Available()) btnRecord_->setToolTip("This build has no HDF5 support");
    connect(btnRecord_, &QPushButton::toggled, this, &MainWindow::toggleRecord);
    // placed on the always-visible toolbar, not here — see MainWindow::MainWindow
    lblRec_ = new QLabel;
    lblRec_->setWordWrap(true);
    f->addRow(lblRec_);
    return grpCapture_;
}

QGroupBox* MainWindow::buildModbus() {
    auto* g = new QGroupBox("Modbus TCP server");
    auto* f = new QFormLayout(g);
    chkModbus_ = new QCheckBox("Serve live data / alarms / commands");
    chkModbus_->setEnabled(modbusAvailable());
    connect(chkModbus_, &QCheckBox::toggled, this, &MainWindow::toggleModbus);
    f->addRow(chkModbus_);
    edBind_ = new QLineEdit(settings_.value("modbus_bind", "0.0.0.0").toString());
    edBind_->setToolTip("Bind to the control-network interface IP (Modbus has no authentication)");
    spPort_ = new QSpinBox;
    spPort_->setRange(1, 65535);
    spPort_->setValue(settings_.value("modbus_port", 502).toInt());
    f->addRow("Bind / port", row({edBind_, spPort_}, 0));
    lblModbus_ = new QLabel(modbusAvailable() ? "stopped" : "not available in this build");
    f->addRow(lblModbus_);
    return g;
}

QGroupBox* MainWindow::buildMeasurements() {
    auto* g = new QGroupBox(QString::fromUtf8("Measurements (°C)"));
    auto* v = new QVBoxLayout(g);
    tbl_ = new QTableWidget(0, 6);
    tbl_->setHorizontalHeaderLabels({"Tool", "Max", "Min", "Avg", "Alarm >", "State"});
    tbl_->verticalHeader()->setVisible(false);
    tbl_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tbl_->setMinimumHeight(150);
    tbl_->setToolTip("Double-click 'Alarm >' to set a threshold (empty = off). Also settable over Modbus.");
    connect(tbl_, &QTableWidget::cellChanged, this, &MainWindow::onAlarmEdited);
    v->addWidget(tbl_);
    return g;
}

// ================================================================ helpers
void MainWindow::err(const QString& msg, bool popup) {
    statusBar()->showMessage(msg, 10000);
    if (popup) QMessageBox::warning(this, "A70", msg);
}

void MainWindow::setConnectedUi(bool on) {
    btnConnect_->setText(on ? "Disconnect" : "Connect");
    cmbBackend_->setEnabled(!on);
    cmbDevice_->setEnabled(!on);
    grpCamera_->setEnabled(on);
    grpCapture_->setEnabled(on);
    if (!on) {
        view_->clearFrame();
        lblHealth_->clear();
    }
}

void MainWindow::setOutput(const QString& folder, const QString& prefix) {
    edFolder_->setText(folder);
    edPrefix_->setText(prefix);
}
void MainWindow::setBoxRange(bool on) { chkBoxRange_->setChecked(on); }
void MainWindow::enableModbus(int port) {
    spPort_->setValue(port);
    chkModbus_->setChecked(true);
}

void MainWindow::setPanelVisible(bool on) {
    panelScroll_->setVisible(on);
    if (btnSettings_->isChecked() != on) {
        QSignalBlocker b(btnSettings_);
        btnSettings_->setChecked(on);
    }
    settings_.setValue("ui/panelVisible", on);
}

void MainWindow::syncConfig() {
    if (!eng_) return;
    auto& c = eng_->config();
    c.folder = edFolder_->text().trimmed().isEmpty() ? QDir::home().filePath("a70_captures").toStdString()
                                                      : edFolder_->text().trimmed().toStdString();
    c.prefix = edPrefix_->text().trimmed().isEmpty() ? "a70" : edPrefix_->text().trimmed().toStdString();
    c.palette = cmbPalette_->currentText().toStdString();
    c.csvWithSnapshots = chkCsv_->isChecked();
    settings_.setValue("folder", edFolder_->text());
    settings_.setValue("prefix", edPrefix_->text());
    settings_.setValue("palette", cmbPalette_->currentText());
}

// ================================================================ connection
void MainWindow::refreshDevices() {
    try {
        A70Camera probe(cmbBackend_->currentData().toString().toStdString());
        auto devs = probe.listDevices();
        cmbDevice_->clear();
        for (auto& d : devs) cmbDevice_->addItem(QString::fromStdString(d.label()), QString::fromStdString(d.id));
        if (devs.empty()) err("No cameras found. Check cabling / IP subnet / firewall, or type the IP address.");
        else statusBar()->showMessage(QString("Found %1 camera(s)").arg(devs.size()), 5000);
    } catch (const std::exception& e) {
        err(QString("Discovery failed: ") + e.what(), true);
    }
}

void MainWindow::connectCamera() {
    if (eng_) return;
    EngineConfig c;
    c.backend = cmbBackend_->currentData().toString().toStdString();
    int i = cmbDevice_->currentIndex();
    QString txt = cmbDevice_->currentText().trimmed();
    c.device = (i >= 0 && cmbDevice_->itemText(i) == txt) ? cmbDevice_->itemData(i).toString().toStdString()
                                                         : txt.toStdString();
    c.irFormat = cmbIrFmt_->currentData().toString().toStdString();
    c.reconnect = true;  // survive cable / power blips once connected
    eng_ = std::make_unique<CaptureEngine>(c);
    syncConfig();
    eng_->setLogCallback([this](const std::string& lvl, const std::string& msg) {
        QMetaObject::invokeMethod(this, [this, l = QString::fromStdString(lvl), m = QString::fromStdString(msg)] {
            onLog(l, m);
        }, Qt::QueuedConnection);
    });
    eng_->setFrameCallback([this](std::shared_ptr<const ThermalFrame> f) {
        QMetaObject::invokeMethod(this, [this, f] { onFrame(f); }, Qt::QueuedConnection);
    });
    eng_->setRois(rois_);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string e;
    bool ok = eng_->start(&e);
    QApplication::restoreOverrideCursor();
    if (!ok) {  // first attempt failed: don't silently retry a wrong IP forever
        eng_->stop();
        eng_.reset();
        err(QString::fromStdString(e), true);
        return;
    }
    settings_.setValue("backend", cmbBackend_->currentData());
    settings_.setValue("device", cmbDevice_->currentText());
    auto st = eng_->status();
    setWindowTitle("FLIR A70 Radiometric Capture — " + QString::fromStdString(st.device.label()));
    populateCameraControls();
    setConnectedUi(true);
}

void MainWindow::disconnectCamera() {
    if (chkModbus_ && chkModbus_->isChecked()) chkModbus_->setChecked(false);
    if (chkInterval_ && chkInterval_->isChecked()) chkInterval_->setChecked(false);
    if (btnRecord_ && btnRecord_->isChecked()) btnRecord_->setChecked(false);
    modbus_.reset();
    if (eng_) {
        eng_->setFrameCallback(nullptr);
        eng_->setLogCallback(nullptr);
        eng_->stop();
        eng_.reset();
    }
    if (btnConnect_) setConnectedUi(false);
    setWindowTitle("FLIR A70 Radiometric Capture");
}

void MainWindow::irFormatChanged() {
    if (!eng_) return;
    std::string e;
    if (!eng_->changeIrFormat(cmbIrFmt_->currentData().toString().toStdString(), &e))
        err(QString("Resolution change failed: ") + QString::fromStdString(e), true);
}

// ================================================================ camera params
void MainWindow::populateCameraControls() {
    while (paramForm_->rowCount()) paramForm_->removeRow(0);
    paramBoxes_.clear();
    A70Camera* c = eng_->camera();
    if (!c) return;
    for (const std::string& key : c->paramKeys()) {
        for (auto& s : paramSpecs()) {
            if (s.key != key) continue;
            auto* sb = new QDoubleSpinBox;
            sb->setRange(s.min, s.max);
            sb->setSingleStep(s.step);
            sb->setDecimals(s.decimals);
            if (!s.unit.empty()) sb->setSuffix(" " + QString::fromStdString(s.unit));
            paramBoxes_[key] = sb;
            paramForm_->addRow(QString::fromStdString(s.label), sb);
        }
    }
    if (paramBoxes_.empty())
        paramForm_->addRow(new QLabel(QString::fromUtf8("No object-parameter features found — see All features…")));
    readParams();
    try {
        auto fr = c->getFrameRate();
        spFps_->setEnabled(fr.has_value());
        if (fr) spFps_->setValue(*fr);
    } catch (...) {
        spFps_->setEnabled(false);
    }
    cmbRange_->clear();
    try {
        for (auto& r : c->ranges()) cmbRange_->addItem(QString::fromStdString(r));
        if (auto cur = c->getRange()) cmbRange_->setCurrentText(QString::fromStdString(*cur));
    } catch (...) {
    }
    cmbRange_->setEnabled(cmbRange_->count() > 0);
    btnNuc_->setEnabled(c->hasNuc());
}

void MainWindow::readParams() {
    A70Camera* c = eng_ ? eng_->camera() : nullptr;
    if (!c) return;
    for (auto& kv : paramBoxes_) {
        try {
            if (auto v = c->getParam(kv.first)) {
                QSignalBlocker b(kv.second);
                kv.second->setValue(*v);
            }
        } catch (const std::exception& e) {
            err(QString("Read %1: %2").arg(QString::fromStdString(kv.first), e.what()));
        }
    }
}

void MainWindow::applyParams() {
    if (!eng_) return;
    QStringList errs;
    for (auto& kv : paramBoxes_) {
        try {
            eng_->setParamPersistent(kv.first, kv.second->value());
        } catch (const std::exception& e) {
            errs << QString::fromStdString(kv.first) + ": " + e.what();
        }
    }
    readParams();
    if (!errs.isEmpty()) err(errs.join("; "), true);
    else statusBar()->showMessage("Object parameters applied", 4000);
}

void MainWindow::showFeatures() {
    A70Camera* c = eng_ ? eng_->camera() : nullptr;
    if (!c) return;
    FeatureBrowser dlg(c, this);
    dlg.exec();
}

// ================================================================ frames
void MainWindow::onFrame(std::shared_ptr<const ThermalFrame> f) {
    if (!eng_ || !f) return;
    last_ = f;
    auto temp = std::make_shared<std::vector<float>>();
    f->toCelsius(*temp);
    double lo, hi;
    if (chkAuto_->isChecked()) {
        const Roi* box = nullptr;
        if (chkBoxRange_->isChecked())
            for (auto& r : rois_)
                if (r.kind == Roi::Kind::Box) {
                    box = &r;
                    break;
                }
        autoRange(temp->data(), f->width, f->height, lo, hi, 0.5, box);
        const double a = 0.3;  // smooth so the palette doesn't flicker
        lo = lo_ + a * (lo - lo_);
        hi = hi_ + a * (hi - hi_);
        QSignalBlocker b1(spLo_), b2(spHi_);
        spLo_->setValue(lo);
        spHi_->setValue(hi);
    } else {
        lo = spLo_->value();
        hi = std::max(spHi_->value(), lo + 0.1);
    }
    lo_ = lo;
    hi_ = hi;
    auto rgb = std::make_shared<std::vector<uint8_t>>(temp->size() * 3);
    QString pal = cmbPalette_->currentText();
    colorize(temp->data(), temp->size(), lo, hi, pal.toStdString(), rgb->data());
    view_->setFrame(rgb, temp, f->width, f->height);
    cbar_->setRange(lo, hi, pal);
    auto st = computeStats(*f);
    lblStats_->setText(QString::fromUtf8("min %1  max %2  avg %3 °C")
                           .arg(st.minC, 6, 'f', 2).arg(st.maxC, 6, 'f', 2).arg(st.meanC, 6, 'f', 2));
    view_->setResults(measure(*f, rois_));
}

void MainWindow::onLog(const QString& level, const QString& msg) {
    statusBar()->showMessage(msg, level == "error" ? 15000 : 6000);
}

// ================================================================ tools / alarms
void MainWindow::onRoisEdited(std::vector<Roi> rois) {
    for (auto& r : rois)  // keep alarm thresholds of tools that still exist
        for (auto& o : rois_)
            if (o.name == r.name) r.alarmAboveC = o.alarmAboveC;
    rois_ = std::move(rois);
    view_->setRois(rois_);
    if (eng_) eng_->setRois(rois_);
    updateStatus();
}

void MainWindow::onAlarmEdited(int row, int col) {
    if (updatingTable_ || col != 4 || row < 0 || row >= int(rois_.size())) return;
    QString t = tbl_->item(row, col) ? tbl_->item(row, col)->text().trimmed() : QString();
    bool ok = false;
    double v = t.toDouble(&ok);
    rois_[size_t(row)].alarmAboveC = (t.isEmpty() || !ok) ? NAN : v;
    if (eng_) eng_->setRoiAlarm(size_t(row), rois_[size_t(row)].alarmAboveC);
}

void MainWindow::updateStatus() {
    EngineStatus s;
    if (eng_) {
        s = eng_->status();
        // alarms may have been changed over Modbus
        for (size_t i = 0; i < rois_.size() && i < s.rois.size(); ++i)
            if (rois_[i].name == s.rois[i].name) rois_[i].alarmAboveC = s.rois[i].alarmAboveC;
        lblFps_->setText(QString("%1 fps").arg(s.fps, 5, 'f', 1));
        if (!s.connected) {
            lblHealth_->setText(QString::fromUtf8("● RECONNECTING"));
            lblHealth_->setStyleSheet("color:#e0a030; font-weight:bold;");
            view_->setOverlayText(QString::fromUtf8("Camera lost — reconnecting… (last image is NOT live)"),
                                  QColor(255, 180, 60));
        } else if (!s.fresh) {
            lblHealth_->setText(QString::fromUtf8("● STALE"));
            lblHealth_->setStyleSheet("color:#e0a030; font-weight:bold;");
            view_->setOverlayText(QString::fromUtf8("No new frames — image is stale"), QColor(255, 180, 60));
        } else {
            lblHealth_->setText(s.anyAlarm ? QString::fromUtf8("● ALARM") : QString::fromUtf8("● LIVE"));
            // brand red/green — reserved for these two real engine states, not decoration
            lblHealth_->setStyleSheet(s.anyAlarm ? "color:#ED1B2F; font-weight:bold;" : "color:#ACC42A; font-weight:bold;");
            view_->setOverlayText({}, {});
        }
        if (s.recording || btnRecord_->isChecked()) {
            QString t = QString::fromUtf8("● REC %1\n%2 frames, %3 MB raw")
                            .arg(QFileInfo(QString::fromStdString(s.recPath)).fileName())
                            .arg(s.recWritten)
                            .arg(s.recBytes / 1e6, 0, 'f', 0);
            if (s.recDropped) t += QString(", %1 DROPPED (disk too slow: use 'none' or keep 1/N)").arg(s.recDropped);
            if (!s.recError.empty()) t += "\nERROR: " + QString::fromStdString(s.recError);
            lblRec_->setText(t);
            lblRec_->setStyleSheet("color:#e04040;");
            if (!s.recording && btnRecord_->isChecked()) {  // finished by duration / disk guard / error
                QSignalBlocker b(btnRecord_);
                btnRecord_->setChecked(false);
                toggleRecord(false);
            }
        }
        if (s.diskLow) lblRec_->setText(QString("Disk space low: %1 MB free").arg(s.freeMB, 0, 'f', 0));
    }
    if (modbus_) {
        lblModbus_->setText(QString("listening on %1:%2 — %3 client(s), %4 requests")
                                .arg(edBind_->text()).arg(modbus_->port()).arg(modbus_->clients()).arg(modbus_->requests()));
    }
    // measurement table
    updatingTable_ = true;
    QWidget* fw = QApplication::focusWidget();
    const bool editing = fw && fw != tbl_ && tbl_->isAncestorOf(fw);
    tbl_->setRowCount(int(rois_.size()));
    for (int r = 0; r < int(rois_.size()); ++r) {
        const Roi& roi = rois_[size_t(r)];
        const RoiResult* res = size_t(r) < s.roiResults.size() && s.roiResults[size_t(r)].name == roi.name &&
                                       s.roiResults[size_t(r)].valid
                                   ? &s.roiResults[size_t(r)]
                                   : nullptr;
        QStringList v = {QString::fromStdString(roi.name), res ? QString::number(res->maxC, 'f', 2) : "",
                         res && roi.kind == Roi::Kind::Box ? QString::number(res->minC, 'f', 2) : "",
                         res && roi.kind == Roi::Kind::Box ? QString::number(res->meanC, 'f', 2) : "",
                         std::isnan(roi.alarmAboveC) ? "" : QString::number(roi.alarmAboveC, 'f', 1),
                         res && res->alarm ? "ALARM" : (std::isnan(roi.alarmAboveC) ? "" : "ok")};
        for (int c = 0; c < v.size(); ++c) {
            if (c == 4 && editing && tbl_->currentRow() == r)
                continue;  // don't overwrite while the user types
            auto* it = tbl_->item(r, c);
            if (!it) {
                it = new QTableWidgetItem;
                tbl_->setItem(r, c, it);
            }
            it->setText(v[c]);
            Qt::ItemFlags fl = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (c == 4) fl |= Qt::ItemIsEditable;
            it->setFlags(fl);
            if (c == 5) it->setForeground(res && res->alarm ? QColor(255, 70, 70) : QColor(120, 200, 120));
        }
    }
    updatingTable_ = false;
}

// ================================================================ capture
void MainWindow::snapshot() {
    if (!eng_) return;
    syncConfig();
    std::string e;
    auto p = eng_->snapshot(&e, false);
    if (p) statusBar()->showMessage("Saved " + QFileInfo(QString::fromStdString(p->tiff)).fileName(), 5000);
    else err(QString::fromStdString(e));
}

void MainWindow::toggleInterval(bool on) {
    if (!eng_) return;
    syncConfig();
    if (on) eng_->startInterval(spInterval_->value());
    else eng_->stopInterval();
    spInterval_->setEnabled(!on);
}

void MainWindow::toggleRecord(bool on) {
    if (!eng_) {
        QSignalBlocker b(btnRecord_);
        btnRecord_->setChecked(false);
        return;
    }
    if (on) {
        syncConfig();
        std::string e;
        if (!eng_->startRecording(spDecim_->value(), cmbComp_->currentData().toString().toStdString(),
                                  spDur_->value(), &e)) {
            QSignalBlocker b(btnRecord_);
            btnRecord_->setChecked(false);
            err(QString("Record failed: ") + QString::fromStdString(e), true);
            return;
        }
        btnRecord_->setText(QString::fromUtf8("⏹  Stop recording"));
    } else {
        eng_->stopRecording();
        auto s = eng_->status();
        QString msg = QString("Saved %1: %2 frames").arg(QFileInfo(QString::fromStdString(s.recPath)).fileName()).arg(s.recWritten);
        if (s.recDropped) msg += QString(", %1 dropped").arg(s.recDropped);
        if (!s.recError.empty()) msg += " — ERROR " + QString::fromStdString(s.recError);
        lblRec_->setText(msg);
        lblRec_->setStyleSheet("");
        btnRecord_->setText(QString::fromUtf8("⏺  Record radiometric video"));
    }
    for (QWidget* w : std::initializer_list<QWidget*>{cmbComp_, spDecim_, spDur_, cmbIrFmt_}) w->setEnabled(!on);
}

void MainWindow::toggleModbus(bool on) {
    if (!on) {
        modbus_.reset();
        lblModbus_->setText("stopped");
        return;
    }
    if (!eng_) {
        QSignalBlocker b(chkModbus_);
        chkModbus_->setChecked(false);
        err("Connect the camera first", true);
        return;
    }
    modbus_ = std::make_unique<ModbusServer>(*eng_, edBind_->text().trimmed().toStdString(), spPort_->value());
    std::string e;
    if (!modbus_->start(&e)) {
        modbus_.reset();
        QSignalBlocker b(chkModbus_);
        chkModbus_->setChecked(false);
        err(QString::fromStdString(e), true);
        return;
    }
    settings_.setValue("modbus_bind", edBind_->text());
    settings_.setValue("modbus_port", spPort_->value());
}

void MainWindow::closeEvent(QCloseEvent* e) {
    disconnectCamera();
    QMainWindow::closeEvent(e);
}
