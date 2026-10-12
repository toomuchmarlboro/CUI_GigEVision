#pragma once
// Every GenICam feature on the camera: read, edit, execute, export CSV.
#include <QDialog>
#include <vector>

#include "a70/camera.h"

class QLineEdit;
class QTableWidget;

class FeatureBrowser : public QDialog {
    Q_OBJECT
public:
    FeatureBrowser(a70::A70Camera* cam, QWidget* parent = nullptr);

private slots:
    void reload();
    void applyFilter(const QString& text);
    void edit(int row, int col);
    void exportCsv();

private:
    a70::A70Camera* cam_;
    QLineEdit* filter_;
    QTableWidget* table_;
    std::vector<a70::Feature> feats_;
};
