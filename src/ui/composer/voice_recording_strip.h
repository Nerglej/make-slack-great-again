// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/loading_indicator/loading_indicator.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <array>

class QToolButton;

// The composer's voice-input status row, shown between the editor and the
// bottom action bar while a dictation started from that composer is in flight:
//   Recording     — pulsing red dot, a scrolling level meter fed by
//                   VoiceInput::level(), the elapsed time (m:ss) and "Esc to
//                   cancel"
//   Transcribing / Cleaning — a small spinner and "Transcribing…" /
//                   "Cleaning up…"
//   Error         — the failure text in the danger colour; dismisses itself
// plus a cancel (×) button on the right in every mode. Painted by hand (no
// stylesheet cascade) so it stays crisp at fractional DPR and follows theme
// switches with a repaint. Hidden in Hidden mode.
class VoiceRecordingStrip : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Hidden, Recording, Transcribing, Cleaning, Error };

    explicit VoiceRecordingStrip(QWidget *parent = nullptr);

    // Entering Recording (from any other mode) restarts the elapsed timer and
    // clears the meter. Error shows `errorText` for kErrorMs.
    void setMode(Mode mode, const QString &errorText = {});
    Mode mode() const { return _mode; }
    // Peak 0..1 of the latest chunk; ignored outside Recording.
    void pushLevel(float peak);

    // "m:ss" for the elapsed recording time (exposed for tests).
    QString elapsedText() const;

    static constexpr int kErrorMs = 6000;

signals:
    // The × button: cancel the dictation (or dismiss the error).
    void cancelClicked();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    void applyTheme();

    static constexpr int kBars   = 28; // meter history, newest on the right
    static constexpr int kBarW   = 2;
    static constexpr int kBarGap = 2;

    Mode                     _mode = Mode::Hidden;
    QString                  _error;
    QToolButton             *_cancelBtn = nullptr;
    QElapsedTimer            _elapsed;
    QTimer                   _tick;      // pulse + clock repaint while recording
    QTimer                   _errorHide; // Error → Hidden
    std::array<float, kBars> _levels{};
    int                      _head = 0; // next slot in the _levels ring
    LoadingIndicator         _spinner;
};
