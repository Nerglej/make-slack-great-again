// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "spell/spell_backend.h"

#import <AppKit/AppKit.h>

// macOS: NSSpellChecker — the system's languages, and the words the user
// taught it in any app ("Learn Spelling"), which "Add to dictionary" adds to.
// Every call is an XPC round trip to the spelling service, hence the checker's
// per-word cache. Main thread only. Manual retain/release (no ARC), like the
// other .mm files in this tree.

namespace Spell {
namespace {

class MacBackend : public Backend {
public:
    MacBackend() : _tag([NSSpellChecker uniqueSpellDocumentTag]) {}
    ~MacBackend() override {
        [[NSSpellChecker sharedSpellChecker] closeSpellDocumentWithTag:_tag];
    }

    bool load(const QStringList &codes) override {
        @autoreleasepool {
            NSArray<NSString *> *avail = [[NSSpellChecker sharedSpellChecker] availableLanguages];
            for (const QString &code : codes)
                if ([avail containsObject:code.toNSString()])
                    _langs << code;
        }
        return !_langs.isEmpty();
    }

    bool check(const QString &word) override {
        @autoreleasepool {
            NSSpellChecker *sc = [NSSpellChecker sharedSpellChecker];
            NSString       *w  = word.toNSString();
            for (const QString &lang : _langs) {
                const NSRange bad = [sc checkSpellingOfString:w
                                                   startingAt:0
                                                     language:lang.toNSString()
                                                         wrap:NO
                                       inSpellDocumentWithTag:_tag
                                                    wordCount:nullptr];
                if (bad.location == NSNotFound)
                    return true;
            }
        }
        return false;
    }

    QStringList suggest(const QString &word, int max) override {
        QStringList out;
        @autoreleasepool {
            NSSpellChecker *sc = [NSSpellChecker sharedSpellChecker];
            NSString       *w  = word.toNSString();
            for (const QString &lang : _langs) {
                NSArray<NSString *> *guesses = [sc guessesForWordRange:NSMakeRange(0, w.length)
                                                              inString:w
                                                              language:lang.toNSString()
                                                inSpellDocumentWithTag:_tag];
                for (NSString *g in guesses) {
                    const QString q = QString::fromNSString(g);
                    if (!out.contains(q))
                        out << q;
                    if (out.size() >= max)
                        return out;
                }
            }
        }
        return out;
    }

    void addToDictionary(const QString &word) override {
        @autoreleasepool {
            [[NSSpellChecker sharedSpellChecker] learnWord:word.toNSString()];
        }
    }

private:
    NSInteger   _tag;
    QStringList _langs;
};

} // namespace

std::unique_ptr<Backend> createPlatformBackend() {
    return std::make_unique<MacBackend>();
}

QList<Language> availableLanguages() {
    QList<Language> out;
    @autoreleasepool {
        for (NSString *l in [[NSSpellChecker sharedSpellChecker] availableLanguages]) {
            const QString code = QString::fromNSString(l);
            out.append({code, languageName(code)});
        }
    }
    return out;
}

QString dictionaryPackageHint() {
    return {};
}

} // namespace Spell
