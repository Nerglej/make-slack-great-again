// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "quick_switcher_dialog.h"
#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/quick_switcher/workspace_tab_strip.h"
#include "ui/shortcuts.h"
#include "ui/styled_line_edit/styled_line_edit.h"
#include "ui/theme.h"
#include "util/fuzzy_match.h"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

using namespace Qt::StringLiterals;

static constexpr int kCardPadH = 24;
static constexpr int kCardPadT = 20;
static constexpr int kCardPadB = 16;

// Group DMs are named after their members, so "John Doe" matches every group
// John is in exactly as well as his 1:1 DM — and the groups, often more recent,
// buried him (issue #61). Half a consecutive-match weight: enough to lose any
// tie or boundary-bonus difference (those are 0.1 apart) to a 1:1 DM or channel,
// not enough to sink a group whose name matches a whole character better.
static constexpr double kGroupDmBias = -0.5;

static double rankBiasFor(const NamedConversation &conv) {
    return conv.kind == ConvKind::Mpim ? kGroupDmBias : 0.0;
}

QuickSwitcherDialog::QuickSwitcherDialog(
    std::vector<NamedConversation> conversations, ImageCache *imgCache, QWidget *parent
)
    : QuickSwitcherDialog(
          std::vector<Workspace>{Workspace{.conversations = std::move(conversations)}},
          {},
          imgCache,
          parent
      ) {}

QuickSwitcherDialog::QuickSwitcherDialog(
    std::vector<Workspace> workspaces,
    const QString         &activeTeamId,
    ImageCache            *imgCache,
    QWidget               *parent
)
    : AppDialog(parent, Chrome::Custom, Scroll::Disabled), _workspaces(std::move(workspaces)),
      _imgCache(imgCache) {
    if (_workspaces.empty())
        _workspaces.emplace_back(); // an empty list is still a valid dialog

    // Card, backdrop, centring, Escape and the Cmd+W hand-off all come from
    // AppDialog; Chrome::Custom means no title header — the field is the header,
    // Spotlight-style.
    auto       *lay = contentLayout();
    const auto &sp  = Th::c().spacing;

    const bool multiTeam = _workspaces.size() > 1;

    // Workspace tabs first — the header of the card when there is a choice to
    // make; hidden (and the field takes the top padding) when there isn't.
    _tabs = new WorkspaceTabStrip(_imgCache, card());
    _tabs->setObjectName("quickSwitcherTabs");
    {
        std::vector<WorkspaceTabStrip::Entry> entries;
        entries.reserve(_workspaces.size());
        for (const auto &ws : _workspaces)
            entries.push_back({.teamId = ws.teamId, .name = ws.name, .iconUrl = ws.iconUrl});
        _tabs->setWorkspaces(std::move(entries));
    }
    auto *tabRow = new QVBoxLayout;
    tabRow->setContentsMargins(kCardPadH, kCardPadT, kCardPadH, sp.lg);
    tabRow->addWidget(_tabs);
    lay->addLayout(tabRow);
    _tabs->setVisible(multiTeam);

    _searchEdit = new StyledLineEdit(card());
    _searchEdit->setPlaceholderText(tr("Jump to a conversation…"));
    _searchEdit->setLeadingIcon(u":/ui/search.svg"_s);
    _searchEdit->lineEdit()->installEventFilter(this);

    auto *fieldRow = new QVBoxLayout;
    fieldRow->setContentsMargins(kCardPadH, multiTeam ? 0 : kCardPadT, kCardPadH, sp.lg);
    fieldRow->addWidget(_searchEdit);
    lay->addLayout(fieldRow);

    _list = new BrowseListView(_imgCache, card());
    _list->setObjectName("quickSwitcherList");
    // Fuzzy, not substring: "xdg" must land on #xd-general (issue #60). The
    // keys are bare names, so scattered matches stay meaningful, and the list
    // reorders best-first so the preselected top row is the likeliest target.
    _list->setMatchMode(BrowseListView::Match::Fuzzy);
    _list->setMinimumHeight(kListMinH);
    _list->onActivated = [this](const QString &id) {
        const QString teamId =
            _shownWorkspace >= 0 ? _workspaces[_shownWorkspace].teamId : QString();
        accept();
        emit conversationActivated(teamId, ConversationId{id});
    };
    lay->addWidget(_list, 1);

    // Shown in the list's place when nothing matches — the empty virtual list
    // would otherwise just be a blank rectangle.
    _empty = new QLabel(tr("No conversations match."), card());
    _empty->setAlignment(Qt::AlignCenter);
    _empty->setMinimumHeight(kListMinH);
    _empty->hide();
    lay->addWidget(_empty, 1);

    // Arrow keys aren't discoverable on a field that looks like plain search.
    _hint = new QLabel(card());
    _hint->setAlignment(Qt::AlignCenter);
    const QString upDown    = QString(QChar(0x2191)) + QChar(0x2193);
    const QString leftRight = QString(QChar(0x2190)) + QChar(0x2192);
    const QString enter     = Ui::Shortcuts::nativeKeys(Ui::Shortcut::SendMessage);
    _hint->setText(
        multiTeam
            ? tr("%1 to move · %2 to switch workspace · %3 to open").arg(upDown, leftRight, enter)
            : tr("%1 to move · %2 to open").arg(upDown, enter)
    );
    auto *hintRow = new QVBoxLayout;
    hintRow->setContentsMargins(kCardPadH, sp.md, kCardPadH, kCardPadB);
    hintRow->addWidget(_hint);
    lay->addLayout(hintRow);

    // Open on the workspace that is on screen: with no query the list is that
    // workspace's recent chats, exactly what the single-workspace dialog showed.
    int initial = 0;
    for (size_t i = 0; i < _workspaces.size(); ++i)
        if (!activeTeamId.isEmpty() && _workspaces[i].teamId == activeTeamId)
            initial = static_cast<int>(i);
    connect(_tabs, &WorkspaceTabStrip::currentChanged, this, &QuickSwitcherDialog::onTabChanged);
    _tabs->setCurrentIndex(initial);
    buildItems(_tabs->currentIndex());
    applyFilter({});

    connect(_searchEdit, &StyledLineEdit::textChanged, this, &QuickSwitcherDialog::applyFilter);
    connect(_searchEdit, &StyledLineEdit::returnPressed, this, [this] {
        _list->activateSelected();
    });

    applyTheme();
    updateCard();
}

void QuickSwitcherDialog::buildItems(int workspaceIndex) {
    if (workspaceIndex < 0 || workspaceIndex >= static_cast<int>(_workspaces.size()))
        return;
    if (workspaceIndex == _shownWorkspace)
        return;
    _shownWorkspace = workspaceIndex;

    const auto                       &convs = _workspaces[workspaceIndex].conversations;
    std::vector<BrowseListView::Item> items;
    items.reserve(convs.size());
    for (const auto &conv : convs) {
        if (conv.name.isEmpty())
            continue; // an id we can't name yet is not something to offer

        const bool isDm = conv.kind == ConvKind::Im || conv.kind == ConvKind::Mpim;

        BrowseListView::Item it;
        it.id        = conv.id.value;
        it.title     = conv.name;
        // No subtitle: this list is names, not metadata. Group DMs share the
        // person treatment (an initial disc) — a "#" would read as a channel.
        it.isPerson  = isDm;
        it.isPrivate = conv.kind == ConvKind::PrivateChannel;
        it.avatarUrl = conv.avatarUrl;
        it.initial   = conv.name.left(1);
        it.searchKey = conv.name.toLower();
        it.rankBias  = rankBiasFor(conv);
        items.push_back(std::move(it));
    }
    _list->setItems(std::move(items));
}

std::optional<double>
QuickSwitcherDialog::bestScore(const Workspace &ws, const QString &query) const {
    // Same key and bias as the list rows, so "the workspace with the best
    // match" is the workspace whose top row would rank highest.
    std::optional<double> best;
    for (const auto &conv : ws.conversations) {
        if (conv.name.isEmpty())
            continue;
        if (const auto s = Fuzzy::score(query, conv.name)) {
            const double v = *s + rankBiasFor(conv);
            if (!best || v > *best)
                best = v;
        }
    }
    return best;
}

void QuickSwitcherDialog::applyFilter(const QString &query) {
    const int n = static_cast<int>(_workspaces.size());
    if (query.isEmpty()) {
        // A fresh query starts with a fresh mind: the next letters may re-aim.
        _manualTab = false;
        _tabs->setDimmed({});
    } else if (n > 1) {
        std::vector<std::optional<double>> scores;
        scores.reserve(n);
        std::vector<bool> dimmed;
        dimmed.reserve(n);
        int current = _tabs->currentIndex();
        int best    = -1;
        for (int i = 0; i < n; ++i) {
            scores.push_back(bestScore(_workspaces[i], query));
            dimmed.push_back(!scores.back().has_value());
            if (scores.back() && (best < 0 || *scores.back() > *scores[best]))
                best = i;
        }
        // Ties keep the tab that is showing: a name present in both workspaces
        // should not flip the view away from the one the user is looking at.
        if (best >= 0 && current >= 0 && scores[current] && *scores[current] == *scores[best])
            best = current;
        _tabs->setDimmed(std::move(dimmed));
        // Re-aim at the most probable workspace — unless the user chose this
        // tab by hand and it still has something to show.
        const bool currentHasMatch = current >= 0 && scores[current].has_value();
        if (best >= 0 && best != current && (!_manualTab || !currentHasMatch)) {
            _autoSwitching = true;
            _tabs->setCurrentIndex(best); // → onTabChanged → buildItems
            _autoSwitching = false;
        }
    }

    refreshList(query);
}

void QuickSwitcherDialog::refreshList(const QString &query) {
    _list->applyFilter(query);
    // Preselect the top match so Enter always opens something: with no keyboard
    // selection the list would need an arrow press first.
    _list->setSelectedRow(0);

    const bool any = _list->visibleCount() > 0;
    _list->setVisible(any);
    _empty->setVisible(!any);
    if (any)
        return;
    // Point at the other tabs when they hold what this one doesn't.
    bool elsewhere = false;
    if (_workspaces.size() > 1 && !query.isEmpty() && _shownWorkspace >= 0)
        for (size_t i = 0; i < _workspaces.size() && !elsewhere; ++i)
            elsewhere = static_cast<int>(i) != _shownWorkspace &&
                        bestScore(_workspaces[i], query).has_value();
    _empty->setText(
        elsewhere ? tr("No matches in %1. Other workspaces have some.")
                        .arg(_workspaces[_shownWorkspace].name)
                  : tr("No conversations match.")
    );
}

void QuickSwitcherDialog::onTabChanged(int index) {
    buildItems(index);
    if (_autoSwitching)
        return; // applyFilter() carries on with the new items
    // A tab picked *while a query is up* is the user overruling the re-aim:
    // it holds until the query is cleared (or runs dry). A tab picked over an
    // empty field is just browsing recents — the first letters typed must
    // still re-aim, or stepping through workspaces before typing would quietly
    // disable the whole "most probable workspace" behaviour.
    _manualTab = !_searchEdit->text().isEmpty();
    refreshList(_searchEdit->text());
}

void QuickSwitcherDialog::stepWorkspace(int delta) {
    if (_tabs->count() > 1)
        _tabs->step(delta);
}

void QuickSwitcherDialog::showEvent(QShowEvent *e) {
    AppDialog::showEvent(e);
    // AppDialog hands focus to the first focusable child; make sure that is the
    // field and not the list, so typing filters immediately.
    QTimer::singleShot(0, this, [this] {
        _searchEdit->lineEdit()->setFocus();
        _searchEdit->lineEdit()->selectAll();
    });
}

bool QuickSwitcherDialog::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _searchEdit->lineEdit() && event->type() == QEvent::KeyPress) {
        auto      *ke        = static_cast<QKeyEvent *>(event);
        const bool multiTeam = _tabs->count() > 1;
        switch (ke->key()) {
        case Qt::Key_Down:
            _list->moveSelection(1);
            return true;
        case Qt::Key_Up:
            _list->moveSelection(-1);
            return true;
        // ←/→ switch workspaces instead of moving the caret: queries here are
        // a few letters edited with Backspace, and the tabs are what the keys
        // visibly point at. With one workspace the field keeps them.
        case Qt::Key_Left:
            if (!multiTeam)
                break;
            stepWorkspace(-1);
            return true;
        case Qt::Key_Right:
            if (!multiTeam)
                break;
            stepWorkspace(1);
            return true;
        case Qt::Key_Tab:
            if (!multiTeam)
                break;
            stepWorkspace(1);
            return true;
        case Qt::Key_Backtab:
            if (!multiTeam)
                break;
            stepWorkspace(-1);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            _list->activateSelected();
            return true;
        default:
            break;
        }
    }
    return AppDialog::eventFilter(obj, event);
}

void QuickSwitcherDialog::applyTheme() {
    AppDialog::applyTheme(); // card + backdrop

    // StyledLineEdit themes itself; the two labels don't.
    const auto &th = Th::c();
    if (_hint)
        _hint->setStyleSheet(
            u"font-size: %1px; color: %2;"_s.arg(th.fonts.caption).arg(Th::qss(th.text.tertiary))
        );
    if (_empty)
        _empty->setStyleSheet(
            u"font-size: %1px; color: %2;"_s.arg(th.fonts.base).arg(Th::qss(th.text.tertiary))
        );
}
