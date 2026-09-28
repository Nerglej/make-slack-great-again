// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "voice_recording_strip.h"

#include "ui/control_metrics.h"
#include "ui/icon_utils.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QToolButton>
#include <QtMath>

using namespace Qt::StringLiterals;

namespace {

constexpr int   kDot       = 8;  // recording dot diameter
constexpr int   kMeterH    = 16; // tallest meter bar
constexpr int   kSpinner   = 14; // processing spinner diameter
constexpr int   kCancelBtn = 20;
constexpr QSize kCancelIcon{12, 12};
// LoadingIndicator paints a fixed 52 px ring; the strip scales it down.
constexpr qreal kLoaderNative = 52.0;

} // namespace

VoiceRecordingStrip::VoiceRecordingStrip(QWidget *parent) : QWidget(parent) {
    setObjectName("voiceStrip");
    setFixedHeight(Ui::kControlHeightSmall);

    auto       *lay = new QHBoxLayout(this);
    const auto &sp  = Th::c().spacing;
    lay->setContentsMargins(sp.md, 0, sp.sm, 0);
    lay->addStretch();
    _cancelBtn = new QToolButton(this);
    _cancelBtn->setFixedSize(kCancelBtn, kCancelBtn);
    _cancelBtn->setIconSize(kCancelIcon);
    _cancelBtn->setFocusPolicy(Qt::NoFocus);
    _cancelBtn->setCursor(Qt::PointingHandCursor);
    _cancelBtn->setToolTip(tr("Cancel voice input (Esc)"));
    connect(_cancelBtn, &QToolButton::clicked, this, &VoiceRecordingStrip::cancelClicked);
    lay->addWidget(_cancelBtn);

    _tick.setInterval(50);
    connect(&_tick, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    _errorHide.setSingleShot(true);
    connect(&_errorHide, &QTimer::timeout, this, [this] {
        if (_mode == Mode::Error)
            setMode(Mode::Hidden);
    });
    _spinner.setUpdateCallback([this] { update(); });

    hide();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void VoiceRecordingStrip::applyTheme() {
    Th::setStyleSheetIfChanged(
        _cancelBtn,
        u"QToolButton { border: none; border-radius: 3px; background: transparent; }"
        "QToolButton:hover { background: %1; }"_s.arg(Th::qss(Th::c().divider.def))
    );
    _cancelBtn->setIcon(svgIcon(u":/ui/x.svg"_s, kCancelIcon, Th::c().text.secondary));
    update();
}

void VoiceRecordingStrip::setMode(Mode mode, const QString &errorText) {
    const Mode was = _mode;
    _mode          = mode;
    _error         = mode == Mode::Error ? errorText : QString();

    if (mode == Mode::Recording && was != Mode::Recording) {
        _elapsed.start();
        _levels.fill(0.f);
        _head = 0;
    }
    if (mode == Mode::Recording)
        _tick.start();
    else
        _tick.stop();

    if (mode == Mode::Transcribing || mode == Mode::Cleaning) {
        if (!_spinner.isRunning())
            _spinner.start();
    } else if (_spinner.isRunning()) {
        _spinner.stop();
    }

    if (mode == Mode::Error)
        _errorHide.start(kErrorMs);
    else
        _errorHide.stop();

    _cancelBtn->setToolTip(mode == Mode::Error ? tr("Dismiss") : tr("Cancel voice input (Esc)"));
    setVisible(mode != Mode::Hidden);
    update();
}

void VoiceRecordingStrip::pushLevel(float peak) {
    if (_mode != Mode::Recording)
        return;
    _levels[_head] = qBound(0.f, peak, 1.f);
    _head          = (_head + 1) % kBars;
    // _tick repaints at a steady rate; no per-chunk update() needed.
}

QString VoiceRecordingStrip::elapsedText() const {
    const qint64 secs = _elapsed.isValid() ? _elapsed.elapsed() / 1000 : 0;
    return QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
}

void VoiceRecordingStrip::paintEvent(QPaintEvent *) {
    if (_mode == Mode::Hidden)
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const auto &th    = Th::c();
    const auto &sp    = th.spacing;
    const int   cy    = height() / 2;
    const int   right = _cancelBtn->geometry().left() - sp.sm;
    int         x     = sp.md;

    QFont f = font();
    f.setPixelSize(th.fonts.caption);
    p.setFont(f);
    const QFontMetrics fm(f);

    const auto drawText = [&](const QString &text, const QColor &color, bool elide = false) {
        if (x >= right)
            return;
        const QString shown = elide ? fm.elidedText(text, Qt::ElideRight, right - x) : text;
        const int     w     = fm.horizontalAdvance(shown);
        if (!elide && x + w > right)
            return; // optional pieces simply drop out on a narrow box
        p.setPen(color);
        p.drawText(QRect(x, 0, w + 1, height()), Qt::AlignVCenter | Qt::AlignLeft, shown);
        x += w + sp.md;
    };

    switch (_mode) {
    case Mode::Recording: {
        // Pulsing dot: ~1.2 s breathing cycle.
        const qreal phase = (_elapsed.elapsed() % 1200) / 1200.0;
        QColor      dot   = th.danger.def;
        dot.setAlphaF(0.45 + 0.55 * (0.5 + 0.5 * qCos(phase * 2 * M_PI)));
        p.setPen(Qt::NoPen);
        p.setBrush(dot);
        p.drawEllipse(QRectF(x, cy - kDot / 2.0, kDot, kDot));
        x += kDot + sp.sm;

        // Level meter: oldest bar on the left, newest on the right.
        // Neutral ink: the accent can sit close to the box colour (dark themes).
        p.setBrush(th.text.secondary);
        for (int i = 0; i < kBars; ++i) {
            const float level = _levels[(_head + i) % kBars];
            // sqrt lifts quiet speech into view; min height keeps a baseline.
            const qreal h     = qMax<qreal>(2.0, kMeterH * qSqrt(level));
            p.drawRoundedRect(QRectF(x, cy - h / 2.0, kBarW, h), kBarW / 2.0, kBarW / 2.0);
            x += kBarW + kBarGap;
        }
        x += sp.md - kBarGap;

        drawText(elapsedText(), th.text.primary);
        //: Hint in the composer's voice-recording strip
        drawText(tr("Esc to cancel"), th.text.tertiary);
        break;
    }
    case Mode::Transcribing:
    case Mode::Cleaning: {
        p.save();
        const qreal s = kSpinner / kLoaderNative;
        p.translate(x + kSpinner / 2.0, cy);
        p.scale(s, s);
        _spinner.paint(p, QRect(-26, -26, 52, 52));
        p.restore();
        x += kSpinner + sp.sm;
        drawText(
            _mode == Mode::Transcribing ? tr("Transcribing…") : tr("Cleaning up…"),
            th.text.secondary
        );
        break;
    }
    case Mode::Error:
        drawText(_error, th.text.danger, true);
        break;
    case Mode::Hidden:
        break;
    }
}
