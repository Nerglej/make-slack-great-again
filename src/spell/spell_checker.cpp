// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "spell/spell_checker.h"

#include <QFutureWatcher>
#include <QLocale>
#include <QSettings>
#include <QtConcurrent/QtConcurrentRun>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

using namespace Qt::StringLiterals;

namespace Spell {

namespace {

// Past this many distinct words the cache starts over, so a long session of
// typing can't grow it without bound.
constexpr int kCacheLimit = 20000;

// Apostrophes: dictionaries spell "don't" with U+0027, keyboards and
// autocorrect often give U+2019.
QString normalized(const QString &word) {
    QString w = word;
    w.replace(QChar(0x2019), u'\'');
    return w;
}

} // namespace

QString languageName(const QString &code) {
    const QStringList parts        = QString(code).replace(u'-', u'_').split(u'_');
    const bool        hasTerritory = parts.size() >= 2 && parts[1].size() == 2;
    const QLocale     loc(hasTerritory ? parts[0] + u'_' + parts[1] : parts[0]);
    if (loc.language() == QLocale::C || QLocale::languageToCode(loc.language()) != parts[0])
        return code;
    // Without a territory QLocale picks the language's main one, whose native
    // name can be a regional one ("en" → "American English"): name the
    // language itself instead.
    QString name =
        hasTerritory ? loc.nativeLanguageName() : QLocale::languageToString(loc.language());
    if (name.isEmpty())
        return code;
    name[0] = name[0].toUpper(); // "español", "русский"
    return u"%1 (%2)"_s.arg(name, code);
}

Checker &Checker::instance() {
    static Checker *checker = new Checker; // lives as long as the app
    return *checker;
}

Checker::Checker() {
    const QSettings s(u"msga"_s, u"msga"_s);
    _enabled   = s.value(kEnabledKey, false).toBool();
    _languages = s.value(kLanguagesKey).toStringList();
    if (_enabled)
        apply();
}

void Checker::configure(bool enabled, const QStringList &languages) {
    if (enabled == _enabled && languages == _languages)
        return;
    _enabled   = enabled;
    _languages = languages;
    QSettings s(u"msga"_s, u"msga"_s);
    s.setValue(kEnabledKey, enabled);
    s.setValue(kLanguagesKey, languages);
    apply();
}

void Checker::reset() {
    ++_generation;
    _misspelled.clear();
    if (_backend) {
        _backend.reset();
#if defined(__GLIBC__)
        // Hunspell holds a dictionary in millions of small blocks; hand the
        // pages back to the system instead of keeping them for reuse.
        malloc_trim(0);
#endif
    }
}

void Checker::apply() {
    const bool wasActive = isActive();
    reset();
    if (!_enabled) {
        if (wasActive)
            emit changed();
        return;
    }
    const QStringList codes =
        _languages.isEmpty() ? defaultLanguages(availableLanguages()) : _languages;
    std::shared_ptr<Backend> backend = createPlatformBackend();
    if (!backend->loadsOffThread()) {
        if (backend->load(codes))
            _backend = std::move(backend);
        emit changed();
        return;
    }
    if (wasActive)
        emit changed(); // the old dictionaries are gone while the new ones load
    const int gen     = _generation;
    auto     *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, backend, gen] {
        watcher->deleteLater();
        if (gen != _generation || !watcher->result())
            return; // superseded or nothing loaded; `backend` goes with the watcher
        _backend = backend;
        emit changed();
    });
    watcher->setFuture(QtConcurrent::run([backend, codes] { return backend->load(codes); }));
}

bool Checker::isMisspelled(const QString &word) {
    if (!_backend)
        return false;
    const QString w = normalized(word);
    if (_ignored.contains(w))
        return false;
    const auto it = _misspelled.constFind(w);
    if (it != _misspelled.cend())
        return *it;
    if (_misspelled.size() >= kCacheLimit)
        _misspelled.clear();
    const bool bad = !_backend->check(w);
    _misspelled.insert(w, bad);
    return bad;
}

QStringList Checker::suggestions(const QString &word, int max) {
    if (!_backend)
        return {};
    return _backend->suggest(normalized(word), max);
}

void Checker::addToDictionary(const QString &word) {
    if (!_backend)
        return;
    _backend->addToDictionary(normalized(word));
    _misspelled.clear(); // "msga" also makes "Msga" right
    emit changed();
}

void Checker::ignore(const QString &word) {
    _ignored.insert(normalized(word));
    emit changed();
}

QStringList Checker::defaultLanguages(const QList<Language> &available) {
    const QString sys  = QLocale::system().name(); // "en_US"
    const QString lang = sys.section(u'_', 0, 0);
    const auto    norm = [](QString code) { return code.replace(u'-', u'_'); };
    for (const Language &l : available)
        if (norm(l.code) == sys)
            return {l.code};
    for (const Language &l : available)
        if (norm(l.code).section(u'_', 0, 0) == lang)
            return {l.code};
    if (!available.isEmpty())
        return {available.first().code};
    return {};
}

void Checker::setBackendForTesting(std::unique_ptr<Backend> backend) {
    reset();
    if (backend)
        _backend = std::move(backend);
    else if (_enabled)
        apply();
    emit changed();
}

} // namespace Spell
