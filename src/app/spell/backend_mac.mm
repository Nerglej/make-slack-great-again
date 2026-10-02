// macOS: NSSpellChecker — the system's languages, and the words the user
// taught it in any app ("Learn Spelling"), which "Add to dictionary" adds to.
// Every call is an XPC round trip to the spelling service, hence the
// checker's per-word cache. Main thread only. Manual retain/release (no ARC),
// like plat's .mm files.
#include "app/spell/spell.h"
#include "app/spell/spell_internal.h"

#import <AppKit/AppKit.h>

#include <algorithm>

namespace spell {

namespace {

NSString *ns(std::string_view s) {
    return [[[NSString alloc] initWithBytes:s.data() length:s.size()
                                   encoding:NSUTF8StringEncoding] autorelease];
}

class MacBackend : public Backend {
public:
    MacBackend() : _tag([NSSpellChecker uniqueSpellDocumentTag]) {}
    ~MacBackend() override { [[NSSpellChecker sharedSpellChecker] closeSpellDocumentWithTag:_tag]; }

    bool load(const std::vector<std::string> &codes) override {
        @autoreleasepool {
            NSArray<NSString *> *avail = [[NSSpellChecker sharedSpellChecker] availableLanguages];
            for (const std::string &code : codes)
                if ([avail containsObject:ns(code)])
                    _langs.push_back(code);
        }
        return !_langs.empty();
    }

    bool check(std::string_view word) override {
        @autoreleasepool {
            NSSpellChecker *sc = [NSSpellChecker sharedSpellChecker];
            NSString       *w  = ns(word);
            if (!w)
                return true;
            for (const std::string &lang : _langs) {
                const NSRange bad = [sc checkSpellingOfString:w
                                                   startingAt:0
                                                     language:ns(lang)
                                                         wrap:NO
                                       inSpellDocumentWithTag:_tag
                                                    wordCount:nullptr];
                if (bad.location == NSNotFound)
                    return true;
            }
        }
        return false;
    }

    std::vector<std::string> suggest(std::string_view word, int max) override {
        std::vector<std::string> out;
        @autoreleasepool {
            NSSpellChecker *sc = [NSSpellChecker sharedSpellChecker];
            NSString       *w  = ns(word);
            if (!w)
                return out;
            for (const std::string &lang : _langs) {
                NSArray<NSString *> *guesses = [sc guessesForWordRange:NSMakeRange(0, w.length)
                                                              inString:w
                                                              language:ns(lang)
                                                inSpellDocumentWithTag:_tag];
                for (NSString *g in guesses) {
                    std::string s(g.UTF8String ? g.UTF8String : "");
                    if (!s.empty() && std::find(out.begin(), out.end(), s) == out.end())
                        out.push_back(std::move(s));
                    if (int(out.size()) >= max)
                        return out;
                }
            }
        }
        return out;
    }

    void addToDictionary(std::string_view word) override {
        @autoreleasepool {
            if (NSString *w = ns(word))
                [[NSSpellChecker sharedSpellChecker] learnWord:w];
        }
    }

private:
    NSInteger                _tag;
    std::vector<std::string> _langs;
};

} // namespace

std::unique_ptr<Backend> createPlatformBackend(plat::App &) {
    return std::make_unique<MacBackend>();
}

namespace detail {

bool listsOffThread() {
    return false;
}

std::vector<Language> availableLanguages() {
    std::vector<Language> out;
    @autoreleasepool {
        for (NSString *l in [[NSSpellChecker sharedSpellChecker] availableLanguages]) {
            const std::string code(l.UTF8String ? l.UTF8String : "");
            if (!code.empty())
                out.push_back({code, languageName(code)});
        }
    }
    return out;
}

std::string dictionaryPackageHint(const std::vector<std::string> &) {
    return {};
}

} // namespace detail

} // namespace spell
