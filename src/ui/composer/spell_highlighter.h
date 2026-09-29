// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Vladimir Osipov
#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <utility>

class QTextEdit;

// Squiggly underlines under the misspelled words of a composer (see
// Spell::Checker; nothing is checked while it is off). A highlighter only sets
// the layout's extra formats, never the text's own char formats, so mention
// pills, drafts and the sent text never see it and it adds no undo steps.
// What counts as a word is Spell::checkableWords'; on top of that, fragments
// whose char format carries `skipProperty` (mention pills) are left alone, and
// the word the cursor is in waits until the cursor leaves it — a word being
// typed isn't wrong yet.
class SpellHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    SpellHighlighter(QTextEdit *edit, int skipProperty);

    // The [start, end) document range of the underlined word at document
    // position `pos` (inside it or at either end); {-1, -1} when there is none.
    std::pair<int, int> misspelledRangeAt(int pos) const;

protected:
    void highlightBlock(const QString &text) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void                applyTheme();
    void                onCursorMoved();
    // The document range of the word the cursor is in while the editor has
    // focus; {-1, -1} otherwise.
    std::pair<int, int> cursorWord() const;
    // Re-check every block, without the editor reporting a text change.
    void                recheckAll();
    // Re-check the block holding document position `pos`, from the event loop
    // (cursor moves can arrive in the middle of a document change).
    void                recheckAt(int pos);

    QTextEdit          *_edit;
    int                 _skipProperty;
    QTextCharFormat     _format;
    std::pair<int, int> _cursorWord{-1, -1};
};
