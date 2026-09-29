// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QWidget>

class QVariantAnimation;

// Feedback for a trackpad back/forward swipe, like a browser's: a round badge
// with an arrow that fades in over the given area, drifts a little in the
// swipe's direction and fades out. Purely decorative — transparent for input.
class SwipeIndicator : public QWidget {
public:
    explicit SwipeIndicator(QWidget *parent);

    // Plays the flash centred on `area` (in parent coordinates); a flash still
    // running restarts in the new direction.
    void flash(bool back, const QRect &area);

    static constexpr int kDiameter   = 64;
    static constexpr int kDurationMs = 650;

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QVariantAnimation *_anim = nullptr;
    bool               _back = true;
    qreal              _t    = 0; // animation progress, 0 → 1
};
