// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "canvas_viewer.h"
#include "canvas_page.h"
#include "ui/icon_button/icon_button.h"
#include "ui/popup_tooltip/popup_tooltip.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QUrl>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

namespace {
constexpr int kPanelRadius = 8;
} // namespace

CanvasViewerOverlay::CanvasViewerOverlay(QWidget *windowParent) : QWidget(windowParent) {
    setFocusPolicy(Qt::StrongFocus);
    hide();

    _panel = new QWidget(this);
    _panel->setObjectName("canvasViewerPanel");
    _panel->setAttribute(Qt::WA_StyledBackground);
    auto *col = new QVBoxLayout(_panel);
    // Bottom margin keeps the page's square corners inside the rounded panel.
    col->setContentsMargins(0, 0, 0, kPanelRadius);
    col->setSpacing(0);

    const auto &sp  = Th::c().spacing;
    auto       *bar = new QHBoxLayout;
    bar->setContentsMargins(sp.lg, sp.sm, sp.sm, 0);
    bar->setSpacing(sp.xs);
    _heading = new QLabel(tr("Canvas"), _panel);
    bar->addWidget(_heading, 1);
    _openBtn = new IconButton(u":/ui/external-link.svg"_s, 32, 16, _panel);
    _openBtn->setCursor(Qt::PointingHandCursor);
    bar->addWidget(_openBtn);
    _closeBtn = new IconButton(u":/ui/x.svg"_s, 32, 16, _panel);
    _closeBtn->setCursor(Qt::PointingHandCursor);
    bar->addWidget(_closeBtn);
    col->addLayout(bar);

    _page = new CanvasPage(_panel);
    col->addWidget(_page, 1);

    _tip = new PopupTooltip(this);
    _openBtn->installEventFilter(this);
    _closeBtn->installEventFilter(this);

    connect(_openBtn, &QPushButton::clicked, this, [this] {
        _tip->hide();
        if (!_permalink.isEmpty())
            QDesktopServices::openUrl(QUrl(_permalink));
    });
    connect(_closeBtn, &QPushButton::clicked, this, &CanvasViewerOverlay::dismiss);
    // Deleted from its own ⋮ menu: nothing left to show.
    connect(_page, &CanvasPage::canvasDeleted, this, [this] { hide(); });

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        applyTheme();
        update();
    });

    if (windowParent)
        windowParent->installEventFilter(this); // track window resizes while open
}

void CanvasViewerOverlay::open(Session *session, const ConversationId &conv, const File &canvas) {
    _permalink = canvas.permalink;
    _openBtn->setVisible(!_permalink.isEmpty());
    _page->setSession(session);
    _page->open(conv, canvas.id, canvas.title);
    relayout();
    show();
    raise();
    setFocus();
}

void CanvasViewerOverlay::dismiss() {
    _tip->hide();
    _page->flushPendingSave();
    hide();
}

void CanvasViewerOverlay::applyTheme() {
    const auto &th = Th::c();
    _panel->setStyleSheet(u"QWidget#canvasViewerPanel { background: %1; border-radius: %2px; }"_s
                              .arg(Th::qss(th.surface.content))
                              .arg(kPanelRadius));
    _heading->setStyleSheet(u"color: %1; font-size: %2px; font-weight: bold;"_s
                                .arg(Th::qss(th.text.secondary))
                                .arg(th.fonts.caption));
}

QRect CanvasViewerOverlay::panelRect() const {
    const int w = std::min(kMaxPanelW, std::max(200, width() - 2 * kMargin));
    const int h = std::max(200, height() - 2 * kMargin);
    return {(width() - w) / 2, (height() - h) / 2, w, h};
}

void CanvasViewerOverlay::relayout() {
    if (parentWidget())
        setGeometry(parentWidget()->rect());
    _panel->setGeometry(panelRect());
}

void CanvasViewerOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Th::c().surface.viewerBackdrop);
}

void CanvasViewerOverlay::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) {
        dismiss();
        return;
    }
    QWidget::keyPressEvent(e);
}

void CanvasViewerOverlay::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton && !_panel->geometry().contains(e->pos())) {
        dismiss();
        return;
    }
    QWidget::mousePressEvent(e);
}

bool CanvasViewerOverlay::eventFilter(QObject *obj, QEvent *ev) {
    if (obj == parentWidget() && ev->type() == QEvent::Resize && isVisible())
        relayout();
    if (obj == _openBtn || obj == _closeBtn) {
        auto *btn = static_cast<QWidget *>(obj);
        if (ev->type() == QEvent::Enter)
            _tip->showAbove(
                obj == _openBtn ? tr("Open in browser") : tr("Close"),
                QRect(btn->mapToGlobal(QPoint(0, 0)), btn->size())
            );
        else if (ev->type() == QEvent::Leave || ev->type() == QEvent::MouseButtonPress)
            _tip->hide();
    }
    return QWidget::eventFilter(obj, ev);
}
