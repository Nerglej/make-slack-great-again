// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QPointF>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

// Turns a trackpad's horizontal swipe into one back/forward step, the way a
// browser does. Two-finger swipes reach Qt as horizontal wheel events on every
// platform — with scroll phases on macOS/Wayland, as a bare delta stream on
// Windows/X11 (segmented here by an idle gap). Three-or-more-finger swipes
// arrive as native gestures: a single SwipeNativeGesture on macOS, a
// Begin/Pan…/End sequence on X11 and Wayland.
//
// The direction follows the content, as in a browser: a swipe that drags the
// view to the right goes back. Each swipe navigates at most once; the rest of
// its stream is swallowed so the newly opened conversation doesn't inherit it.
class SwipeNavRecognizer {
public:
    enum class Action { Pass, Swallow, Back, Forward };

    // Horizontal travel that makes a swipe, in pixels; angle-only streams
    // (Windows, X11, tilt wheels) are in 120-per-notch units.
    static constexpr qreal  kPixelThreshold = 120;
    static constexpr qreal  kAngleThreshold = 360;
    // No-phase streams: a gap this long starts a new swipe.
    static constexpr qint64 kIdleGapMs      = 300;

    struct Wheel {
        Qt::ScrollPhase phase;
        QPointF         delta;  // Qt sign convention: positive x = content moves right
        bool            pixels; // delta is pixelDelta (else angleDelta)
        qint64          ms;
        // The pointer is over content that scrolls horizontally itself (or a
        // modifier turns the wheel into something else): leave the swipe to it.
        bool            blocked = false;
    };

    Action wheel(const Wheel &w) {
        const bool newSwipe = w.phase == Qt::ScrollBegin ||
                              (w.phase == Qt::NoScrollPhase && w.ms - _lastMs > kIdleGapMs);
        if (newSwipe)
            _wheel = {};
        _lastMs = w.ms;
        // Where the swipe starts decides whether it is ours.
        if (w.blocked && _wheel.state == State::Tracking && _wheel.acc.isNull())
            _wheel.state = State::Ignored;
        Action act = Action::Pass;
        // Momentum is the flick coasting after the fingers lifted — not part
        // of the swipe the user made (but still swallowed after a jump).
        if (w.phase != Qt::ScrollMomentum || _wheel.state == State::Fired)
            act = _wheel.feed(w.delta, w.pixels ? kPixelThreshold : kAngleThreshold);
        if (w.phase == Qt::ScrollEnd) {
            _wheel = {};
            // Carries no delta; let widgets that saw the begin see the end too.
            if (act == Action::Swallow)
                act = Action::Pass;
        }
        return act;
    }

    // macOS three-finger swipe (NSEvent swipeWithEvent). Qt reports the
    // direction as an angle: 180° for deltaX = +1, which AppKit treats as
    // "back" (WebKit's own swipe handling), 0° for deltaX = -1 (forward).
    static Action nativeSwipe(qreal angle) {
        if (qFuzzyCompare(angle, 180))
            return Action::Back;
        if (qFuzzyIsNull(angle))
            return Action::Forward;
        return Action::Pass;
    }

    // X11/Wayland multi-finger gesture sequence. `delta` is finger motion
    // (positive x = fingers moved right = back). A pinch sends Pan too (its
    // centroid drift) — once a zoom/rotate shows up the sequence is ignored.
    void gestureBegin(int fingers) {
        _gesture       = {};
        _gesture.state = fingers >= 3 ? State::Tracking : State::Ignored;
    }
    void gesturePinch() {
        if (_gesture.state == State::Tracking)
            _gesture.state = State::Ignored;
    }
    void   gestureEnd() { _gesture = {State::Ignored, {}}; }
    Action gesturePan(const QPointF &delta) { return _gesture.feed(delta, kPixelThreshold); }

private:
    enum class State { Tracking, Fired, Ignored };

    struct Stream {
        State   state = State::Tracking;
        QPointF acc;

        // Horizontal and clearly dominant fires; clearly vertical first (a
        // normal message-list scroll) locks the stream out.
        Action feed(const QPointF &delta, qreal threshold) {
            if (state == State::Fired)
                return Action::Swallow;
            if (state == State::Ignored)
                return Action::Pass;
            acc += delta;
            const qreal ax = std::abs(acc.x()), ay = std::abs(acc.y());
            if (ay > threshold / 3 && ay >= ax) {
                state = State::Ignored;
                return Action::Pass;
            }
            if (ax >= threshold && ax > 2 * ay) {
                state = State::Fired;
                return acc.x() > 0 ? Action::Back : Action::Forward;
            }
            return Action::Pass;
        }
    };

    Stream _wheel;
    qint64 _lastMs  = 0;
    Stream _gesture = {State::Ignored, {}};
};
