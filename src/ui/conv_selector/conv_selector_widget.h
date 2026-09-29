// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "backend/domain.h"

#include <QWidget>
#include <utility>
#include <vector>

class QFrame;
class QLineEdit;
class QLabel;
class QPushButton;
class QListWidget;
class Session;

// Somewhere in a workspace a message can go: a conversation it already has, or
// a chat that starts on the way there — with a person who has no DM yet
// (Capabilities::openDm) or a new session with a teammate (Backend::agentRoles).
struct ChatTarget {
    enum class Kind { None, Conversation, Person, Teammate };
    Kind           kind = Kind::None;
    ConversationId conv; // Kind::Conversation
    UserId         user; // Kind::Person
    QString        role; // Kind::Teammate: AgentRole::id

    static ChatTarget conversation(ConversationId id) {
        return {Kind::Conversation, std::move(id), {}, {}};
    }
    static ChatTarget person(UserId id) { return {Kind::Person, {}, std::move(id), {}}; }
    static ChatTarget teammate(QString roleId) {
        return {Kind::Teammate, {}, {}, std::move(roleId)};
    }

    bool isEmpty() const { return kind == Kind::None; }
    bool operator==(const ChatTarget &) const = default;
};

// Combobox-style picker of where to write in a session: its conversations,
// then the people there is no DM with yet and the teammates to start a
// session with. Shows a search field; typing filters a dropdown list ("#"
// scopes it to channels, "@" to people, DMs and teammates). After selection,
// the field shows a chip with the name and an ×-clear button. Emits
// targetSelected() on selection and targetSelected({}, {}) when cleared.
class ConvSelectorWidget : public QWidget {
    Q_OBJECT
public:
    // At most this many people without a DM are offered at once (big rosters).
    static constexpr int kMaxPeopleRows = 50;

    explicit ConvSelectorWidget(Session *session, QWidget *parent = nullptr);
    ~ConvSelectorWidget();

    const ChatTarget &selectedTarget() const { return _selected; }
    // The picked conversation; empty while none or a chat not started yet is.
    ConversationId    selectedConv() const {
        return _selected.kind == ChatTarget::Kind::Conversation ? _selected.conv : ConversationId{};
    }
    QString selectedName() const { return _selectedName; }

    // Pick from another session's conversations instead. Any selection belongs
    // to the old session, so it is cleared (emitting targetSelected({}, {})).
    void setSession(Session *session);

signals:
    void targetSelected(const ChatTarget &target, const QString &name);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void applyTheme();
    void openDropdown();
    void closeDropdown();
    void positionDropdown();
    void rebuildList(const QString &filter);
    void selectRow(int row);
    void clearSelection();
    void showChip();
    void showSearch();

    Session   *_session;
    ChatTarget _selected;
    QString    _selectedName;

    // Input frame (always visible)
    QFrame    *_inputFrame = nullptr;
    QLineEdit *_searchEdit = nullptr;

    // Chip shown when selection made (inside _inputFrame)
    QWidget     *_chip      = nullptr;
    QLabel      *_chipLabel = nullptr;
    QPushButton *_chipClear = nullptr;

    // Dropdown — parented to window() so it overlays siblings
    QFrame                 *_dropdown = nullptr;
    QListWidget            *_dropList = nullptr;
    std::vector<ChatTarget> _listTargets;
};
