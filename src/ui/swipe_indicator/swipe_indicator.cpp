// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "swipe_indicator.h"
#include "ui/theme.h"

#include <QEasingCurve>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

#include <algorithm>

namespace {

// Room around the badge for its drift, so it never clips at the widget edge.
constexpr int   kDrift     = 10;
constexpr int   kMargin    = kDrift + 2;
constexpr qreal kFadeIn    = 0.2; // share of the run spent fading in
constexpr qreal kFadeOut   = 0.5; // share of the run spent fading out
constexpr qreal kPeakAlpha = 0.85;

} // namespace

SwipeIndicator::SwipeIndicator(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    resize(kDiameter + 2 * kMargin, kDiameter + 2 * kMargin);
    hide();

    _anim = new QVariantAnimation(this);
    _anim->setStartValue(0.0);
    _anim->setEndValue(1.0);
    _anim->setDuration(kDurationMs);
    connect(_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        _t = v.toReal();
        update();
    });
    connect(_anim, &QVariantAnimation::finished, this, &QWidget::hide);
}

void SwipeIndicator::flash(bool back, const QRect &area) {
    _back = back;
    _t    = 0;
    move(area.center() - rect().center());
    raise();
    show();
    _anim->stop();
    _anim->start();
}

void SwipeIndicator::paintEvent(QPaintEvent *) {
    // Opacity: ease in, hold, ease out. The badge drifts toward the direction
    // it points and grows slightly as it appears.
    const qreal in = std::min(1.0, _t / kFadeIn);
    const qreal out =
        _t <= 1 - kFadeOut
            ? 1.0
            : QEasingCurve(QEasingCurve::InQuad).valueForProgress((1 - _t) / kFadeOut);
    const qreal opacity = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(in) * out;
    if (opacity <= 0)
        return;
    const qreal drift = (QEasingCurve(QEasingCurve::OutCubic).valueForProgress(_t) - 0.5) * kDrift;
    const qreal scale = 0.85 + 0.15 * QEasingCurve(QEasingCurve::OutBack).valueForProgress(in);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(opacity);
    p.translate(QRectF(rect()).center() + QPointF(_back ? -drift : drift, 0));
    p.scale(scale, scale);

    // Inverted against the content surface, so it reads on light and dark
    // themes alike.
    const auto &th = Th::c();
    QColor      bg = th.text.primary;
    bg.setAlphaF(kPeakAlpha);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    const qreal r = kDiameter / 2.0;
    p.drawEllipse(QPointF(0, 0), r, r);

    // Arrow: a shaft with a chevron head, pointing left for back.
    const qreal  dir = _back ? -1 : 1;
    const qreal  len = r * 0.52, head = r * 0.3;
    QPainterPath arrow;
    arrow.moveTo(-dir * len, 0);
    arrow.lineTo(dir * len, 0);
    arrow.moveTo(dir * (len - head), -head);
    arrow.lineTo(dir * len, 0);
    arrow.lineTo(dir * (len - head), head);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(th.surface.content, r * 0.14, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(arrow);
}
