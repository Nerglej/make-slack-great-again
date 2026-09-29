// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <memory>

namespace Spell {

// A spelling language this machine can check: `code` is the backend's own id
// ("en_US" for Hunspell and AppKit, "en-US" on Windows), `name` is for people.
struct Language {
    QString code;
    QString name;
};

// Platform spell checker behind Spell::Checker, one per enabled set of
// languages. Backends (see spell_backend_{linux,mac,win}):
//   • Linux   — the vendored, trimmed Hunspell (third_party/hunspell) over the
//               system's dictionaries (/usr/share/hunspell, …/myspell). The
//               static release binary can't dlopen Enchant or Sonnet providers.
//   • macOS   — NSSpellChecker: the system languages and learned words.
//   • Windows — ISpellChecker (Windows 8+), one checker per language.
// A word is correct when any of the loaded languages accepts it.
class Backend {
public:
    virtual ~Backend() = default;

    // Get ready to check `codes`. May block for a while (Hunspell reads each
    // dictionary: 30–50 ms and 8–27 MB apiece), so the checker calls it on a
    // worker thread when loadsOffThread(); nothing else runs on the backend
    // until it returns. False when none of the languages could be loaded.
    virtual bool load(const QStringList &codes) = 0;
    virtual bool loadsOffThread() const { return false; }

    // Main thread, after load().
    virtual bool        check(const QString &word)            = 0;
    virtual QStringList suggest(const QString &word, int max) = 0;
    // Into the system's user dictionary (macOS, Windows) or msga's own word
    // list (Linux).
    virtual void        addToDictionary(const QString &word)  = 0;
};

// The backend for the platform this binary was built for.
std::unique_ptr<Backend> createPlatformBackend();

// The languages this machine can check, without loading any of them.
QList<Language> availableLanguages();

// For someone whose availableLanguages() is empty: on Linux, the package with
// the system language's dictionary under the distribution's name for it
// ("hunspell-en-us"); "" where the OS always ships its languages.
QString dictionaryPackageHint();

// "American English (en_US)": the language's own name for itself — readable to
// its speakers whatever the app language — and the code that tells variants
// ("de_DE", "de_DE_frami") apart. Shared by the backends (spell_checker.cpp).
QString languageName(const QString &code);

} // namespace Spell
