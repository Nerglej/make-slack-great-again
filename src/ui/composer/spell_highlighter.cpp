// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#include "spell_highlighter.h"
#include "spell/spell_checker.h"
#include "spell/spell_words.h"
#include "ui/theme.h"
#include "ui/theme_manager.h"

#include <QEvent>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>

// Block state: 1 = the block ends inside a ``` fence, so the next one starts
// in code; anything else = it doesn't.
static constexpr int kInFenceState = 1;

// The block's fragments that carry `property` (mention pills), as spans of the
// block's text.
static QList<Spell::Span> skippedRanges(const QTextBlock &block, int property) {
    QList<Spell::Span> out;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment frag = it.fragment();
        if (frag.isValid() && frag.charFormat().hasProperty(property))
            out.append({frag.position() - block.position(), frag.length()});
    }
    return out;
}

SpellHighlighter::SpellHighlighter(QTextEdit *edit, int skipProperty)
    : QSyntaxHighlighter(edit->document()), _edit(edit), _skipProperty(skipProperty) {
    applyTheme();
    connect(&Spell::Checker::instance(), &Spell::Checker::changed, this, [this] {
        recheckAll(); // also clears every underline when the checker went off
    });
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        applyTheme();
        if (Spell::Checker::instance().isActive())
            recheckAll();
    });
    connect(edit, &QTextEdit::cursorPositionChanged, this, &SpellHighlighter::onCursorMoved);
    edit->installEventFilter(this); // focus in/out moves the "being typed" word
}

void SpellHighlighter::applyTheme() {
    _format.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
    _format.setUnderlineColor(Th::c().text.danger);
}

void SpellHighlighter::highlightBlock(const QString &text) {
    auto &checker = Spell::Checker::instance();
    if (!checker.isActive())
        return;
    const QTextBlock block   = currentBlock();
    bool             inFence = previousBlockState() == kInFenceState;
    const auto words = Spell::checkableWords(text, inFence, skippedRanges(block, _skipProperty));
    setCurrentBlockState(inFence ? kInFenceState : 0);

    const auto typing = cursorWord();
    for (const Spell::Span &w : words) {
        const int start = block.position() + w.start;
        if (start == typing.first)
            continue;
        if (checker.isMisspelled(text.mid(w.start, w.length)))
            setFormat(w.start, w.length, _format);
    }
}

std::pair<int, int> SpellHighlighter::misspelledRangeAt(int pos) const {
    auto &checker = Spell::Checker::instance();
    if (!checker.isActive())
        return {-1, -1};
    const QTextBlock block = document()->findBlock(pos);
    if (!block.isValid())
        return {-1, -1};
    const QTextBlock prev    = block.previous();
    bool             inFence = prev.isValid() && prev.userState() == kInFenceState;
    const QString    text    = block.text();
    for (const Spell::Span &w :
         Spell::checkableWords(text, inFence, skippedRanges(block, _skipProperty))) {
        const int start = block.position() + w.start;
        if (pos >= start && pos <= start + w.length &&
            checker.isMisspelled(text.mid(w.start, w.length)))
            return {start, start + w.length};
    }
    return {-1, -1};
}

std::pair<int, int> SpellHighlighter::cursorWord() const {
    if (!_edit->hasFocus())
        return {-1, -1};
    const QTextCursor cursor = _edit->textCursor();
    if (cursor.hasSelection())
        return {-1, -1};
    const QTextBlock  block = cursor.block();
    const Spell::Span w     = Spell::wordAt(block.text(), cursor.positionInBlock());
    if (w.length == 0)
        return {-1, -1};
    return {block.position() + w.start, block.position() + w.end()};
}

void SpellHighlighter::onCursorMoved() {
    const auto now = cursorWord();
    // Typing on inside the same word only moves its end: nothing to redo.
    if (now.first == _cursorWord.first) {
        _cursorWord = now;
        return;
    }
    const auto before = _cursorWord;
    _cursorWord       = now;
    if (!Spell::Checker::instance().isActive())
        return;
    if (before.first >= 0)
        recheckAt(before.first); // the word just left may be wrong now
    if (now.first >= 0 && (before.first < 0 ||
                           document()->findBlock(now.first) != document()->findBlock(before.first)))
        recheckAt(now.first); // and the one entered isn't until it's left
}

void SpellHighlighter::recheckAll() {
    // QSyntaxHighlighter re-formats inside an edit block, and closing it
    // makes the editor emit textChanged although no text changed — which the
    // composer (and anyone watching it) would take for typing.
    const QSignalBlocker quiet(_edit);
    rehighlight();
}

void SpellHighlighter::recheckAt(int pos) {
    // A QTextCursor follows the text through any edit made before this runs.
    QTextCursor at(document());
    at.setPosition(std::min(pos, document()->characterCount() - 1));
    QMetaObject::invokeMethod(
        this,
        [this, at] {
            if (at.isNull())
                return;
            const QSignalBlocker quiet(_edit); // see recheckAll
            rehighlightBlock(at.block());
        },
        Qt::QueuedConnection
    );
}

bool SpellHighlighter::eventFilter(QObject *obj, QEvent *event) {
    if (obj == _edit && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
        QMetaObject::invokeMethod(this, &SpellHighlighter::onCursorMoved, Qt::QueuedConnection);
    return QSyntaxHighlighter::eventFilter(obj, event);
}
