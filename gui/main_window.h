#pragma once
#include <QMainWindow>
#include <QSettings>
#include <map>
#include <memory>

#include "a70/engine.h"
#include "a70/modbus_server.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;
class QToolButton;
class ThermalView;
class ColorBar;

struct GuiOptions {
    QString backend, device;
    bool autoConnect = false;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const GuiOptions& opt, QWidget* parent = nullptr);
    ~MainWindow() override;

    // used by the offscreen self-test
    ThermalView* view() const { return view_; }
    a70::CaptureEngine* engine() const { return eng_.get(); }
    void setOutput(const QString& folder, const QString& prefix);
    void setBoxRange(bool on);
    QWidget* panel() const { return panel_; }
    void enableModbus(int port);
    void setPanelVisible(bool on);

public slots:
    void connectCamera();
    void disconnectCamera();
    void snapshot();
    void refreshDevices();

protected:
    void closeEvent(QCloseEvent*) override;

private slots:
    void onFrame(std::shared_ptr<const a70::ThermalFrame> f);
    void onLog(const QString& level, const QString& msg);
    void onRoisEdited(std::vector<a70::Roi> rois);
    void onAlarmEdited(int row, int col);
    void updateStatus();
    void applyParams();
    void readParams();
    void toggleRecord(bool on);
    void toggleInterval(bool on);
    void toggleModbus(bool on);
    void irFormatChanged();
    void showFeatures();

private:
    QGroupBox* buildConnection();
    QGroupBox* buildDisplay();
    QGroupBox* buildCamera();
    QGroupBox* buildCapture();
    QGroupBox* buildModbus();
    QGroupBox* buildMeasurements();
    void populateCameraControls();
    void setConnectedUi(bool on);
    void syncConfig();
    void err(const QString& msg, bool popup = false);

    QSettings settings_{"a70_thermal", "a70_gui_cpp"};
    std::unique_ptr<a70::CaptureEngine> eng_;
    std::unique_ptr<a70::ModbusServer> modbus_;
    std::vector<a70::Roi> rois_;
    double lo_ = 20, hi_ = 40;
    std::shared_ptr<const a70::ThermalFrame> last_;

    ThermalView* view_;
    QWidget* panel_ = nullptr;
    QWidget* panelScroll_ = nullptr;
    QToolButton* btnSettings_ = nullptr;
    QPushButton* btnSnapshot_ = nullptr;
    ColorBar* cbar_;
    QLabel *lblCursor_, *lblStats_, *lblFps_, *lblHealth_, *lblRec_, *lblModbus_;
    // connection
    QComboBox *cmbBackend_, *cmbDevice_, *cmbIrFmt_;
    QPushButton* btnConnect_;
    // display
    QComboBox* cmbPalette_;
    QCheckBox *chkAuto_, *chkBoxRange_, *chkExtremes_;
    QDoubleSpinBox *spLo_, *spHi_;
    // camera
    QGroupBox* grpCamera_;
    QFormLayout* paramForm_;
    std::map<std::string, QDoubleSpinBox*> paramBoxes_;
    QDoubleSpinBox* spFps_;
    QComboBox* cmbRange_;
    QPushButton* btnNuc_;
    // capture
    QGroupBox* grpCapture_;
    QLineEdit *edFolder_, *edPrefix_;
    QCheckBox *chkCsv_, *chkInterval_;
    QSpinBox *spInterval_, *spDecim_, *spDur_;
    QComboBox* cmbComp_;
    QPushButton* btnRecord_;
    // modbus
    QCheckBox* chkModbus_;
    QLineEdit* edBind_;
    QSpinBox* spPort_;
    // measurements
    QTableWidget* tbl_;
    bool updatingTable_ = false;

    QTimer* statusTimer_;
};
