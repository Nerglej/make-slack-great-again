// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
//
// Spell checking: what the composer checks (Spell::checkableWords), the
// checker's cache and word lists, the Hunspell backend against the system's
// en_US dictionary (skipped where it isn't installed), and the composer's
// underlines and context menu against a fake backend.

#include <catch2/catch_test_macros.hpp>

#include "test_main.h"

#include "spell/spell_backend.h"
#include "spell/spell_checker.h"
#include "spell/spell_words.h"
#include "ui/composer/composer_widget.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QMenu>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextLayout>

using namespace Qt::StringLiterals;

MSGA_TEST_MAIN(argc, argv) {
    QApplication app(argc, argv);
    app.setApplicationName("msga-test-spell-check");
    app.setOrganizationName("msga-test");
    // The checker reads and writes QSettings("msga", "msga"); the Hunspell
    // backend keeps added words under AppDataLocation.
    QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsDir.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QStandardPaths::setTestModeEnabled(true);
    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).removeRecursively();
    return msga_test::runCatch(argc, argv);
}

namespace {

// The words checkableWords finds, as text.
QStringList words(const QString &line, bool inFence = false, const QList<Spell::Span> &ex = {}) {
    QStringList out;
    for (const Spell::Span &s : Spell::checkableWords(line, inFence, ex))
        out << line.mid(s.start, s.length);
    return out;
}

// Knows every word but `bad`; suggests from `suggestions`.
struct FakeBackend : Spell::Backend {
    QSet<QString>               bad;
    QHash<QString, QStringList> suggestions;
    QStringList                 added;
    int                         checks = 0;

    bool load(const QStringList &) override { return true; }
    bool check(const QString &w) override {
        ++checks;
        return !bad.contains(w) || added.contains(w);
    }
    QStringList suggest(const QString &w, int max) override {
        return suggestions.value(w).mid(0, max);
    }
    void addToDictionary(const QString &w) override { added << w; }
};

// Installs a fake as the checker's backend for one test, and takes it away
// again (the checker is a process-wide singleton).
struct FakeChecker {
    FakeBackend *fake;
    FakeChecker(std::initializer_list<QString> bad) {
        auto f = std::make_unique<FakeBackend>();
        f->bad = QSet<QString>(bad);
        fake   = f.get();
        Spell::Checker::instance().setBackendForTesting(std::move(f));
    }
    ~FakeChecker() { Spell::Checker::instance().setBackendForTesting(nullptr); }
};

QTextEdit *editOf(ComposerWidget &c) {
    return c.findChild<QTextEdit *>("composerEdit");
}

// The underlined runs of the editor's first block, as text.
QStringList underlined(QTextEdit *ed, int blockNumber = 0) {
    QStringList      out;
    const QTextBlock block = ed->document()->findBlockByNumber(blockNumber);
    for (const QTextLayout::FormatRange &r : block.layout()->formats())
        if (r.format.underlineStyle() == QTextCharFormat::SpellCheckUnderline)
            out << block.text().mid(r.start, r.length);
    return out;
}

// Types into a shown, focused composer so the cursor rules apply.
struct ShownComposer {
    ComposerWidget c;
    QTextEdit     *ed = nullptr;
    ShownComposer() {
        c.resize(500, 160);
        c.show();
        c.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&c));
        ed = editOf(c);
        ed->setFocus();
        QCoreApplication::processEvents();
    }
    void type(const QString &text) {
        QTest::keyClicks(ed, text);
        QCoreApplication::processEvents();
    }
};

} // namespace

// ── What gets checked ─────────────────────────────────────────────────────────

TEST_CASE("prose words are checked, in order", "[spell][words]") {
    CHECK(
        words(u"Helo wrold, how are you?"_s) ==
        QStringList{u"Helo"_s, u"wrold"_s, u"how"_s, u"are"_s, u"you"_s}
    );
}

TEST_CASE("an apostrophe between letters stays in the word", "[spell][words]") {
    CHECK(words(u"don't won’t"_s) == QStringList{u"don't"_s, u"won’t"_s});
    CHECK(words(u"'quoted' James'"_s) == QStringList{u"quoted"_s, u"James"_s});
}

TEST_CASE("markdown markers around a word are not part of it", "[spell][words]") {
    CHECK(
        words(u"*bold* _it_ ~gone~ (paren)"_s) ==
        QStringList{u"bold"_s, u"it"_s, u"gone"_s, u"paren"_s}
    );
}

TEST_CASE("code is never checked", "[spell][words]") {
    CHECK(words(u"see `fooo barr` here"_s) == QStringList{u"see"_s, u"here"_s});
    CHECK(words(u"a lone ` tick stays"_s) == QStringList{u"lone"_s, u"tick"_s, u"stays"_s});
    CHECK(words(u"one ```xyzzy``` two"_s) == QStringList{u"one"_s, u"two"_s});
}

TEST_CASE("a code fence carries over to the next lines", "[spell][words]") {
    bool fence = false;
    CHECK(words(u"before ```qwrt"_s, fence) == QStringList{u"before"_s});
    Spell::checkableWords(u"before ```qwrt"_s, fence);
    CHECK(fence);
    bool inside = true;
    CHECK(Spell::checkableWords(u"zzkx vvqq"_s, inside).isEmpty());
    CHECK(inside);
    bool closing = true;
    CHECK(words(u"zzkx``` after"_s, closing) == QStringList{u"after"_s});
    Spell::checkableWords(u"zzkx``` after"_s, closing);
    CHECK_FALSE(closing);
}

TEST_CASE("links, tokens and emoji are never checked", "[spell][words]") {
    CHECK(words(u"go https://exmaple.com/pth?q=1 now"_s) == QStringList{u"go"_s, u"now"_s});
    CHECK(words(u"www.exmaple.org ok"_s) == QStringList{u"ok"_s});
    CHECK(words(u"hi <@U123ABC> and <https://x.io|lnk>"_s) == QStringList{u"hi"_s, u"and"_s});
    CHECK(words(u"nice :thumbsup: :skin-tone-2: yes"_s) == QStringList{u"nice"_s, u"yes"_s});
}

TEST_CASE("mentions, channels, commands, paths and hosts are skipped", "[spell][words]") {
    CHECK(words(u"ask @jdoe in #genral"_s) == QStringList{u"ask"_s, u"in"_s});
    CHECK(words(u"mail me@exmaple.com"_s) == QStringList{u"mail"_s});
    CHECK(words(u"/remnd me"_s) == QStringList{u"me"_s});
    CHECK(words(u"open C:\\Users\\bob or msga.app"_s) == QStringList{u"open"_s, u"or"_s});
}

TEST_CASE("identifiers, acronyms and single letters are skipped", "[spell][words]") {
    CHECK(words(u"mp3 v2beta snake_case HTTP camelCase iPhone a Ok"_s) == QStringList{u"Ok"_s});
}

TEST_CASE("excluded ranges read as whitespace", "[spell][words]") {
    // "@Jane Doe" is a mention pill: neither half is checked, and the word
    // after it is not glued to it.
    const QString line = u"hey @Jane Doe thx"_s;
    CHECK(words(line, false, {{4, 9}}) == QStringList{u"hey"_s, u"thx"_s});
}

TEST_CASE("wordAt finds the word around a cursor", "[spell][words]") {
    const QString line = u"say don't now"_s;
    CHECK(Spell::wordAt(line, 4) == Spell::Span{4, 5});
    CHECK(Spell::wordAt(line, 9) == Spell::Span{4, 5}); // at its end
    CHECK(Spell::wordAt(line, 6) == Spell::Span{4, 5}); // at the apostrophe
    CHECK(Spell::wordAt(u"a  b"_s, 2).length == 0);
}

// ── The checker ───────────────────────────────────────────────────────────────

TEST_CASE("the checker is off by default and checks nothing", "[spell][checker]") {
    auto &checker = Spell::Checker::instance();
    CHECK_FALSE(checker.enabled());
    CHECK_FALSE(checker.isActive());
    CHECK_FALSE(checker.isMisspelled(u"wrold"_s));
    CHECK(checker.suggestions(u"wrold"_s).isEmpty());
}

TEST_CASE("the checker caches each word's answer", "[spell][checker]") {
    FakeChecker fc{u"wrold"_s};
    auto       &checker = Spell::Checker::instance();
    CHECK(checker.isMisspelled(u"wrold"_s));
    CHECK(checker.isMisspelled(u"wrold"_s));
    CHECK_FALSE(checker.isMisspelled(u"world"_s));
    CHECK(fc.fake->checks == 2);
}

TEST_CASE("a curly apostrophe is checked as a straight one", "[spell][checker]") {
    FakeChecker fc{u"dont't"_s};
    CHECK(Spell::Checker::instance().isMisspelled(u"dont’t"_s));
}

TEST_CASE("ignore and add to dictionary stop the underline", "[spell][checker]") {
    // Ignoring lasts for the whole run: a word no other test uses.
    FakeChecker fc{u"worlld"_s, u"msgaa"_s};
    auto       &checker = Spell::Checker::instance();
    QSignalSpy  changed(&checker, &Spell::Checker::changed);
    checker.ignore(u"worlld"_s);
    CHECK_FALSE(checker.isMisspelled(u"worlld"_s));
    CHECK(checker.isMisspelled(u"msgaa"_s));
    checker.addToDictionary(u"msgaa"_s);
    CHECK(fc.fake->added == QStringList{u"msgaa"_s});
    CHECK_FALSE(checker.isMisspelled(u"msgaa"_s));
    CHECK(changed.count() == 2);
}

TEST_CASE("the default language follows the system locale", "[spell][checker]") {
    const QString sys  = QLocale::system().name();
    const QString lang = sys.section(u'_', 0, 0);
    CHECK(Spell::Checker::defaultLanguages({{u"zz_ZZ"_s, {}}, {sys, {}}}) == QStringList{sys});
    CHECK(
        Spell::Checker::defaultLanguages({{u"zz_ZZ"_s, {}}, {lang + u"_XX"_s, {}}}) ==
        QStringList{lang + u"_XX"_s}
    );
    CHECK(Spell::Checker::defaultLanguages({{u"zz_ZZ"_s, {}}}) == QStringList{u"zz_ZZ"_s});
    CHECK(Spell::Checker::defaultLanguages({}).isEmpty());
}

TEST_CASE("language names are the language's own", "[spell][checker]") {
    CHECK(Spell::languageName(u"de_DE"_s) == u"Deutsch (de_DE)"_s);
    CHECK(Spell::languageName(u"ru-RU"_s) == u"Русский (ru-RU)"_s);
    CHECK(Spell::languageName(u"de_DE_frami"_s) == u"Deutsch (de_DE_frami)"_s);
    CHECK(Spell::languageName(u"qqq"_s) == u"qqq"_s);
}

TEST_CASE("configure persists the choice and off frees the backend", "[spell][checker]") {
    auto &checker = Spell::Checker::instance();
    checker.configure(true, {u"xx_NONE"_s}); // no such dictionary: nothing loads
    CHECK(QSettings(u"msga"_s, u"msga"_s).value(Spell::Checker::kEnabledKey).toBool());
    CHECK(
        QSettings(u"msga"_s, u"msga"_s).value(Spell::Checker::kLanguagesKey).toStringList() ==
        QStringList{u"xx_NONE"_s}
    );
    QTRY_VERIFY_WITH_TIMEOUT(!checker.isActive(), 2000);
    checker.configure(false, {u"xx_NONE"_s});
    CHECK_FALSE(checker.enabled());
    CHECK_FALSE(checker.isActive());
    CHECK_FALSE(QSettings(u"msga"_s, u"msga"_s).value(Spell::Checker::kEnabledKey).toBool());
}

// ── Hunspell (Linux) ──────────────────────────────────────────────────────────

#if defined(Q_OS_LINUX)

namespace {

bool hasSystemEnUs() {
    for (const Spell::Language &l : Spell::availableLanguages())
        if (l.code == u"en_US"_s)
            return true;
    return false;
}

void writeFile(const QString &path, const QByteArray &bytes) {
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
}

} // namespace

TEST_CASE("Hunspell checks and suggests with the system en_US dictionary", "[spell][hunspell]") {
    if (!hasSystemEnUs())
        SKIP("no en_US Hunspell dictionary installed (hunspell-en-us)");
    auto backend = Spell::createPlatformBackend();
    REQUIRE(backend->loadsOffThread());
    REQUIRE(backend->load({u"en_US"_s}));
    CHECK(backend->check(u"hello"_s));
    CHECK(backend->check(u"Hello"_s));
    CHECK(backend->check(u"don't"_s));
    CHECK_FALSE(backend->check(u"helo"_s));
    CHECK_FALSE(backend->check(u"wrold"_s));
    const QStringList s = backend->suggest(u"wrold"_s, 5);
    CHECK(s.contains(u"world"_s));
    CHECK(s.size() <= 5);
}

TEST_CASE("Hunspell keeps added words across loads", "[spell][hunspell]") {
    if (!hasSystemEnUs())
        SKIP("no en_US Hunspell dictionary installed (hunspell-en-us)");
    {
        auto backend = Spell::createPlatformBackend();
        REQUIRE(backend->load({u"en_US"_s}));
        CHECK_FALSE(backend->check(u"msgaword"_s));
        backend->addToDictionary(u"msgaword"_s);
        CHECK(backend->check(u"msgaword"_s));
    }
    auto again = Spell::createPlatformBackend();
    REQUIRE(again->load({u"en_US"_s}));
    CHECK(again->check(u"msgaword"_s));
}

TEST_CASE("Hunspell reads 8-bit dictionaries through their SET", "[spell][hunspell]") {
    // Two tiny dictionaries on $DICPATH, as ISO-8859-1 and KOI8-R bytes.
    QTemporaryDir dir;
    writeFile(dir.filePath(u"xx_LATIN.aff"_s), "SET ISO8859-1\nTRY esartn\n");
    writeFile(
        dir.filePath(u"xx_LATIN.dic"_s),
        "2\nstra\xdf"
        "e\nf\xfcr\n"
    );
    writeFile(dir.filePath(u"xx_KOI.aff"_s), "SET KOI8-R\nTRY \xd4\xc5\n");
    // "привет" in KOI8-R.
    writeFile(dir.filePath(u"xx_KOI.dic"_s), "1\n\xd0\xd2\xc9\xd7\xc5\xd4\n");
    const QByteArray oldPath = qgetenv("DICPATH");
    qputenv("DICPATH", QFile::encodeName(dir.path()));

    bool latin = false, koi = false;
    for (const Spell::Language &l : Spell::availableLanguages()) {
        latin = latin || l.code == u"xx_LATIN"_s;
        koi   = koi || l.code == u"xx_KOI"_s;
    }
    CHECK(latin);
    CHECK(koi);

    auto backend = Spell::createPlatformBackend();
    REQUIRE(backend->load({u"xx_LATIN"_s, u"xx_KOI"_s}));
    CHECK(backend->check(u"straße"_s));
    CHECK(backend->check(u"für"_s));
    CHECK(backend->check(u"привет"_s));
    CHECK_FALSE(backend->check(u"strase"_s));
    CHECK_FALSE(backend->check(u"прывет"_s));
    CHECK_FALSE(backend->check(u"日本"_s)); // in neither set
    CHECK(backend->suggest(u"приветт"_s, 5).contains(u"привет"_s));
    CHECK(backend->suggest(u"strasse"_s, 5).contains(u"straße"_s));

    if (oldPath.isNull())
        qunsetenv("DICPATH");
    else
        qputenv("DICPATH", oldPath);
}

#endif

// ── The system checker (macOS, Windows) ───────────────────────────────────────

#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)

TEST_CASE("the system spell checker checks and suggests English", "[spell][native]") {
    // Any English the OS offers: "en" / "en_US" (AppKit), "en-US" (Windows).
    QString english;
    for (const Spell::Language &l : Spell::availableLanguages())
        if (l.code.startsWith(u"en"_s) && (english.isEmpty() || l.code.size() < english.size()))
            english = l.code;
    if (english.isEmpty())
        SKIP("the system offers no English spell checking");
    auto backend = Spell::createPlatformBackend();
    REQUIRE_FALSE(backend->loadsOffThread());
    REQUIRE(backend->load({english}));
    CHECK(backend->check(u"hello"_s));
    CHECK(backend->check(u"don't"_s));
    CHECK_FALSE(backend->check(u"wrold"_s));
    const QStringList s = backend->suggest(u"wrold"_s, 5);
    CHECK(s.contains(u"world"_s));
    CHECK(s.size() <= 5);
    CHECK_FALSE(Spell::createPlatformBackend()->load({u"xx-NONE"_s}));
}

#endif

// ── The composer ──────────────────────────────────────────────────────────────

TEST_CASE("nothing is underlined while spell checking is off", "[spell][composer]") {
    ComposerWidget c;
    c.setText(u"teh wrold"_s);
    CHECK(underlined(editOf(c)).isEmpty());
}

TEST_CASE("misspelled words are underlined once checking is on", "[spell][composer]") {
    ComposerWidget c;
    c.setText(u"teh cat wrold"_s);
    FakeChecker fc{u"teh"_s, u"wrold"_s};
    CHECK(underlined(editOf(c)) == QStringList{u"teh"_s, u"wrold"_s});
    Spell::Checker::instance().setBackendForTesting(nullptr); // turned off
    CHECK(underlined(editOf(c)).isEmpty());
}

TEST_CASE("mention pills are never checked and still send as tokens", "[spell][composer]") {
    // The pill shows "@Jane Wrold": without the pill rule "Wrold" would be
    // checked as a word of its own.
    FakeChecker    fc{u"Wrold"_s, u"teh"_s};
    ComposerWidget c;
    const QString  text = u"<@U0ABCDEF|Jane Wrold> teh"_s;
    c.setText(text);
    CHECK(editOf(c)->toPlainText() == u"@Jane Wrold teh"_s);
    CHECK(underlined(editOf(c)) == QStringList{u"teh"_s});
    CHECK(c.currentText() == text);
    const ComposerDraft d = c.takeDraft();
    CHECK(d.text == text);
    c.restoreDraft(d);
    CHECK(c.currentText() == text);
    CHECK(underlined(editOf(c)) == QStringList{u"teh"_s});
}

TEST_CASE("re-checking changes neither the text nor the undo history", "[spell][composer]") {
    ComposerWidget c;
    QTextEdit     *ed = editOf(c);
    c.setText(u"teh wrold"_s);
    const int  undoSteps = ed->document()->availableUndoSteps();
    QSignalSpy textChanged(ed, &QTextEdit::textChanged);
    {
        FakeChecker fc{u"teh"_s, u"wrold"_s};
        CHECK(underlined(ed).size() == 2);
    }
    CHECK(textChanged.count() == 0);
    CHECK(ed->document()->availableUndoSteps() == undoSteps);
}

TEST_CASE("the word being typed is underlined once the cursor leaves it", "[spell][composer]") {
    FakeChecker   fc{u"teh"_s, u"cta"_s};
    ShownComposer s;
    s.type(u"teh"_s);
    CHECK(underlined(s.ed).isEmpty()); // still being typed
    s.type(u" cta"_s);
    CHECK(underlined(s.ed) == QStringList{u"teh"_s});
    s.type(u" "_s);
    CHECK(underlined(s.ed) == QStringList{u"teh"_s, u"cta"_s});
}

TEST_CASE("a code block spanning lines is not checked", "[spell][composer]") {
    FakeChecker    fc{u"teh"_s, u"qwrt"_s};
    ComposerWidget c;
    c.setText(u"teh\n```\nqwrt teh\n```\nteh"_s);
    QTextEdit *ed = editOf(c);
    CHECK(underlined(ed, 0) == QStringList{u"teh"_s});
    CHECK(underlined(ed, 2).isEmpty());
    CHECK(underlined(ed, 4) == QStringList{u"teh"_s});
}

TEST_CASE(
    "the context menu offers suggestions and replaces in one undo step", "[spell][composer]"
) {
    FakeChecker fc{u"teh"_s};
    fc.fake->suggestions.insert(u"teh"_s, {u"the"_s, u"ten"_s, u"tea"_s});
    ComposerWidget c;
    c.resize(500, 160);
    c.show();
    QTextEdit *ed = editOf(c);
    c.setText(u"teh cat"_s);
    REQUIRE(underlined(ed) == QStringList{u"teh"_s});

    QTextCursor at(ed->document());
    at.setPosition(1);
    const QPoint      pos = ed->cursorRect(at).center();
    QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, ed->viewport()->mapToGlobal(pos));
    QApplication::sendEvent(ed->viewport(), &ev);
    auto *menu = ed->findChild<QMenu *>();
    REQUIRE(menu);
    const QList<QAction *> acts = menu->actions();
    REQUIRE(acts.size() > 6);
    CHECK(acts[0]->text() == u"the"_s);
    CHECK(acts[1]->text() == u"ten"_s);
    CHECK(acts[2]->text() == u"tea"_s);
    CHECK(acts[3]->isSeparator());
    CHECK(acts[4]->text() == ComposerWidget::tr("Add to dictionary"));
    CHECK(acts[5]->text() == ComposerWidget::tr("Ignore"));
    CHECK(acts[6]->isSeparator());

    acts[0]->trigger();
    menu->close();
    CHECK(c.currentText() == u"the cat"_s);
    CHECK(underlined(ed).isEmpty());
    ed->document()->undo();
    CHECK(c.currentText() == u"teh cat"_s);
}

TEST_CASE("the context menu's Ignore stops underlining the word", "[spell][composer]") {
    FakeChecker    fc{u"wrold"_s};
    ComposerWidget c;
    c.resize(500, 160);
    c.show();
    QTextEdit *ed = editOf(c);
    c.setText(u"hi wrold"_s);
    QTextCursor at(ed->document());
    at.setPosition(5);
    const QPoint      pos = ed->cursorRect(at).center();
    QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, ed->viewport()->mapToGlobal(pos));
    QApplication::sendEvent(ed->viewport(), &ev);
    auto *menu = ed->findChild<QMenu *>();
    REQUIRE(menu);
    CHECK_FALSE(menu->actions()[0]->isEnabled()); // "No spelling suggestions"
    QAction *ignore = nullptr;
    for (QAction *a : menu->actions())
        if (a->text() == ComposerWidget::tr("Ignore"))
            ignore = a;
    REQUIRE(ignore);
    ignore->trigger();
    menu->close();
    CHECK(underlined(ed).isEmpty());
    CHECK(c.currentText() == u"hi wrold"_s);
}

TEST_CASE("a correctly spelled word gets the standard menu only", "[spell][composer]") {
    FakeChecker    fc{u"wrold"_s};
    ComposerWidget c;
    c.resize(500, 160);
    c.show();
    QTextEdit *ed = editOf(c);
    c.setText(u"hello"_s);
    QTextCursor at(ed->document());
    at.setPosition(2);
    const QPoint      pos = ed->cursorRect(at).center();
    QContextMenuEvent ev(QContextMenuEvent::Mouse, pos, ed->viewport()->mapToGlobal(pos));
    QApplication::sendEvent(ed->viewport(), &ev);
    auto *menu = ed->findChild<QMenu *>();
    REQUIRE(menu);
    for (QAction *a : menu->actions())
        CHECK(a->text() != ComposerWidget::tr("Add to dictionary"));
    menu->close();
}
