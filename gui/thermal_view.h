#pragma once
// Live thermal image with spot / box tools and hot/cold markers, plus a colour bar.
#include <QImage>
#include <QPointF>
#include <QWidget>
#include <memory>
#include <vector>

#include "a70/thermal.h"

class ThermalView : public QWidget {
    Q_OBJECT
public:
    explicit ThermalView(QWidget* parent = nullptr);

    // rgb: w*h*3, temp: w*h °C (shared so we don't copy per frame)
    void setFrame(std::shared_ptr<std::vector<uint8_t>> rgb, std::shared_ptr<std::vector<float>> temp, int w, int h);
    void clearFrame();
    void setRois(const std::vector<a70::Roi>& rois);
    void setResults(const std::vector<a70::RoiResult>& res);
    void setShowExtremes(bool on);
    const std::vector<a70::Roi>& rois() const { return rois_; }
    void setOverlayText(const QString& t, const QColor& c);

signals:
    void hovered(int x, int y, double tempC);  // x = -1 when leaving
    void roisEdited(std::vector<a70::Roi> rois);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    QRectF target() const;
    bool toImage(const QPointF& p, int& x, int& y) const;
    QPointF toWidget(double x, double y) const;
    QString nextName(a70::Roi::Kind k) const;
    void removeNearest(int x, int y);

    QImage img_;
    std::shared_ptr<std::vector<uint8_t>> rgb_;
    std::shared_ptr<std::vector<float>> temp_;
    int w_ = 0, h_ = 0;
    std::vector<a70::Roi> rois_;
    std::vector<a70::RoiResult> res_;
    bool showExtremes_ = true;
    bool dragging_ = false;
    int dx0_ = 0, dy0_ = 0, dx1_ = 0, dy1_ = 0;
    QString overlay_;
    QColor overlayColor_;
};

class ColorBar : public QWidget {
    Q_OBJECT
public:
    explicit ColorBar(QWidget* parent = nullptr);
    void setRange(double lo, double hi, const QString& palette);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    double lo_ = 20, hi_ = 40;
    QString palette_ = "Iron";
};
