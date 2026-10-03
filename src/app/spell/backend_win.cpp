// Windows: the system spell checker (ISpellCheckerFactory, Windows 8+), one
// ISpellChecker per language; "Add to dictionary" goes to that language's
// user dictionary, shared with every other app. spellcheck.h ships with the
// Windows SDK and mingw-w64; a toolchain without it builds a checker that
// offers no languages. COM is initialised on the UI thread by plat (OLE drag
// and drop), and every call here happens there.
#include "app/spell/spell.h"
#include "app/spell/spell_internal.h"
#include "base/winstr.h"

#include <windows.h>

#include <algorithm>

#if __has_include(<spellcheck.h>)
#include <spellcheck.h>
#define MSGA_WIN_SPELLCHECK 1
#endif

namespace spell {

namespace {

#if defined(MSGA_WIN_SPELLCHECK)

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
std::vector<std::string> strings(IEnumString *e, int max = -1) {
    std::vector<std::string> out;
    LPOLESTR                 s = nullptr;
    while ((max < 0 || int(out.size()) < max) && e->Next(1, &s, nullptr) == S_OK) {
        out.push_back(base::narrow(s));
        CoTaskMemFree(s);
    }
    return out;
}

class WinBackend : public Backend {
public:
    bool load(const std::vector<std::string> &codes) override {
        auto f = factory();
        if (!f)
            return false;
        for (const std::string &code : codes) {
            const std::wstring tag = base::wide(code);
            BOOL               ok  = FALSE;
            if (FAILED(f->IsSupported(tag.c_str(), &ok)) || !ok)
                continue;
            Com<ISpellChecker> c;
            if (SUCCEEDED(f->CreateSpellChecker(tag.c_str(), c.put())) && c)
                _checkers.push_back(std::move(c));
        }
        return !_checkers.empty();
    }

    bool check(std::string_view word) override {
        const std::wstring w = base::wide(word);
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

    std::vector<std::string> suggest(std::string_view word, int max) override {
        std::vector<std::string> out;
        const std::wstring       w = base::wide(word);
        for (const auto &c : _checkers) {
            Com<IEnumString> e;
            if (FAILED(c->Suggest(w.c_str(), e.put())) || !e)
                continue;
            for (std::string &s : strings(e.get(), max))
                if (std::find(out.begin(), out.end(), s) == out.end() && int(out.size()) < max)
                    out.push_back(std::move(s));
        }
        return out;
    }

    void addToDictionary(std::string_view word) override {
        const std::wstring w = base::wide(word);
        for (const auto &c : _checkers)
            c->Add(w.c_str());
    }

private:
    std::vector<Com<ISpellChecker>> _checkers;
};

#else // no <spellcheck.h>

class WinBackend : public Backend {
public:
    bool                     load(const std::vector<std::string> &) override { return false; }
    bool                     check(std::string_view) override { return true; }
    std::vector<std::string> suggest(std::string_view, int) override { return {}; }
    void                     addToDictionary(std::string_view) override {}
};

#endif

} // namespace

std::unique_ptr<Backend> createPlatformBackend(plat::App &) {
    return std::make_unique<WinBackend>();
}

namespace detail {

bool listsOffThread() {
    return false;
}

std::vector<Language> availableLanguages() {
    std::vector<Language> out;
#if defined(MSGA_WIN_SPELLCHECK)
    auto             f = factory();
    Com<IEnumString> e;
    if (!f || FAILED(f->get_SupportedLanguages(e.put())) || !e)
        return out;
    for (const std::string &code : strings(e.get()))
        out.push_back({code, languageName(code)});
#endif
    return out;
}

std::string dictionaryPackageHint(const std::vector<std::string> &) {
    return {};
}

} // namespace detail

} // namespace spell
