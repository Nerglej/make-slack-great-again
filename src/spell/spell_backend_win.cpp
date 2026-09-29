// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "spell/spell_backend.h"

#include <windows.h>

#include <vector>

// Windows: the system spell checker (ISpellCheckerFactory, Windows 8+), one
// ISpellChecker per language; "Add to dictionary" goes to that language's user
// dictionary, shared with every other app. The header ships with the Windows
// SDK and with mingw-w64; a toolchain without it builds a checker that offers
// no languages. COM is already initialised on the GUI thread by Qt (OLE drag
// and drop), and every call here happens there.
#if __has_include(<spellcheck.h>)
#include <spellcheck.h>
#define MSGA_WIN_SPELLCHECK 1
#endif

namespace Spell {

#if defined(MSGA_WIN_SPELLCHECK)
namespace {

// Owns one COM reference.
template <class T>
class Com {
public:
    Com()                       = default;
    Com(const Com &)            = delete;
    Com &operator=(const Com &) = delete;
    Com(Com &&o) noexcept : _p(o._p) { o._p = nullptr; }
    ~Com() {
        if (_p)
            _p->Release();
    }
    T       *operator->() const { return _p; }
    T       *get() const { return _p; }
    T      **put() { return &_p; }
    explicit operator bool() const { return _p != nullptr; }

private:
    T *_p = nullptr;
};

Com<ISpellCheckerFactory> factory() {
    Com<ISpellCheckerFactory> f;
    CoCreateInstance(
        __uuidof(SpellCheckerFactory),
        nullptr,
        CLSCTX_INPROC_SERVER,
        __uuidof(ISpellCheckerFactory),
        reinterpret_cast<void **>(f.put())
    );
    return f;
}

// Drains a COM string enumerator (languages, suggestions).
QStringList strings(IEnumString *e, int max = -1) {
    QStringList out;
    LPOLESTR    s = nullptr;
    while ((max < 0 || out.size() < max) && e->Next(1, &s, nullptr) == S_OK) {
        out << QString::fromWCharArray(s);
        CoTaskMemFree(s);
    }
    return out;
}

class WinBackend : public Backend {
public:
    bool load(const QStringList &codes) override {
        auto f = factory();
        if (!f)
            return false;
        for (const QString &code : codes) {
            const std::wstring tag = code.toStdWString();
            BOOL               ok  = FALSE;
            if (FAILED(f->IsSupported(tag.c_str(), &ok)) || !ok)
                continue;
            Com<ISpellChecker> c;
            if (SUCCEEDED(f->CreateSpellChecker(tag.c_str(), c.put())) && c)
                _checkers.push_back(std::move(c));
        }
        return !_checkers.empty();
    }

    bool check(const QString &word) override {
        const std::wstring w = word.toStdWString();
        for (const auto &c : _checkers) {
            Com<IEnumSpellingError> errors;
            if (FAILED(c->Check(w.c_str(), errors.put())) || !errors)
                continue;
            Com<ISpellingError> first;
            if (errors->Next(first.put()) != S_OK)
                return true; // no error: this language knows the word
        }
        return false;
    }

    QStringList suggest(const QString &word, int max) override {
        QStringList        out;
        const std::wstring w = word.toStdWString();
        for (const auto &c : _checkers) {
            Com<IEnumString> e;
            if (FAILED(c->Suggest(w.c_str(), e.put())) || !e)
                continue;
            for (const QString &s : strings(e.get(), max))
                if (!out.contains(s) && out.size() < max)
                    out << s;
        }
        return out;
    }

    void addToDictionary(const QString &word) override {
        const std::wstring w = word.toStdWString();
        for (const auto &c : _checkers)
            c->Add(w.c_str());
    }

private:
    std::vector<Com<ISpellChecker>> _checkers;
};

} // namespace

std::unique_ptr<Backend> createPlatformBackend() {
    return std::make_unique<WinBackend>();
}

QList<Language> availableLanguages() {
    QList<Language>  out;
    auto             f = factory();
    Com<IEnumString> e;
    if (!f || FAILED(f->get_SupportedLanguages(e.put())) || !e)
        return out;
    for (const QString &code : strings(e.get()))
        out.append({code, languageName(code)});
    return out;
}

#else // no <spellcheck.h>

namespace {
class NoBackend : public Backend {
public:
    bool        load(const QStringList &) override { return false; }
    bool        check(const QString &) override { return true; }
    QStringList suggest(const QString &, int) override { return {}; }
    void        addToDictionary(const QString &) override {}
};
} // namespace

std::unique_ptr<Backend> createPlatformBackend() {
    return std::make_unique<NoBackend>();
}

QList<Language> availableLanguages() {
    return {};
}

#endif

QString dictionaryPackageHint() {
    return {};
}

} // namespace Spell
