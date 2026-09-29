// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Tests for SwipeIndicator — the arrow badge a trackpad back/forward swipe
// flashes over the chat: centred on the given area, on top, input-transparent,
// gone once the animation has run.
#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include <QApplication>
#include <QImage>
#include <QTest>

#include <cstdlib>

#include "ui/swipe_indicator/swipe_indicator.h"
#include "ui/theme_manager.h"

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-swipe-indicator");
    app.setOrganizationName("msga-test");
    ThemeManager::instance();
    return msga_test::runCatch(argc, argv);
}

TEST_CASE("swipe indicator flashes centred on the area, then hides") {
    QWidget host;
    host.resize(800, 600);
    host.show();
    auto *sibling = new QWidget(&host);
    sibling->setGeometry(host.rect());
    sibling->show();

    SwipeIndicator ind(&host);
    CHECK_FALSE(ind.isVisible());
    CHECK(ind.testAttribute(Qt::WA_TransparentForMouseEvents));

    const QRect area(200, 100, 600, 400);
    ind.flash(true, area);
    CHECK(ind.isVisible());
    CHECK(ind.geometry().center() == area.center());
    CHECK(host.children().last() == &ind); // raised above the page

    QTRY_VERIFY_WITH_TIMEOUT(!ind.isVisible(), SwipeIndicator::kDurationMs * 3);
}

TEST_CASE("swipe indicator paints the arrow on the side it points to") {
    QWidget host;
    host.resize(300, 300);
    SwipeIndicator ind(&host);

    // Mid-run, sample the row through the chevron's upper arm: it sits only on
    // the side the arrow points to (the shaft is symmetric, the head is not).
    // Returns which half differs more from the plain badge disc.
    const auto headSide = [&](bool back) {
        ind.flash(back, host.rect());
        QTest::qWait(SwipeIndicator::kDurationMs / 3);
        QImage img(ind.size(), QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        ind.render(&img);
        const int  r = SwipeIndicator::kDiameter / 2, cx = img.width() / 2, cy = img.height() / 2;
        const int  y     = cy - r / 4;
        const QRgb badge = img.pixel(cx, cy - r * 2 / 3);
        long       left = 0, right = 0;
        for (int dx = -r * 3 / 4; dx <= r * 3 / 4; ++dx) {
            const QRgb px = img.pixel(cx + dx, y);
            const long d = std::abs(qRed(px) - qRed(badge)) + std::abs(qGreen(px) - qGreen(badge)) +
                           std::abs(qBlue(px) - qBlue(badge));
            (dx < 0 ? left : right) += d;
        }
        return right - left;
    };
    CHECK(headSide(true) < 0);  // back: points left
    CHECK(headSide(false) > 0); // forward: points right
}
