#include "feature_browser.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>

static const QStringList kCols = {"Category", "Name", "Type", "Value", "Unit", "Access", "Description"};

FeatureBrowser::FeatureBrowser(a70::A70Camera* cam, QWidget* parent) : QDialog(parent), cam_(cam) {
    setWindowTitle("GenICam feature browser");
    resize(1000, 640);
    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(QString::fromUtf8("Filter (name, category, description)…"));
    connect(filter_, &QLineEdit::textChanged, this, &FeatureBrowser::applyFilter);
    auto* bRef = new QPushButton("Refresh");
    connect(bRef, &QPushButton::clicked, this, &FeatureBrowser::reload);
    auto* bCsv = new QPushButton(QString::fromUtf8("Export CSV…"));
    connect(bCsv, &QPushButton::clicked, this, &FeatureBrowser::exportCsv);
    top->addWidget(filter_, 1);
    top->addWidget(bRef);
    top->addWidget(bCsv);
    lay->addLayout(top);
    table_ = new QTableWidget(0, kCols.size());
    table_->setHorizontalHeaderLabels(kCols);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->verticalHeader()->setVisible(false);
    connect(table_, &QTableWidget::cellDoubleClicked, this, &FeatureBrowser::edit);
    lay->addWidget(table_, 1);
    auto* hint = new QLabel(
        "Double-click a writable value to change it, or a Command to execute it. Change image size / pixel "
        "format / IRFormat from the main window, not here, while streaming.");
    hint->setWordWrap(true);
    lay->addWidget(hint);
    reload();
}

void FeatureBrowser::reload() {
    try {
        std::lock_guard<std::recursive_mutex> lk(cam_->mutex());
        feats_ = cam_->backend().features();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Features", e.what());
        feats_.clear();
    }
    table_->setSortingEnabled(false);
    table_->setRowCount(int(feats_.size()));
    for (int r = 0; r < int(feats_.size()); ++r) {
        const auto& f = feats_[size_t(r)];
        QString acc = QString(f.readable ? "R" : "") + (f.writable ? "W" : "");
        QStringList v = {QString::fromStdString(f.category), QString::fromStdString(f.name), a70::toString(f.type),
                         QString::fromStdString(f.value), QString::fromStdString(f.unit), acc,
                         QString::fromStdString(f.description)};
        for (int c = 0; c < v.size(); ++c) {
            auto* it = new QTableWidgetItem(v[c]);
            it->setData(Qt::UserRole, r);
            if (c == 3 && f.writable) it->setForeground(QColor(240, 150, 60));
            table_->setItem(r, c, it);
        }
    }
    table_->setSortingEnabled(true);
    table_->resizeColumnsToContents();
    table_->setColumnWidth(6, 320);
    applyFilter(filter_->text());
}

void FeatureBrowser::applyFilter(const QString& text) {
    QString t = text.trimmed().toLower();
    for (int r = 0; r < table_->rowCount(); ++r) {
        QString s;
        for (int c : {0, 1, 6})
            if (table_->item(r, c)) s += table_->item(r, c)->text() + " ";
        table_->setRowHidden(r, !t.isEmpty() && !s.toLower().contains(t));
    }
}

void FeatureBrowser::edit(int row, int) {
    auto* it = table_->item(row, 1);
    if (!it) return;
    const auto& f = feats_[size_t(it->data(Qt::UserRole).toInt())];
    QString name = QString::fromStdString(f.name);
    try {
        if (f.type == a70::FeatureType::Command) {
            if (QMessageBox::question(this, "Execute", "Execute command " + name + "?") == QMessageBox::Yes) {
                std::lock_guard<std::recursive_mutex> lk(cam_->mutex());
                cam_->backend().execute(f.name);
            }
            return;
        }
        if (!f.writable) return;
        bool ok = false;
        QString v;
        if (f.type == a70::FeatureType::Enumeration && !f.choices.empty()) {
            QStringList ch;
            for (auto& c : f.choices) ch << QString::fromStdString(c);
            v = QInputDialog::getItem(this, name, name, ch, std::max(0, int(ch.indexOf(QString::fromStdString(f.value)))),
                                      false, &ok);
        } else if (f.type == a70::FeatureType::Boolean) {
            v = QInputDialog::getItem(this, name, name, {"true", "false"}, f.value == "true" || f.value == "1" ? 0 : 1,
                                      false, &ok);
        } else {
            v = QInputDialog::getText(this, name, name + " (" + a70::toString(f.type) + " " +
                                                      QString::fromStdString(f.unit) + ")",
                                      QLineEdit::Normal, QString::fromStdString(f.value), &ok);
        }
        if (ok) {
            {
                std::lock_guard<std::recursive_mutex> lk(cam_->mutex());
                cam_->backend().setString(f.name, v.toStdString());
            }
            reload();
        }
    } catch (const std::exception& e) {
        QMessageBox::warning(this, name, e.what());
    }
}

void FeatureBrowser::exportCsv() {
    QString path = QFileDialog::getSaveFileName(this, "Export features", "a70_features.csv", "CSV (*.csv)");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream o(&file);
    auto q = [](const std::string& s) {
        QString x = QString::fromStdString(s);
        x.replace("\"", "\"\"");
        return "\"" + x + "\"";
    };
    o << "category,name,type,value,unit,access,choices,description\n";
    for (auto& f : feats_) {
        std::string ch;
        for (auto& c : f.choices) ch += (ch.empty() ? "" : "|") + c;
        o << q(f.category) << ',' << q(f.name) << ',' << a70::toString(f.type) << ',' << q(f.value) << ',' << q(f.unit)
          << ',' << (f.readable ? "R" : "") << (f.writable ? "W" : "") << ',' << q(ch) << ',' << q(f.description)
          << '\n';
    }
}
