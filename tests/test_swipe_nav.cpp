// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "ui/swipe_nav.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

namespace {

using Action = SwipeNavRecognizer::Action;

SwipeNavRecognizer::Wheel px(Qt::ScrollPhase phase, qreal dx, qreal dy, qint64 ms) {
    return {phase, QPointF(dx, dy), true, ms};
}
SwipeNavRecognizer::Wheel angle(qreal dx, qreal dy, qint64 ms) {
    return {Qt::NoScrollPhase, QPointF(dx, dy), false, ms};
}

// Feeds a phased (macOS/Wayland) swipe: begin, `steps` updates, end.
std::vector<Action> phasedSwipe(SwipeNavRecognizer &r, qreal dx, qreal dy, int steps, qint64 t0) {
    std::vector<Action> out;
    out.push_back(r.wheel(px(Qt::ScrollBegin, 0, 0, t0)));
    for (int i = 1; i <= steps; ++i)
        out.push_back(r.wheel(px(Qt::ScrollUpdate, dx, dy, t0 + i * 10)));
    out.push_back(r.wheel(px(Qt::ScrollEnd, 0, 0, t0 + (steps + 1) * 10)));
    return out;
}

int count(const std::vector<Action> &v, Action a) {
    return int(std::count(v.begin(), v.end(), a));
}

} // namespace

TEST_CASE("a phased swipe dragging content right goes back exactly once") {
    SwipeNavRecognizer r;
    const auto         acts = phasedSwipe(r, 20, 1, 20, 1000);
    CHECK(count(acts, Action::Back) == 1);
    CHECK(count(acts, Action::Forward) == 0);
    // Everything after the jump is swallowed, nothing before it is.
    const auto fired = std::find(acts.begin(), acts.end(), Action::Back);
    CHECK(std::all_of(acts.begin(), fired, [](Action a) { return a == Action::Pass; }));
    CHECK(std::all_of(fired + 1, acts.end() - 1, [](Action a) { return a == Action::Swallow; }));
    CHECK(acts.back() == Action::Pass); // ScrollEnd reaches the widgets
}

TEST_CASE("a phased swipe dragging content left goes forward") {
    SwipeNavRecognizer r;
    CHECK(count(phasedSwipe(r, -20, 0, 20, 1000), Action::Forward) == 1);
}

TEST_CASE("two consecutive swipes navigate twice") {
    SwipeNavRecognizer r;
    CHECK(count(phasedSwipe(r, 20, 0, 20, 1000), Action::Back) == 1);
    CHECK(count(phasedSwipe(r, 20, 0, 20, 1300), Action::Back) == 1);
}

TEST_CASE("a short horizontal nudge does not navigate") {
    SwipeNavRecognizer r;
    CHECK(count(phasedSwipe(r, 10, 0, 5, 1000), Action::Pass) == 7);
}

TEST_CASE("a vertical scroll with horizontal drift never navigates") {
    SwipeNavRecognizer  r;
    // Mostly vertical first, then a sideways drift within the same stream.
    std::vector<Action> acts;
    acts.push_back(r.wheel(px(Qt::ScrollBegin, 0, 0, 0)));
    for (int i = 1; i <= 10; ++i)
        acts.push_back(r.wheel(px(Qt::ScrollUpdate, 2, 30, i * 10)));
    for (int i = 11; i <= 30; ++i)
        acts.push_back(r.wheel(px(Qt::ScrollUpdate, 30, 0, i * 10)));
    CHECK(count(acts, Action::Pass) == int(acts.size()));
}

TEST_CASE("a diagonal swipe does not navigate") {
    SwipeNavRecognizer r;
    CHECK(count(phasedSwipe(r, 20, 15, 20, 1000), Action::Pass) == 22);
}

TEST_CASE("momentum after the fingers lift neither starts nor completes a swipe") {
    SwipeNavRecognizer r;
    r.wheel(px(Qt::ScrollBegin, 0, 0, 0));
    r.wheel(px(Qt::ScrollUpdate, 50, 0, 10));
    for (int i = 2; i < 20; ++i)
        CHECK(r.wheel(px(Qt::ScrollMomentum, 50, 0, i * 10)) == Action::Pass);
}

TEST_CASE("momentum after a jump is swallowed") {
    SwipeNavRecognizer r;
    r.wheel(px(Qt::ScrollBegin, 0, 0, 0));
    CHECK(r.wheel(px(Qt::ScrollUpdate, 200, 0, 10)) == Action::Back);
    CHECK(r.wheel(px(Qt::ScrollMomentum, 80, 0, 20)) == Action::Swallow);
    CHECK(r.wheel(px(Qt::ScrollEnd, 0, 0, 30)) == Action::Pass);
}

TEST_CASE("a swipe starting over horizontally scrollable content is left to it") {
    SwipeNavRecognizer r;
    auto               first = px(Qt::ScrollBegin, 0, 0, 0);
    first.blocked            = true;
    CHECK(r.wheel(first) == Action::Pass);
    for (int i = 1; i <= 20; ++i)
        CHECK(r.wheel(px(Qt::ScrollUpdate, 30, 0, i * 10)) == Action::Pass);
    // The next swipe elsewhere works again.
    CHECK(count(phasedSwipe(r, 20, 0, 20, 1000), Action::Back) == 1);
}

TEST_CASE("phase-less angle streams are segmented by an idle gap") {
    SwipeNavRecognizer  r;
    std::vector<Action> acts;
    for (int i = 0; i < 10; ++i)
        acts.push_back(r.wheel(angle(60, 0, 1000 + i * 15)));
    CHECK(count(acts, Action::Back) == 1);
    // Still the same stream: swallowed.
    CHECK(r.wheel(angle(60, 0, 1200)) == Action::Swallow);
    // After a pause, a new swipe the other way.
    acts.clear();
    for (int i = 0; i < 10; ++i)
        acts.push_back(r.wheel(angle(-60, 0, 2000 + i * 15)));
    CHECK(count(acts, Action::Forward) == 1);
}

TEST_CASE("a single tilt-wheel notch does not navigate") {
    SwipeNavRecognizer r;
    CHECK(r.wheel(angle(120, 0, 1000)) == Action::Pass);
    CHECK(r.wheel(angle(120, 0, 2000)) == Action::Pass);
}

TEST_CASE("vertical mouse-wheel notches never navigate") {
    SwipeNavRecognizer r;
    for (int i = 0; i < 20; ++i)
        CHECK(r.wheel(angle(0, -120, 1000 + i * 30)) == Action::Pass);
}

TEST_CASE("macOS three-finger swipe maps its angle to a direction") {
    CHECK(SwipeNavRecognizer::nativeSwipe(180) == Action::Back);
    CHECK(SwipeNavRecognizer::nativeSwipe(0) == Action::Forward);
    CHECK(SwipeNavRecognizer::nativeSwipe(90) == Action::Pass);
    CHECK(SwipeNavRecognizer::nativeSwipe(270) == Action::Pass);
}

TEST_CASE("a three-finger pan gesture navigates once") {
    SwipeNavRecognizer r;
    r.gestureBegin(3);
    std::vector<Action> acts;
    for (int i = 0; i < 20; ++i)
        acts.push_back(r.gesturePan(QPointF(-15, 1)));
    r.gestureEnd();
    CHECK(count(acts, Action::Forward) == 1);
    CHECK(r.gesturePan(QPointF(-15, 0)) == Action::Pass); // outside any gesture
}

TEST_CASE("pinches and two-finger gestures never navigate") {
    SwipeNavRecognizer r;
    r.gestureBegin(3);
    r.gesturePinch();
    for (int i = 0; i < 20; ++i)
        CHECK(r.gesturePan(QPointF(20, 0)) == Action::Pass);
    r.gestureEnd();
    r.gestureBegin(2);
    for (int i = 0; i < 20; ++i)
        CHECK(r.gesturePan(QPointF(20, 0)) == Action::Pass);
}
