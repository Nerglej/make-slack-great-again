// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "teammate_page.h"
#include "backend/backend.h"
#include "session/session.h"
#include "ui/browse_channels_dialog/browse_list_view.h"
#include "ui/context_menu/context_menu.h"
#include "ui/file_dialog_utils.h"
#include "ui/image_cache.h"
#include "ui/styled_button/styled_button.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"
#include "ui/user_avatar.h"
#include "util/recent_folders.h"
#include "util/relative_time.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSettings>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace {

constexpr int kAvatarSize   = 56;
constexpr int kAvatarRadius = 12;

QString homeRelative(const QString &path) {
    const QString home = QDir::homePath();
    if (!home.isEmpty() && (path == home || path.startsWith(home + QLatin1Char('/'))))
        return QLatin1Char('~') + path.mid(home.size());
    return QDir::toNativeSeparators(path);
}

QString folderKey(const QString &role) {
    return QStringLiteral("claudeCode/lastDir/") + role;
}

} // namespace

TeammatePage::TeammatePage(ImageCache *imgCache, QWidget *parent)
    : QWidget(parent), _imgCache(imgCache) {
    setObjectName("teammatePage");
    setAttribute(Qt::WA_StyledBackground);

    const auto &sp   = Th::c().spacing;
    auto       *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── Who it is ────────────────────────────────────────────────────
    _header = new QWidget(this);
    _header->setObjectName("teammateHeader");
    _header->setAttribute(Qt::WA_StyledBackground);
    auto *headerLayout = new QHBoxLayout(_header);
    headerLayout->setContentsMargins(sp.xxl, sp.xxl, sp.xxl, sp.xl);
    headerLayout->setSpacing(sp.xl);
    _avatar = new QLabel(_header);
    _avatar->setFixedSize(kAvatarSize, kAvatarSize);
    headerLayout->addWidget(_avatar, 0, Qt::AlignTop);
    auto *who = new QVBoxLayout();
    who->setContentsMargins(0, 0, 0, 0);
    who->setSpacing(sp.sm);
    who->addStretch(1);
    _name = new QLabel(_header);
    who->addWidget(_name);
    _description = new QLabel(_header);
    _description->setWordWrap(true);
    who->addWidget(_description);
    who->addStretch(1);
    headerLayout->addLayout(who, 1);
    _editBtn = new StyledButton(tr("Edit teammate…"), StyledButton::Variant::Secondary, _header);
    _editBtn->setSize(StyledButton::Size::Small);
    _editBtn->setFocusPolicy(Qt::NoFocus);
    headerLayout->addWidget(_editBtn, 0, Qt::AlignTop);
    connect(_editBtn, &QPushButton::clicked, this, [this] {
        if (!_mate.id.isEmpty())
            emit editRequested(_mate.id);
    });
    root->addWidget(_header);

    // ── Its sessions ─────────────────────────────────────────────────
    _listTitle = new QLabel(tr("Sessions"), this);
    _listTitle->setContentsMargins(sp.xxl, sp.lg, sp.xxl, sp.sm);
    root->addWidget(_listTitle);

    _stack = new QStackedWidget(this);
    _empty = new QLabel(tr("No sessions yet. Write below to start one."), _stack);
    _empty->setAlignment(Qt::AlignCenter);
    _empty->setWordWrap(true);
    _stack->addWidget(_empty);
    _list = new BrowseListView(imgCache, _stack);
    _list->setOnContentSurface(true);
    _list->setAvatarRadius(8);
    _list->onActivated = [this](const QString &id) { emit openSessionRequested({id}); };
    _stack->addWidget(_list);
    root->addWidget(_stack, 1);

    // ── Where a new one starts ───────────────────────────────────────
    _footer = new QWidget(this);
    _footer->setObjectName("teammateFooter");
    _footer->setAttribute(Qt::WA_StyledBackground);
    auto *footerLayout = new QVBoxLayout(_footer);
    // No bottom margin: the composer below already opens with its own sp.md top
    // margin, so the gap under the row matches the one above it.
    footerLayout->setContentsMargins(sp.xxl, sp.md, sp.xxl, 0);
    footerLayout->setSpacing(sp.sm);
    auto *folderRow = new QHBoxLayout();
    folderRow->setContentsMargins(0, 0, 0, 0);
    folderRow->setSpacing(sp.md);
    _folderLabel = new QLabel(_footer);
    _folderLabel->setTextFormat(Qt::RichText);
    folderRow->addWidget(_folderLabel, 1);
    _folderBtn = new StyledButton(tr("Change folder"), StyledButton::Variant::Ghost, _footer);
    _folderBtn->setSize(StyledButton::Size::Small);
    _folderBtn->setFocusPolicy(Qt::NoFocus);
    folderRow->addWidget(_folderBtn);
    footerLayout->addLayout(folderRow);
    root->addWidget(_footer);

    connect(_folderBtn, &QPushButton::clicked, this, [this] { showFolderMenu(); });
    if (_imgCache)
        connect(_imgCache, &ImageCache::loaded, this, [this](const QString &url) {
            if (url == _mate.avatarUrl)
                updateAvatar();
        });

    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] { applyTheme(); });
}

void TeammatePage::setSession(Session *session) {
    if (_session == session)
        return;
    _session         = session;
    _sessionLifetime = rpl::lifetime();
    clear();
    if (!_session)
        return;
    // Sessions come, go, start and finish working: follow them while shown.
    _session->conversations() |
        rpl::on_next(
            [this](const std::vector<Conversation> &) { scheduleRebuild(); }, _sessionLifetime
        );
    _session->users() |
        rpl::on_next([this](const std::vector<User> &) { scheduleRebuild(); }, _sessionLifetime);
}

void TeammatePage::open(const AgentRole &mate) {
    _mate = mate;
    _name->setText(mate.name);
    _description->setText(mate.description);
    _description->setVisible(!mate.description.isEmpty());
    updateAvatar();
    QSettings     s("msga", "msga");
    const QString any = s.value("claudeCode/lastDir", QDir::homePath()).toString();
    setFolder(s.value(folderKey(mate.id), any).toString());
    rebuild();
}

void TeammatePage::clear() {
    _mate = {};
    _list->setItems({});
    _stack->setCurrentWidget(_empty);
}

void TeammatePage::scheduleRebuild() {
    if (_rebuildQueued || !isVisible())
        return; // open() rebuilds when the page comes back
    _rebuildQueued = true;
    QTimer::singleShot(0, this, [this] {
        _rebuildQueued = false;
        if (isVisible())
            rebuild();
    });
}

void TeammatePage::rebuild() {
    if (!_session || _mate.id.isEmpty())
        return;
    struct Row {
        BrowseListView::Item item;
        bool                 working  = false;
        qint64               activity = 0; // epoch micros
    };
    std::vector<Row> rows;
    for (const Conversation &c : _session->currentConversations()) {
        if (c.agentRole != _mate.id || c.kind != ConvKind::Im || !c.dmUser)
            continue;
        const User *u = _session->findUser(*c.dmUser);
        Row         r;
        r.item.id        = c.id.value;
        r.item.isPerson  = true;
        r.item.avatarUrl = u ? u->avatarUrl : _mate.avatarUrl;
        r.item.title     = u && !u->displayName.isEmpty() ? u->displayName : c.name;
        r.item.initial   = r.item.title.left(1).toUpper();
        r.working        = u && u->isActive;
        r.activity       = std::max(
            c.latestTs.isEmpty() ? 0 : decimalTsToMicros(c.latestTs),
            c.lastRead.isEmpty() ? 0 : decimalTsToMicros(c.lastRead)
        );
        QStringList sub;
        if (!c.description.isEmpty())
            sub << c.description; // the folder (and "no permission checks")
        if (r.activity > 0)
            sub << relativeTime(r.activity / 1000000);
        r.item.subtitle = sub.join(QStringLiteral(" · "));
        // What needs a look first: news, else what it's doing right now.
        if (c.mentionCount > 0) {
            r.item.badge       = tr("%n new", "", c.mentionCount);
            r.item.badgeStrong = true;
        } else if (u && !u->statusText.isEmpty()) {
            r.item.badge = u->statusText;
        }
        r.item.badgeCheck = false;
        rows.push_back(std::move(r));
    }
    // Working ones on top, then the most recent.
    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.working != b.working)
            return a.working;
        return a.activity > b.activity;
    });
    std::vector<BrowseListView::Item> items;
    items.reserve(rows.size());
    for (auto &r : rows)
        items.push_back(std::move(r.item));
    const bool none = items.empty();
    _list->setItems(std::move(items));
    _list->applyFilter({});
    _stack->setCurrentWidget(none ? static_cast<QWidget *>(_empty) : _list);
}

void TeammatePage::setFolder(const QString &dir) {
    _folder  = dir;
    _blocker = _session ? _session->backend()->agentSessionBlocker(dir) : QString();
    _folderLabel->setText(
        tr("Start new session in %1").arg("<b>" + homeRelative(dir).toHtmlEscaped() + "</b>")
    );
    emit folderChanged();
}

// The folders sessions were last started in or work in, newest first, then
// "Browse…" for any other. Built on every open: sessions come and go.
void TeammatePage::showFolderMenu() {
    std::vector<RecentFolders::SessionFolder> sessions;
    if (_session)
        for (const Conversation &c : _session->currentConversations()) {
            if (c.kind != ConvKind::Im)
                continue;
            const QString dir = _session->backend()->agentSessionFolder(c.id);
            if (dir.isEmpty())
                continue;
            const qint64 micros = std::max(
                c.latestTs.isEmpty() ? 0 : decimalTsToMicros(c.latestTs),
                c.lastRead.isEmpty() ? 0 : decimalTsToMicros(c.lastRead)
            );
            sessions.push_back({dir, micros / 1000000});
        }
    QSettings  s("msga", "msga");
    const auto isDir = [](const QString &p) { return QFileInfo(p).isDir(); };
    auto       choices =
        RecentFolders::rank(RecentFolders::load(s, RecentFolders::kClaudeCodeKey), sessions, isDir);
    // The current one is always there to see (e.g. home, never picked).
    const QString current = RecentFolders::normalized(_folder);
    if (!current.isEmpty() &&
        std::none_of(
            choices.begin(), choices.end(), [&](const auto &c) { return c.path == current; }
        ) &&
        isDir(current))
        choices.insert(choices.begin(), RecentFolders::Choice{current});

    auto *menu = new ContextMenu(this);
    for (const RecentFolders::Choice &c : choices) {
        ContextMenu::Item it;
        it.text     = homeRelative(c.path);
        it.selected = c.path == current;
        QStringList hint;
        if (c.sessions > 0)
            hint << tr("%Ln session(s)", "", c.sessions);
        if (c.lastUsed > 0)
            hint << relativeTime(c.lastUsed);
        it.hint   = hint.join(QStringLiteral(" · "));
        it.action = [this, path = c.path] { pickFolder(path); };
        menu->addItem(std::move(it));
    }
    menu->addSeparator();
    menu->addItem(tr("Browse…"), [this] {
        // After the menu is gone: the dialog runs its own loop.
        QTimer::singleShot(0, this, [this] { chooseFolder(); });
    });
    menu->popup(_folderBtn->mapToGlobal(QPoint(0, _folderBtn->height() + 2)));
}

void TeammatePage::chooseFolder() {
    const QString dir = Ui::getExistingDirectory(
        this, tr("Folder for new sessions with the %1").arg(_mate.name), _folder
    );
    if (!dir.isEmpty())
        pickFolder(dir);
}

// Today's default for this teammate, and the most recent pick for all of them.
void TeammatePage::pickFolder(const QString &dir) {
    if (_mate.id.isEmpty())
        return;
    QSettings s("msga", "msga");
    s.setValue(folderKey(_mate.id), dir);
    RecentFolders::bump(s, RecentFolders::kClaudeCodeKey, dir);
    setFolder(dir);
}

void TeammatePage::updateAvatar() {
    const qreal dpr = devicePixelRatioF();
    QPixmap     canvas(QSize(kAvatarSize, kAvatarSize) * dpr);
    canvas.setDevicePixelRatio(dpr);
    canvas.fill(Qt::transparent);
    const QPixmap px =
        (_imgCache && !_mate.avatarUrl.isEmpty()) ? _imgCache->get(_mate.avatarUrl) : QPixmap{};
    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect r(0, 0, kAvatarSize, kAvatarSize);
    if (!px.isNull())
        UserAvatar::paintPhoto(p, r, px, dpr, kAvatarRadius);
    else
        UserAvatar::paintInitial(
            p,
            r,
            _mate.name.left(1),
            Th::c().presence.away,
            Qt::white,
            kAvatarRadius,
            r.height() * 0.38
        );
    p.end();
    _avatar->setPixmap(canvas);
}

void TeammatePage::applyTheme() {
    const auto &th = Th::c();
    Th::setStyleSheetIfChanged(
        this, QString("QWidget#teammatePage { background: %1; }").arg(Th::qss(th.surface.content))
    );
    Th::setStyleSheetIfChanged(
        _header, QString("QWidget#teammateHeader { background: transparent; }")
    );
    Th::setStyleSheetIfChanged(
        _footer,
        QString(
            "QWidget#teammateFooter { background: transparent; "
            "border-top: 1px solid %1; }"
        )
            .arg(Th::qss(th.divider.subtle))
    );
    Th::setStyleSheetIfChanged(
        _name,
        QString("background: transparent; font-weight: bold; font-size: %1px; color: %2;")
            .arg(th.fonts.xxxl)
            .arg(Th::qss(th.text.primary))
    );
    Th::setStyleSheetIfChanged(
        _description,
        QString("background: transparent; font-size: %1px; color: %2;")
            .arg(th.fonts.lg)
            .arg(Th::qss(th.text.secondary))
    );
    Th::setStyleSheetIfChanged(
        _listTitle,
        QString("background: transparent; font-weight: bold; font-size: %1px; color: %2;")
            .arg(th.fonts.xl)
            .arg(Th::qss(th.text.primary))
    );
    Th::setStyleSheetIfChanged(
        _empty,
        QString("background: transparent; color: %1; padding: %2px;")
            .arg(Th::qss(th.text.secondary))
            .arg(th.spacing.xxl)
    );
    Th::setStyleSheetIfChanged(
        _folderLabel,
        QString("background: transparent; color: %1; font-size: %2px;")
            .arg(Th::qss(th.text.secondary))
            .arg(th.fonts.md)
    );
    updateAvatar();
}
