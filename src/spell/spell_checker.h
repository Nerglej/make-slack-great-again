// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include "spell/spell_backend.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <memory>

namespace Spell {

// The composer's spell checker (Settings → Appearance → Composer → "Check
// spelling"). Off by default, and while off there is no backend and no
// dictionary in memory: turning it off frees them. Turning it on loads the
// chosen languages in the background (Backend::loadsOffThread) and emits
// changed() once words can be checked. Main thread only.
class Checker : public QObject {
    Q_OBJECT
public:
    static Checker &instance();

    static constexpr char kEnabledKey[]   = "composer/spellCheck";
    static constexpr char kLanguagesKey[] = "composer/spellLanguages";

    bool        enabled() const { return _enabled; }
    // The chosen languages' codes; empty = the system language's
    // (defaultLanguages).
    QStringList languages() const { return _languages; }
    // Persist and apply (a no-op when nothing changed).
    void        configure(bool enabled, const QStringList &languages);

    // Enabled with its dictionaries loaded: words get checked. False while
    // they load, so nothing is underlined yet.
    bool isActive() const { return _backend != nullptr; }

    // False while inactive, and for an ignored word. Cached per word — on
    // macOS each check is an XPC round trip.
    bool        isMisspelled(const QString &word);
    // Up to `max`, best first; empty while inactive. Hunspell can take
    // 15–120 ms, so this belongs where the user just asked (a context menu),
    // never in a per-keystroke path.
    QStringList suggestions(const QString &word, int max = 5);
    void        addToDictionary(const QString &word);
    void        ignore(const QString &word); // for the rest of this run only

    // What to check when no language was chosen: the system locale's own
    // dictionary ("en_US"), else another of its language ("en_GB"), else the
    // first there is.
    static QStringList defaultLanguages(const QList<Language> &available);

    // Tests: check with `backend` (already loaded) as if it were the
    // platform's; nullptr goes back to what configure() set up.
    void setBackendForTesting(std::unique_ptr<Backend> backend);

signals:
    // Whether and against what words are checked changed (on/off, languages,
    // loading finished, a word added or ignored): re-check what's on screen.
    void changed();

private:
    Checker();
    void apply();
    void reset();

    bool                     _enabled = false;
    QStringList              _languages;
    std::shared_ptr<Backend> _backend;        // null while off or loading
    int                      _generation = 0; // drops a load that apply() superseded
    QHash<QString, bool>     _misspelled;     // the word → isMisspelled cache
    QSet<QString>            _ignored;
};

} // namespace Spell
