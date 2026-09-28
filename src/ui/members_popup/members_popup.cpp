// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "members_popup.h"
#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/popup_placement.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#include <QStackedLayout>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

MembersPopup::MembersPopup(ImageCache *imgCache, QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName("membersPopup");

    auto       *lay = new QVBoxLayout(this);
    const auto &sp  = Th::c().spacing;
    lay->setContentsMargins(sp.md, sp.lg, sp.md, sp.md);
    lay->setSpacing(sp.md);

    _title = new QLabel(this);
    _title->setObjectName("membersTitle");
    _title->setContentsMargins(sp.sm, 0, sp.sm, 0);
    lay->addWidget(_title);

    _search = new StyledLineEdit(this);
    _search->setObjectName("membersSearch");
    _search->setPlaceholderText(tr("Find members"));
    _search->setLeadingIcon(QStringLiteral(":/ui/search.svg"));
    _search->lineEdit()->installEventFilter(this);
    lay->addWidget(_search);

    // The list and the message share the body: exactly one is ever shown.
    _body = new QStackedLayout;
    _body->setStackingMode(QStackedLayout::StackOne);
    _list = new BrowseListView(imgCache, this);
    _list->setObjectName("membersList");
    // The popup's own margin already insets the rows; the list's default
    // padding (sized for full dialogs) would push the avatars far in.
    _list->setRowPadding(sp.md);
    _body->addWidget(_list);
    _message = new QLabel(this);
    _message->setObjectName("membersMessage");
    _message->setAlignment(Qt::AlignCenter);
    _message->setWordWrap(true);
    _body->addWidget(_message);
    lay->addLayout(_body, 1);

    _list->onActivated = [this](const QString &id) {
        hide();
        emit memberActivated(UserId{id});
    };
    connect(_search, &StyledLineEdit::textChanged, this, [this](const QString &) {
        applyFilter();
    });

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void MembersPopup::applyTheme() {
    const auto &th = Th::c();
    setStyleSheet(
        u"QFrame#membersPopup {"
        "  background: %1;"
        "  border: 1px solid %2;"
        "  border-radius: 8px;"
        "}"_s.arg(Th::qss(th.surface.raised), Th::qss(th.divider.strong))
    );
    _title->setStyleSheet(
        u"QLabel { color: %1; font-size: %2px; font-weight: 600; background: transparent; }"_s
            .arg(Th::qss(th.text.secondary))
            .arg(th.fonts.sm)
    );
    _message->setStyleSheet(u"QLabel { color: %1; font-size: %2px; background: transparent; }"_s
                                .arg(Th::qss(th.text.secondary))
                                .arg(th.fonts.md));
}

void MembersPopup::open(const QRect &anchorGlobal, int expectedCount) {
    _error.clear();
    _loading = true;
    _list->setItems({});
    _search->clear();
    _title->setText(tr("Members"));
    applyFilter();

    // The list gets exactly `rows` whole rows and the layout sizes the rest:
    // summing the title's and search's size hints by hand came out short (the
    // stylesheets aren't polished yet), cutting the last row off.
    const int rows = std::clamp(expectedCount, 1, kMaxRows);
    _list->setFixedHeight(rows * BrowseListView::rowHeight());
    ensurePolished();
    layout()->activate();
    setFixedSize(kWidth, layout()->sizeHint().height());

    // Within the app window as well as the screen: a header button sits at the
    // window's edge, and a panel hanging past it reads as detached.
    QRect bounds;
    if (QScreen *scr = QGuiApplication::screenAt(anchorGlobal.center()))
        bounds = scr->availableGeometry();
    if (const QWidget *host = parentWidget() ? parentWidget()->window() : nullptr)
        bounds = bounds.isValid() ? bounds.intersected(host->geometry()) : host->geometry();
    move(Ui::placePopup(anchorGlobal, size(), bounds, Ui::Edge::Below, Th::c().spacing.sm));
    show();
    raise();
    _search->lineEdit()->setFocus();
}

void MembersPopup::setMembers(const std::vector<User> &members, const UserId &me) {
    std::vector<const User *> people;
    people.reserve(members.size());
    for (const auto &u : members)
        if (!u.isDeactivated)
            people.push_back(&u);
    std::sort(people.begin(), people.end(), [](const User *a, const User *b) {
        return QString::localeAwareCompare(a->displayLabel(), b->displayLabel()) < 0;
    });

    std::vector<BrowseListView::Item> items;
    items.reserve(people.size());
    for (const User *u : people) {
        const QString &label = u->displayLabel();

        BrowseListView::Item it;
        it.id        = u->id.value;
        it.title     = u->id == me ? tr("%1 (you)").arg(label) : label;
        it.avatarUrl = u->avatarUrl;
        it.initial   = label.left(1);
        it.isPerson  = true;
        QStringList subtitle;
        if (!u->name.isEmpty() && u->name != label)
            subtitle << '@' + u->name;
        if (!u->title.isEmpty())
            subtitle << u->title;
        it.subtitle  = subtitle.join(QStringLiteral(" · "));
        it.searchKey = (label + ' ' + u->name + ' ' + u->title).toLower();
        items.push_back(std::move(it));
    }

    const int n = int(items.size());
    _title->setText(tr("%Ln member(s)", "", n));
    _error.clear();
    _loading = false;
    _list->setItems(std::move(items));
    applyFilter();
}

void MembersPopup::showError(const QString &message) {
    _error   = message;
    _loading = false;
    applyFilter();
}

int MembersPopup::visibleCount() const {
    return _body->currentWidget() == _list ? _list->visibleCount() : 0;
}

void MembersPopup::showMessage(const QString &message) {
    _message->setText(message);
    _body->setCurrentWidget(_message);
}

void MembersPopup::applyFilter() {
    if (!_error.isEmpty()) {
        showMessage(_error);
        return;
    }
    if (_loading) {
        showMessage(tr("Loading members…"));
        return;
    }
    const QString query = _search->text().trimmed();
    _list->applyFilter(query);
    // Typing picks the first match for Enter; with no query nothing is picked.
    _list->setSelectedRow(query.isEmpty() ? -1 : 0);
    if (_list->visibleCount() == 0) {
        showMessage(
            query.isEmpty() ? tr("No members to show.") : tr("No one here matches “%1”.").arg(query)
        );
        return;
    }
    _body->setCurrentWidget(_list);
}

bool MembersPopup::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _search->lineEdit() && event->type() == QEvent::KeyPress) {
        switch (static_cast<QKeyEvent *>(event)->key()) {
        case Qt::Key_Escape:
            hide();
            return true;
        case Qt::Key_Down:
            _list->moveSelection(1);
            return true;
        case Qt::Key_Up:
            _list->moveSelection(-1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            _list->activateSelected();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(obj, event);
}
