#include "thermal_view.h"

#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

using a70::Roi;

static constexpr int kMaxSpots = 6;
static constexpr int kMaxBoxes = 4;

ThermalView::ThermalView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void ThermalView::setFrame(std::shared_ptr<std::vector<uint8_t>> rgb, std::shared_ptr<std::vector<float>> temp, int w,
                           int h) {
    rgb_ = std::move(rgb);
    temp_ = std::move(temp);
    w_ = w;
    h_ = h;
    img_ = QImage(rgb_->data(), w, h, 3 * w, QImage::Format_RGB888);
    update();
}

void ThermalView::clearFrame() {
    img_ = QImage();
    rgb_.reset();
    temp_.reset();
    update();
}

void ThermalView::setRois(const std::vector<Roi>& r) {
    rois_ = r;
    update();
}
void ThermalView::setResults(const std::vector<a70::RoiResult>& r) {
    res_ = r;
    update();
}
void ThermalView::setShowExtremes(bool on) {
    showExtremes_ = on;
    update();
}
void ThermalView::setOverlayText(const QString& t, const QColor& c) {
    overlay_ = t;
    overlayColor_ = c;
    update();
}

QRectF ThermalView::target() const {
    if (img_.isNull()) return QRectF(rect());
    double s = std::min(width() / double(w_), height() / double(h_));
    double w = w_ * s, h = h_ * s;
    return QRectF((width() - w) / 2, (height() - h) / 2, w, h);
}

bool ThermalView::toImage(const QPointF& p, int& x, int& y) const {
    if (img_.isNull()) return false;
    QRectF r = target();
    double fx = (p.x() - r.x()) / r.width() * w_, fy = (p.y() - r.y()) / r.height() * h_;
    if (fx < 0 || fy < 0 || fx >= w_ || fy >= h_) return false;
    x = int(fx);
    y = int(fy);
    return true;
}

QPointF ThermalView::toWidget(double x, double y) const {
    QRectF r = target();
    return {r.x() + (x + 0.5) / w_ * r.width(), r.y() + (y + 0.5) / h_ * r.height()};
}

static QPointF evPos(QMouseEvent* e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return e->position();
#else
    return e->localPos();
#endif
}

QString ThermalView::nextName(Roi::Kind k) const {
    QString base = k == Roi::Kind::Spot ? "Sp" : "Bx";
    for (int i = 1;; ++i) {
        QString n = base + QString::number(i);
        bool used = std::any_of(rois_.begin(), rois_.end(), [&](const Roi& r) { return QString::fromStdString(r.name) == n; });
        if (!used) return n;
    }
}

void ThermalView::mousePressEvent(QMouseEvent* e) {
    int x, y;
    if (!toImage(evPos(e), x, y)) return;
    if (e->button() == Qt::LeftButton) {
        dragging_ = true;
        dx0_ = dx1_ = x;
        dy0_ = dy1_ = y;
    } else if (e->button() == Qt::RightButton) {
        removeNearest(x, y);
    }
}

void ThermalView::mouseMoveEvent(QMouseEvent* e) {
    int x, y;
    if (toImage(evPos(e), x, y)) {
        if (temp_) emit hovered(x, y, (*temp_)[size_t(y) * w_ + x]);
        if (dragging_) {
            dx1_ = x;
            dy1_ = y;
            update();
        }
    }
}

void ThermalView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !dragging_) return;
    dragging_ = false;
    Roi r;
    auto countKind = [&](Roi::Kind k) {
        return int(std::count_if(rois_.begin(), rois_.end(), [&](const Roi& q) { return q.kind == k; }));
    };
    if (std::abs(dx1_ - dx0_) < 3 && std::abs(dy1_ - dy0_) < 3) {
        if (countKind(Roi::Kind::Spot) >= kMaxSpots) return;
        r.kind = Roi::Kind::Spot;
        r.x0 = r.x1 = dx0_;
        r.y0 = r.y1 = dy0_;
    } else {
        if (countKind(Roi::Kind::Box) >= kMaxBoxes) return;
        r.kind = Roi::Kind::Box;
        r.x0 = std::min(dx0_, dx1_);
        r.y0 = std::min(dy0_, dy1_);
        r.x1 = std::max(dx0_, dx1_);
        r.y1 = std::max(dy0_, dy1_);
    }
    r.name = nextName(r.kind).toStdString();
    rois_.push_back(r);
    update();
    emit roisEdited(rois_);
}

void ThermalView::leaveEvent(QEvent*) { emit hovered(-1, -1, NAN); }

void ThermalView::removeNearest(int x, int y) {
    if (rois_.empty()) return;
    double best = 1e18;
    size_t idx = 0;
    for (size_t i = 0; i < rois_.size(); ++i) {
        const Roi& r = rois_[i];
        double cx = std::clamp(x, r.x0, r.x1), cy = std::clamp(y, r.y0, r.y1);
        double d = (cx - x) * (cx - x) + (cy - y) * (cy - y);
        if (d < best) {
            best = d;
            idx = i;
        }
    }
    rois_.erase(rois_.begin() + long(idx));
    update();
    emit roisEdited(rois_);
}

void ThermalView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(18, 18, 20));
    if (img_.isNull()) {
        p.setPen(QColor(160, 160, 160));
        p.drawText(rect(), Qt::AlignCenter, overlay_.isEmpty() ? "No camera connected" : overlay_);
        return;
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target(), img_);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f = p.font();
    f.setPointSizeF(9);
    f.setBold(true);
    p.setFont(f);
    QFontMetrics fm(f);

    auto label = [&](QPointF pt, const QString& text, const QColor& c) {
        int w = fm.horizontalAdvance(text) + 6;
        QRectF r(pt.x() + 6, pt.y() - fm.height() - 2, w, fm.height() + 2);
        p.fillRect(r, QColor(0, 0, 0, 150));
        p.setPen(c);
        p.drawText(r, Qt::AlignCenter, text);
    };
    auto cross = [&](QPointF pt, const QColor& c, double s) {
        p.setPen(QPen(QColor(0, 0, 0), 3));
        p.drawLine(QLineF(pt.x() - s, pt.y(), pt.x() + s, pt.y()));
        p.drawLine(QLineF(pt.x(), pt.y() - s, pt.x(), pt.y() + s));
        p.setPen(QPen(c, 1.5));
        p.drawLine(QLineF(pt.x() - s, pt.y(), pt.x() + s, pt.y()));
        p.drawLine(QLineF(pt.x(), pt.y() - s, pt.x(), pt.y() + s));
    };
    const QColor white(255, 255, 255), alarm(255, 60, 60);

    if (showExtremes_ && temp_ && !temp_->empty()) {
        auto mm = std::minmax_element(temp_->begin(), temp_->end());
        size_t imin = size_t(mm.first - temp_->begin()), imax = size_t(mm.second - temp_->begin());
        QColor hot(255, 70, 70), cold(90, 170, 255);
        QPointF ph = toWidget(double(imax % w_), double(imax / w_)), pc = toWidget(double(imin % w_), double(imin / w_));
        cross(ph, hot, 5);
        label(ph, QString::fromUtf8("▲ %1").arg(*mm.second, 0, 'f', 1), hot);
        cross(pc, cold, 5);
        label(pc, QString::fromUtf8("▼ %1").arg(*mm.first, 0, 'f', 1), cold);
    }

    for (size_t i = 0; i < rois_.size(); ++i) {
        const Roi& r = rois_[i];
        const a70::RoiResult* res = i < res_.size() && res_[i].name == r.name && res_[i].valid ? &res_[i] : nullptr;
        QColor c = res && res->alarm ? alarm : white;
        QString name = QString::fromStdString(r.name);
        if (r.kind == Roi::Kind::Spot) {
            QPointF pt = toWidget(r.x0, r.y0);
            cross(pt, c, 7);
            label(pt, res ? QString("%1 %2").arg(name).arg(res->maxC, 0, 'f', 1) : name, c);
        } else {
            QPointF a = toWidget(r.x0 - 0.5, r.y0 - 0.5), b = toWidget(r.x1 + 0.5, r.y1 + 0.5);
            p.setPen(QPen(QColor(0, 0, 0), 3));
            p.drawRect(QRectF(a, b));
            p.setPen(QPen(c, 1.5));
            p.drawRect(QRectF(a, b));
            QString t = res ? QString("%1 max %2  avg %3").arg(name).arg(res->maxC, 0, 'f', 1).arg(res->meanC, 0, 'f', 1)
                            : name;
            label(QPointF(a.x() - 6, a.y()), t, c);
        }
    }
    if (dragging_) {
        p.setPen(QPen(white, 1, Qt::DashLine));
        p.drawRect(QRectF(toWidget(dx0_, dy0_), toWidget(dx1_, dy1_)).normalized());
    }
    if (!overlay_.isEmpty()) {
        QRectF r(8, 8, fm.horizontalAdvance(overlay_) + 16, fm.height() + 8);
        p.fillRect(r, QColor(0, 0, 0, 170));
        p.setPen(overlayColor_);
        p.drawText(r, Qt::AlignCenter, overlay_);
    }
}

// ------------------------------------------------------------------ colour bar
ColorBar::ColorBar(QWidget* parent) : QWidget(parent) { setFixedWidth(66); }

void ColorBar::setRange(double lo, double hi, const QString& palette) {
    lo_ = lo;
    hi_ = hi;
    palette_ = palette;
    update();
}

void ColorBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(18, 18, 20));
    const auto* lut = &a70::paletteLut(palette_.toStdString());
    int top = 16, bot = height() - 14, h = std::max(bot - top, 1), x0 = 6, w = 16;
    for (int i = 0; i < h; ++i) {
        const auto c = (*lut)[size_t(255 * (1.0 - double(i) / h))];
        p.setPen(QColor(c[0], c[1], c[2]));
        p.drawLine(x0, top + i, x0 + w, top + i);
    }
    p.setPen(QColor(220, 220, 220));
    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);
    for (int k = 0; k <= 5; ++k) {
        double v = hi_ - (hi_ - lo_) * k / 5.0;
        double y = top + h * k / 5.0;
        p.drawLine(x0 + w, int(y), x0 + w + 4, int(y));
        p.drawText(QRectF(x0 + w + 6, y - 8, 42, 16), Qt::AlignVCenter | Qt::AlignLeft, QString::number(v, 'f', 1));
    }
    p.drawText(QRectF(0, 0, width(), 14), Qt::AlignCenter, QString::fromUtf8("°C"));
}
