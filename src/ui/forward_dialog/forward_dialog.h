// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "ui/app_dialog/app_dialog.h"
#include "backend/domain.h"

#include <functional>
#include <optional>
#include <vector>

class Dropdown;
class QFrame;
class Session;
class ConvSelectorWidget;
class ComposerWidget;
class StyledButton;

// Dialog for forwarding a message to another conversation.
// User picks a target conversation, optionally adds a comment via the composer,
// and confirms. Accepted → caller reads targetSession(), targetConv(),
// comment() and files().
class ForwardDialog : public AppDialog {
    Q_OBJECT
public:
    // A workspace the message may be forwarded into.
    struct Workspace {
        Session *session = nullptr;
        QString  name;
    };

    // `session` owns the message (its preview renders against it). With two or
    // more `workspaces`, a picker above the conversation selector chooses where
    // the message goes, starting on `session`; otherwise it stays in `session`.
    // With `onlyFile` (a file's own Share action) just that file is forwarded:
    // the preview shows it alone and the message text stays behind.
    explicit ForwardDialog(
        const Message         &msg,
        Session               *session,
        std::vector<Workspace> workspaces = {},
        std::optional<File>    onlyFile   = std::nullopt,
        QWidget               *parent     = nullptr
    );

    Session                 *targetSession() const { return _target; }
    // True if the dialog holds `session` (as target or picker choice): the host
    // must close it before that session is destroyed.
    bool                     usesSession(const Session *session) const;
    ConversationId           targetConv() const;
    QString                  comment() const;
    // The files that go along: the message's, or just the one being shared.
    const std::vector<File> &files() const { return _files; }
    bool                     fileOnly() const { return _fileOnly; }

protected:
    void applyTheme() override;

private:
    void setTargetSession(Session *session);

    std::vector<Workspace> _workspaces;
    std::vector<File>      _files;
    bool                   _fileOnly    = false;
    Session               *_target      = nullptr;
    Dropdown              *_wsPicker    = nullptr;
    ConvSelectorWidget    *_selector    = nullptr;
    ComposerWidget        *_composer    = nullptr;
    QFrame                *_previewCard = nullptr;
    StyledButton          *_copyLinkBtn = nullptr;
    StyledButton          *_cancelBtn   = nullptr;
    StyledButton          *_fwdBtn      = nullptr;
};

// Local copies of `files` for re-uploading them elsewhere, fetched through the
// `source` session that can read them. done(paths, links, error) fires once:
// `paths` in file order; `links` are permalinks of files that have no bytes to
// re-upload (canvases, links-only files) and go along as text; a non-empty
// `error` means a download failed and nothing should be sent.
void fetchForwardedFiles(
    Session                                                                 *source,
    const std::vector<File>                                                 &files,
    std::function<void(QStringList paths, QStringList links, QString error)> done
);
