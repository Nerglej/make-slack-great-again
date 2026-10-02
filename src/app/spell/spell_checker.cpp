#include "app/spell/spell.h"
#include "app/spell/spell_internal.h"

#include "app/model/jobs.h"
#include "plat/plat.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace spell {

namespace {

// Past this many distinct words the cache starts over, so a long session of
// typing can't grow it without bound.
constexpr size_t kCacheLimit = 20000;
// Words a main-thread backend checks per slice before letting events through
// (macOS: each is an XPC round trip; the cache makes repeats free).
constexpr size_t kSliceWords = 200;

// Apostrophes: dictionaries spell "don't" with U+0027, keyboards and
// autocorrect often give U+2019.
std::string normalized(std::string_view word) {
    std::string w;
    w.reserve(word.size());
    for (size_t i = 0; i < word.size(); ++i) {
        if (word.substr(i, 3) == "\xE2\x80\x99") {
            w += '\'';
            i += 2;
        } else {
            w += word[i];
        }
    }
    return w;
}

} // namespace

// The backend with what it learnt. Workers and the UI thread share it, so
// everything in it is used under `m`; a worker that outlives a configure()
// keeps its own (old) State alive and its answer is dropped by the caller.
struct Checker::State {
    std::mutex                            m;
    std::unique_ptr<Backend>              backend;
    std::unordered_map<std::string, bool> misspelled; // word → answer cache
    std::vector<std::string>              ignored;

    // Under m.
    bool isMisspelled(std::string_view word) {
        const std::string w = normalized(word);
        if (std::find(ignored.begin(), ignored.end(), w) != ignored.end())
            return false;
        const auto it = misspelled.find(w);
        if (it != misspelled.end())
            return it->second;
        if (misspelled.size() >= kCacheLimit)
            misspelled.clear();
        const bool bad = !backend->check(w);
        misspelled.emplace(w, bad);
        return bad;
    }
};

Checker &Checker::instance() {
    static Checker *checker = new Checker; // lives as long as the app
    return *checker;
}

Checker::Checker() = default;

void Checker::configure(plat::App &app, bool enabled, std::vector<std::string> languages) {
    _app = &app;
    if (enabled == _enabled && languages == _languages)
        return;
    _enabled   = enabled;
    _languages = std::move(languages);
    apply();
}

void Checker::reset() {
    ++_generation;
    if (_state) {
        _state.reset();
        _backend = nullptr;
#if defined(__GLIBC__)
        // Hunspell holds a dictionary in millions of small blocks; hand the
        // pages back to the system instead of keeping them for reuse.
        malloc_trim(0);
#endif
    }
}

void Checker::apply() {
    const bool wasActive = active();
    reset();
    if (wasActive)
        changed(); // the old dictionaries are gone (while the new ones load)
    if (!_enabled || !_app)
        return;
    auto state               = std::make_shared<State>();
    state->backend           = createPlatformBackend(*_app);
    const auto     preferred = _app->preferredLanguages();
    const uint32_t gen       = _generation;
    auto           ok        = std::make_shared<bool>(false);
    // The languages (the system's when none were chosen) and the load itself
    // run where the backend allows: a worker for Hunspell's directory scan
    // and dictionary reads, the UI thread for the system checkers.
    auto           load      = [state, ok, codes = _languages, preferred]() mutable {
        if (codes.empty())
            codes = defaultLanguages(detail::availableLanguages(), preferred);
        *ok = !codes.empty() && state->backend->load(codes);
    };
    auto then = [this, state, ok, gen] {
        if (gen != _generation || !*ok)
            return; // superseded, or nothing loaded
        _state   = state;
        _backend = state->backend.get();
        changed();
    };
    if (state->backend->loadsOffThread()) {
        model::runInBackground(*_app, std::move(load), std::move(then));
    } else {
        _app->post([load = std::move(load), then = std::move(then)]() mutable {
            load();
            then();
        });
    }
}

void Checker::check(
    std::string text, std::vector<Span> excluded, std::function<void(std::vector<Span>)> done
) {
    if (!_state || !_app) {
        if (_app)
            _app->post([done = std::move(done)] { done({}); });
        return;
    }
    auto st    = _state;
    auto t     = std::make_shared<std::string>(std::move(text));
    auto words = std::make_shared<std::vector<Span>>();
    auto bad   = std::make_shared<std::vector<Span>>();
    if (_backend->threadSafe()) {
        model::runInBackground(
            *_app,
            [st, t, words, bad, excluded = std::move(excluded)] {
                *words = checkableWords(*t, excluded);
                std::lock_guard<std::mutex> lock(st->m);
                for (const Span &w : *words)
                    if (st->isMisspelled(std::string_view(*t).substr(w.start, w.length)))
                        bad->push_back(w);
            },
            [bad, done = std::move(done)] { done(std::move(*bad)); }
        );
        return;
    }
    // A main-thread checker: the words are cut on a worker, then checked here
    // a slice at a time.
    plat::App *app = _app;
    model::runInBackground(
        *_app,
        [t, words, excluded = std::move(excluded)] { *words = checkableWords(*t, excluded); },
        [app, st, t, words, bad, done = std::move(done)]() mutable {
            auto next = std::make_shared<size_t>(0);
            auto step = std::make_shared<std::function<void()>>();
            *step     = [app, st, t, words, bad, next, step, done = std::move(done)] {
                size_t checked = 0;
                while (*next < words->size() && checked < kSliceWords) {
                    const Span                  w = (*words)[(*next)++];
                    std::lock_guard<std::mutex> lock(st->m);
                    const std::string_view word(std::string_view(*t).substr(w.start, w.length));
                    checked += st->misspelled.count(normalized(word)) ? 0 : 1;
                    if (st->isMisspelled(word))
                        bad->push_back(w);
                }
                if (*next < words->size()) {
                    app->addTimer(1, false, [step] { (*step)(); });
                    return;
                }
                auto cb     = std::move(done);
                auto result = std::move(*bad);
                auto self   = step;    // keeps the closure alive past the reset below
                *self       = nullptr; // breaks the step ↔ closure cycle
                cb(std::move(result));
            };
            (*step)();
        }
    );
}

void Checker::suggest(
    std::string word, int max, std::function<void(std::vector<std::string>)> done
) {
    if (!_state || !_app) {
        if (_app)
            _app->post([done = std::move(done)] { done({}); });
        return;
    }
    auto st  = _state;
    auto out = std::make_shared<std::vector<std::string>>();
    auto run = [st, out, w = normalized(word), max] {
        std::lock_guard<std::mutex> lock(st->m);
        *out = st->backend->suggest(w, max);
    };
    auto then = [out, done = std::move(done)] { done(std::move(*out)); };
    if (_backend->threadSafe())
        return model::runInBackground(*_app, std::move(run), std::move(then));
    _app->post([run = std::move(run), then = std::move(then)]() mutable {
        run();
        then();
    });
}

void Checker::addToDictionary(const std::string &word) {
    if (!_state || !_app)
        return;
    auto st  = _state;
    auto run = [st, w = normalized(word)] {
        std::lock_guard<std::mutex> lock(st->m);
        st->backend->addToDictionary(w); // Linux appends to msga's word list
        st->misspelled.clear();          // "msga" also makes "Msga" right
    };
    const uint32_t gen  = _generation;
    auto           then = [this, gen] {
        if (gen == _generation)
            changed();
    };
    if (_backend->threadSafe())
        return model::runInBackground(*_app, std::move(run), std::move(then));
    run();
    then();
}

void Checker::ignore(const std::string &word) {
    if (_state) {
        std::lock_guard<std::mutex> lock(_state->m);
        _state->ignored.push_back(normalized(word));
    }
    changed();
}

Checker::ObserverId Checker::observe(std::function<void()> fn) {
    _observers.push_back({_nextObserver, std::move(fn)});
    return _nextObserver++;
}

void Checker::unobserve(ObserverId id) {
    std::erase_if(_observers, [id](const Slot &s) { return s.id == id; });
}

void Checker::changed() {
    const std::vector<Slot> copy = _observers; // an observer may unobserve
    for (const Slot &s : copy)
        if (s.fn)
            s.fn();
}

std::vector<std::string> Checker::defaultLanguages(
    const std::vector<Language> &available, const std::vector<std::string> &preferred
) {
    const auto norm = [](std::string c) {
        std::replace(c.begin(), c.end(), '-', '_');
        return c;
    };
    const std::string sys  = preferred.empty() ? std::string() : detail::localeCode(preferred[0]);
    const std::string lang = sys.substr(0, sys.find('_'));
    if (!sys.empty()) {
        for (const Language &l : available)
            if (norm(l.code) == sys)
                return {l.code};
        for (const Language &l : available) {
            const std::string c = norm(l.code);
            if (c.substr(0, c.find('_')) == lang)
                return {l.code};
        }
    }
    if (!available.empty())
        return {available.front().code};
    return {};
}

void Checker::setBackendForTesting(plat::App &app, std::unique_ptr<Backend> backend) {
    _app = &app;
    reset();
    if (backend) {
        _state          = std::make_shared<State>();
        _state->backend = std::move(backend);
        _backend        = _state->backend.get();
    }
    changed();
}

void listLanguages(plat::App &app, std::function<void(Available)> done) {
    auto out = std::make_shared<Available>();
    auto run = [out, preferred = app.preferredLanguages()] {
        out->languages = detail::availableLanguages();
        if (out->languages.empty())
            out->packageHint = detail::dictionaryPackageHint(preferred);
    };
    auto then = [out, done = std::move(done)] { done(std::move(*out)); };
    if (detail::listsOffThread())
        return model::runInBackground(app, std::move(run), std::move(then));
    app.post([run = std::move(run), then = std::move(then)]() mutable {
        run();
        then();
    });
}

} // namespace spell
